// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the party mana sync (src/party_mana.h) and its policy step: off by default, one line per low-mana episode, the hold lasts until 80 percent or
// the cap, and it never applies in combat. No core needed.
#include "advisor_text.h"
#include "bot_policy.h"
#include "party_mana.h"

#include <cstdio>

using namespace party_mana;
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
    Settings settings;
    settings.Enabled = true;
    return settings;
}

Facts Healer(std::uint32_t power, bool combat = false)
{
    Facts facts;
    facts.InDungeonParty = true;
    facts.ManaUser = true;
    facts.InCombat = combat;
    facts.Power = power;
    facts.MaxPower = 1000;
    return facts;
}

void OffByDefault()
{
    Tracker tracker;
    CHECK(!Settings{}.Enabled);
    CHECK(!tracker.Note(Settings{}, Healer(10), 1, 0));
    CHECK(!tracker.Holding(Settings{}, false, 1000));
}

void SaysOneLineAtTwentyPercentAndHoldsUntilEighty()
{
    Tracker tracker;
    CHECK(!tracker.Note(On(), Healer(250), 1, 0));          // 25 percent: fine
    CHECK(!tracker.Holding(On(), false, 0));
    CHECK(tracker.Note(On(), Healer(190), 1, 1000));        // 19 percent: says its line
    CHECK(!tracker.Note(On(), Healer(190), 1, 2000));       // but only once
    CHECK(tracker.Holding(On(), false, 3000));
    CHECK(!tracker.Note(On(), Healer(790), 1, 4000));       // 79 percent: still waiting
    CHECK(tracker.Holding(On(), false, 4000));
    CHECK(!tracker.Note(On(), Healer(800), 1, 5000));       // 80 percent: ready
    CHECK(!tracker.Holding(On(), false, 5000));
    CHECK(tracker.Note(On(), Healer(100), 1, 6000));        // a later dry spell speaks again
}

void TheHoldIsCappedAndNeverInCombat()
{
    Tracker tracker;
    CHECK(tracker.Note(On(), Healer(50), 1, 10000));
    CHECK(tracker.Holding(On(), false, 10000 + 59000));
    CHECK(!tracker.Holding(On(), true, 10000 + 1000));      // in combat: no hold
    CHECK(!tracker.Holding(On(), false, 10000 + 60000));    // the cap
    CHECK(!tracker.Holding(On(), false, 10000 + 200000));   // and it stays released while the bot is still low
}

void ASecondLowBotCannotExtendTheCap()
{
    Tracker tracker;
    CHECK(tracker.Note(On(), Healer(50), 1, 0));
    CHECK(tracker.Note(On(), Healer(50), 2, 50000));
    CHECK(!tracker.Holding(On(), false, 61000));
}

void OnlyManaUsersInADungeonParty()
{
    Tracker tracker;
    Facts warrior = Healer(10);
    warrior.ManaUser = false;
    CHECK(!tracker.Note(On(), warrior, 1, 0));
    Facts outside = Healer(10);
    outside.InDungeonParty = false;
    CHECK(!tracker.Note(On(), outside, 2, 0));
    CHECK(!tracker.Note(On(), Healer(10, true), 3, 0));     // in combat
    CHECK(!tracker.Holding(On(), false, 100));
}

BotSnapshot Priest(std::uint64_t guid, std::uint32_t power, std::uint8_t role)
{
    BotSnapshot bot;
    bot.guid = guid;
    bot.health = bot.max_health = 1000;
    bot.power = power;
    bot.max_power = 1000;
    bot.level = 20;
    bot.map_id = 33;
    bot.group_role = role;
    bot.group_leader_guid = forever_module::MakeUnitRef(0, 0, 1);
    return bot;
}

void PolicySaysItOnceInPartyChat()
{
    PolicyOptions options;
    options.WanderWhenIdle = false;
    options.PartyMana = On();
    options.DungeonsByMap[33] = PolicyOptions::DungeonRoute{};
    options.ClassOf = [](std::uint64_t) -> std::uint32_t { return 5; };   // priest
    auto pools = std::make_shared<advisor::Pools>();
    pools->PartyMana = { "Low on mana, one moment." };
    options.Pools = pools;
    BotPolicy policy(options);
    int lines = 0;
    for (std::uint64_t t = 1000; t < 20000; t += 1000)
    {
        WorldTickSnapshotView view;
        view.max_nearby_units = 4;
        view.bots.push_back(Priest(5, 100, 1));
        view.nearby.resize(4);
        policy.Decide(view, t);
        for (advisor::Order const& order : policy.TakeAdvisorOrders())
            lines += order.What == advisor::Kind::PartySay && order.Text == "Low on mana, one moment.";
    }
    CHECK(lines == 1);
}

}  // namespace

int main()
{
    OffByDefault();
    SaysOneLineAtTwentyPercentAndHoldsUntilEighty();
    TheHoldIsCappedAndNeverInCombat();
    ASecondLowBotCannotExtendTheCap();
    OnlyManaUsersInADungeonParty();
    PolicySaysItOnceInPartyChat();
    if (failures == 0)
        std::printf("party_mana_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
