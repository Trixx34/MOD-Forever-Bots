// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the group etiquette (src/group_etiquette.h): off by default, the wait (healer, tank, straggler, caps), the chat limits, the roll rule, and the policy
// wiring (a route party's leader stands while the healer is dry, says "ready?" after). No core needed.
#include "advisor_text.h"
#include "bot_policy.h"
#include "group_etiquette.h"

#include <cstdio>

using namespace group_etiquette;
using forever_module::BotSnapshot;
using forever_module::WorldTickSnapshotView;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

Settings On()
{
    Settings s;
    s.Enabled = true;
    s.Loot = true;
    return s;
}

Member Healer(std::uint32_t mana) { Member m; m.Bot = 2; m.Healer = true; m.ManaUser = true; m.ManaPct = mana; return m; }
Member Tank(std::uint32_t hp) { Member m; m.Bot = 1; m.Tank = true; m.HealthPct = hp; return m; }

void OffByDefault()
{
    Waiter waiter;
    CHECK(!Settings{}.Enabled && !Settings{}.Loot);
    CHECK(!waiter.Holding(Settings{}, { Healer(1) }, false, 0));
    Chatter chat;
    Line line;
    ChatFacts facts;
    facts.InDungeonParty = true;
    facts.GroupSize = 5;
    CHECK(!chat.Note(Settings{}, facts, 1, 1, 1, 100000, line));
    RollFacts roll;
    roll.Usable = true;
    roll.Quality = 3;
    roll.Upgrade = 500;
    CHECK(DecideRoll(Settings{}, roll) == Roll::Pass);
}

void WaitsForHealerAndTank()
{
    CHECK(Evaluate(On(), { Tank(100), Healer(79) }) == Hold::Healer);
    CHECK(Evaluate(On(), { Tank(100), Healer(80) }) == Hold::None);
    CHECK(Evaluate(On(), { Tank(60), Healer(100) }) == Hold::Tank);
    Member dead = Healer(0);
    dead.Alive = false;
    CHECK(Evaluate(On(), { Tank(100), dead }) == Hold::None);   // the route's dead-member wait owns that
    Member dps; dps.ManaPct = 5; dps.ManaUser = true;
    CHECK(Evaluate(On(), { Tank(100), Healer(100), dps }) == Hold::None);   // a dry caster is the mana wait's business
}

void WaitIsCappedAndEnds()
{
    Waiter waiter;
    bool ended = false;
    CHECK(waiter.Holding(On(), { Healer(10) }, false, 1000, &ended));
    CHECK(waiter.Holding(On(), { Healer(10) }, false, 1000 + 44000, &ended));
    CHECK(!waiter.Holding(On(), { Healer(10) }, false, 1000 + 45000, &ended));   // the cap
    CHECK(!waiter.Holding(On(), { Healer(10) }, false, 1000 + 90000, &ended));   // stays released
    CHECK(!ended);
    Waiter second;
    CHECK(second.Holding(On(), { Healer(10) }, false, 0, &ended));
    CHECK(!second.Holding(On(), { Healer(90) }, false, 5000, &ended));
    CHECK(ended);                                                                  // everyone ready: "ready?"
    CHECK(!second.Holding(On(), { Healer(90) }, false, 6000, &ended));
    CHECK(!ended);
    Waiter fight;
    CHECK(!fight.Holding(On(), { Healer(10) }, true, 0));                          // never in combat
}

void StragglerWaitIsShorter()
{
    Member far = Tank(100);
    far.LeaderYards = 80;
    Waiter waiter;
    CHECK(waiter.Holding(On(), { far, Healer(100) }, false, 0));
    CHECK(waiter.Holding(On(), { far, Healer(100) }, false, 19000));
    CHECK(!waiter.Holding(On(), { far, Healer(100) }, false, 20000));
}

void ChatLimitsAndKinds()
{
    Chatter chat;
    ChatFacts facts;
    facts.InDungeonParty = true;
    facts.GroupSize = 5;
    Line line = Line::Ready;
    int greets = 0;
    for (std::uint64_t bot = 1; bot <= 5; ++bot)
    {
        chat.Note(On(), facts, 7, bot, 100, 1000, line);   // first sight, nothing yet
    }
    for (std::uint64_t t = 1000; t < 60000; t += 1000)
        for (std::uint64_t bot = 1; bot <= 5; ++bot)
            if (chat.Note(On(), facts, 7, bot, 100, t, line))
                greets += line == Line::Greet;
    CHECK(greets == 2);                                    // at most two greeters per party

    // "ready?" only from the leader, once.
    ChatFacts leader = facts;
    leader.Leader = true;
    chat.MarkReady(100);
    CHECK(!chat.Note(On(), facts, 7, 3, 100, 200000, line) || line != Line::Ready);
    bool ready = false;
    for (int i = 0; i < 2; ++i)
        ready = chat.Note(On(), leader, 7, 1, 100, 300000 + i * 100, line) && line == Line::Ready ? true : ready;
    CHECK(ready);

    // Thanks after being dead, a mate near.
    Chatter again;
    ChatFacts down = facts;
    down.Alive = false;
    CHECK(!again.Note(On(), down, 7, 4, 100, 1000, line));
    ChatFacts up = facts;
    up.GroupMateNear = true;
    CHECK(again.Note(On(), up, 7, 4, 100, 20000, line) && line == Line::Thanks);
    CHECK(!again.Note(On(), up, 7, 4, 100, 21000, line));  // the bot's minute

    // Party limit: 6 lines in 5 minutes.
    Chatter busy;
    int said = 0;
    for (std::uint64_t t = 0; t < 280000; t += 1000)
        for (std::uint64_t bot = 1; bot <= 40; ++bot)
        {
            ChatFacts f = facts;
            f.GroupMateNear = true;
            if (busy.Note(On(), f, 3, bot, 9, t, line))
                ++said;
        }
    CHECK(said <= 6);
}

void LinePool()
{
    std::vector<std::string> pool = { "greet|Hi.", "ready|Ready?", "thanks|Thx.", "junk", "greet|" };
    CHECK(LinesOf(pool, Line::Greet).size() == 1 && LinesOf(pool, Line::Greet)[0] == "Hi.");
    CHECK(LinesOf(pool, Line::Ready).size() == 1);
    CHECK(LinesOf(pool, Line::Thanks)[0] == "Thx.");
}

void Rolls()
{
    RollFacts r;
    r.Bot = 5;
    r.Quality = 3;
    r.Usable = true;
    r.Upgrade = 200;
    CHECK(DecideRoll(On(), r) == Roll::Need);
    r.Rivals.push_back({ 6, true, 300 });                  // a groupmate gains more: greed
    CHECK(DecideRoll(On(), r) == Roll::Greed);
    r.Rivals[0] = { 6, false, 900 };                       // one who cannot use it does not count
    CHECK(DecideRoll(On(), r) == Roll::Need);
    r.Rivals[0] = { 4, true, 200 };                        // a tie: the lower id needs
    CHECK(DecideRoll(On(), r) == Roll::Greed);
    r.Bot = 3;
    CHECK(DecideRoll(On(), r) == Roll::Need);
    r.Upgrade = 9;                                         // under a point of item level
    r.Rivals.clear();
    CHECK(DecideRoll(On(), r) == Roll::Greed);
    r.Upgrade = 400;
    r.WantsStats = false;                                  // spirit mail on a warrior
    CHECK(DecideRoll(On(), r) == Roll::Greed);
    r.WantsStats = true;
    r.Usable = false;
    CHECK(DecideRoll(On(), r) == Roll::Greed);             // unusable BoE: sells
    r.BindOnPickup = true;
    CHECK(DecideRoll(On(), r) == Roll::Pass);
    r.Usable = true;
    r.Quality = 0;
    CHECK(DecideRoll(On(), r) == Roll::Pass);
}

BotSnapshot Member5(std::uint64_t guid, std::uint8_t role, float x, std::uint32_t mana)
{
    BotSnapshot bot;
    bot.guid = guid;
    bot.health = bot.max_health = 1000;
    bot.power_type = 0;
    bot.power = mana;
    bot.max_power = 1000;
    bot.level = 20;
    bot.map_id = 33;
    bot.group_role = role;
    bot.group_size = 2;
    bot.pos_x = x;
    bot.group_leader_guid = forever_module::MakeUnitRef(0, 0, 1);
    return bot;
}

void PolicyGreetsOnceAndNothingWhenOff()
{
    for (bool on : { false, true })
    {
        PolicyOptions options;
        options.WanderWhenIdle = false;
        options.GroupEtiquette.Enabled = on;
        options.DungeonsByMap[33] = PolicyOptions::DungeonRoute{};
        auto pools = std::make_shared<advisor::Pools>();
        pools->GroupEtiquette = { "greet|Hi all." };
        options.Pools = pools;
        BotPolicy policy(options);
        int lines = 0;
        for (std::uint64_t t = 1000; t < 40000; t += 1000)
        {
            WorldTickSnapshotView view;
            view.max_nearby_units = 4;
            view.bots.push_back(Member5(1, 2, 0, 1000));
            view.bots.push_back(Member5(2, 1, 5, 1000));
            view.nearby.resize(8);
            policy.Decide(view, t);
            for (advisor::Order const& order : policy.TakeAdvisorOrders())
                lines += order.What == advisor::Kind::PartySay && order.Text == "Hi all.";
        }
        CHECK(on ? lines >= 1 && lines <= 2 : lines == 0);
    }
}

}  // namespace

int main()
{
    OffByDefault();
    WaitsForHealerAndTank();
    WaitIsCappedAndEnds();
    StragglerWaitIsShorter();
    ChatLimitsAndKinds();
    LinePool();
    Rolls();
    PolicyGreetsOnceAndNothingWhenOff();
    if (failures == 0)
        std::printf("group_etiquette_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
