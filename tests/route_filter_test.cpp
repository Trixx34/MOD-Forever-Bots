// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the route filter (src/route_filter.h and its use in the grind and explore states): off changes nothing, a refused spot or cell is skipped with a growing
// lifetime, other bots skip it once enough bots were refused, an arrival refused for "no route" is a success, a level change forgets the bot's refusals, explore walks back off.
#include "bot_policy.h"
#include "grind_data.h"
#include "route_filter.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotActionStatus;
using forever_module::BotSnapshot;
using forever_module::WorldTickSnapshotView;
using namespace route_filter;

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
    s.SharedBots = 2; // the tests below count two bots (the shipped default is 3)
    return s;
}

constexpr std::uint64_t kSec = 1000;

void OffNeverBlocks()
{
    Memory memory{ Settings{} };
    CHECK(!memory.Refuse(1, SpotKey(3), 1000));
    CHECK(!memory.Blocked(1, SpotKey(3), 1001));
    CHECK(memory.BotEntries(1) == 0 && memory.SharedSize() == 0);
}

void RefusedKeyIsSkippedByTheBotUntilItsLifetimeEnds()
{
    Memory memory{ On() };
    CHECK(!memory.Blocked(1, SpotKey(3), 0));
    memory.Refuse(1, SpotKey(3), 10 * kSec);
    CHECK(memory.Blocked(1, SpotKey(3), 11 * kSec));
    CHECK(!memory.Blocked(1, SpotKey(4), 11 * kSec));          // another spot
    CHECK(!memory.Blocked(2, SpotKey(3), 11 * kSec));          // another bot: one report is not shared (SharedBots = 2)
    CHECK(memory.Blocked(1, SpotKey(3), 10 * kSec + 299 * kSec));
    CHECK(!memory.Blocked(1, SpotKey(3), 10 * kSec + 301 * kSec)); // 300 s
}

void RepeatsDoubleTheLifetimeUpToTheCap()
{
    Settings s = On();
    s.SharedBots = 0;
    Memory memory{ s };
    std::uint64_t now = 0;
    std::uint64_t expect[] = { 300, 600, 1200, 2400, 3600, 3600 };
    for (std::uint64_t seconds : expect)
    {
        memory.Refuse(1, SpotKey(9), now);
        CHECK(memory.Blocked(1, SpotKey(9), now + (seconds - 1) * kSec));
        CHECK(!memory.Blocked(1, SpotKey(9), now + (seconds + 1) * kSec));
        now += (seconds + 1) * kSec; // refused again just after it ran out: still remembered (forget time is twice the lifetime)
    }
    // A refusal long after everything was forgotten starts over at the first lifetime.
    memory.Refuse(1, SpotKey(9), now + 100000 * kSec);
    CHECK(!memory.Blocked(1, SpotKey(9), now + 100000 * kSec + 301 * kSec));
}

void EnoughBotsShareTheKey()
{
    Memory memory{ On() };
    CHECK(!memory.Refuse(1, SpotKey(5), 1000));
    CHECK(!memory.Refuse(1, SpotKey(5), 2000));                 // the same bot twice is one bot
    CHECK(!memory.Blocked(3, SpotKey(5), 3000));
    CHECK(memory.Refuse(2, SpotKey(5), 4000));                  // the second bot: shared
    CHECK(memory.Blocked(3, SpotKey(5), 5000));                 // a bot that never went there skips it
    CHECK(!memory.Refuse(4, SpotKey(5), 6000));                 // already shared: not reported again
    CHECK(!memory.Blocked(3, SpotKey(5), 4000 + 301 * kSec));
    // After the block ran out a single new refusal blocks again, with a longer lifetime.
    memory.Refuse(5, SpotKey(5), 4000 + 302 * kSec);
    CHECK(memory.Blocked(3, SpotKey(5), 4000 + 302 * kSec + 500 * kSec));
    CHECK(!memory.Blocked(3, SpotKey(5), 4000 + 302 * kSec + 601 * kSec));
}

void PendingReportsFadeAndNeverShareWhenOff()
{
    Memory memory{ On() };
    memory.Refuse(1, SpotKey(5), 0);
    CHECK(!memory.Refuse(2, SpotKey(5), 700 * kSec)); // the first report is older than two lifetimes: forgotten, so this is a first report again
    Settings none = On();
    none.SharedBots = 0;
    Memory alone{ none };
    alone.Refuse(1, SpotKey(5), 0);
    alone.Refuse(2, SpotKey(5), 1);
    alone.Refuse(3, SpotKey(5), 2);
    CHECK(alone.SharedSize() == 0 && !alone.Blocked(4, SpotKey(5), 3));
}

void SharedKeyCanBeFinerThanTheOwnKey()
{
    Settings three = On();
    three.SharedBots = 3;
    Memory memory{ three };
    std::uint64_t const here = SpotStartKey(5, 100, 100, 200), far = SpotStartKey(5, 3000, -900, 200);
    CHECK(here != far && SpotStartKey(5, 100, 100, 200) == SpotStartKey(5, 150, 190, 200) && SpotStartKey(5, 1, 1, 0) == SpotKey(5));
    CHECK(SpotStartKey(5, -10, -10, 200) != SpotStartKey(5, 10, 10, 200));
    memory.Refuse(1, SpotKey(5), 0, here);
    memory.Refuse(2, SpotKey(5), 1, here);
    CHECK(!memory.Blocked(9, SpotKey(5), 2, here));          // two bots, SharedBots = 3
    CHECK(memory.Refuse(3, SpotKey(5), 3, here));
    CHECK(memory.Blocked(9, SpotKey(5), 4, here));           // a bot of that start cell skips the spot
    CHECK(!memory.Blocked(9, SpotKey(5), 4, far));           // a bot that starts elsewhere still goes
    CHECK(memory.Blocked(1, SpotKey(5), 4, far));            // except the bot that was refused itself
}

void ReachedEraseTheKeyForEveryone()
{
    Memory memory{ On() };
    memory.Refuse(1, SpotKey(5), 0);
    memory.Refuse(2, SpotKey(5), 1);
    CHECK(memory.Blocked(3, SpotKey(5), 2));
    memory.Reached(1, SpotKey(5));
    CHECK(!memory.Blocked(1, SpotKey(5), 3) && !memory.Blocked(3, SpotKey(5), 3));
    CHECK(memory.Blocked(2, SpotKey(5), 3));   // another bot's own entry stays until its lifetime ends (reaching it erases the reaching bot's and the shared one)
}

void ForgetBotDropsOnlyItsOwnEntries()
{
    Memory memory{ On() };
    memory.Refuse(1, SpotKey(5), 0);
    memory.Refuse(1, SpotKey(6), 0);
    memory.Refuse(2, SpotKey(5), 1); // shared now
    memory.ForgetBot(1);
    CHECK(memory.BotEntries(1) == 0 && memory.BotEntries(2) == 1);
    CHECK(memory.Blocked(1, SpotKey(5), 2));   // the shared block stays
    CHECK(!memory.Blocked(1, SpotKey(6), 2));
}

void MemoryIsBounded()
{
    Settings s = On();
    s.MaxEntries = 100;
    Memory memory{ s };
    for (std::size_t i = 0; i < 200; ++i)
        memory.Refuse(1, SpotKey(i), i);
    CHECK(memory.BotEntries(1) <= kPerBotMax);
    for (std::uint64_t bot = 1; bot <= 3; ++bot)
        for (std::size_t i = 0; i < 300; ++i)
            memory.Refuse(bot, PointKey(0, float(i * 8), 0, 8), 1000 + i);
    CHECK(memory.SharedSize() <= 100);
}

void PointKeysAreCells()
{
    CHECK(PointKey(0, 3.0f, 3.0f, 8) == PointKey(0, 7.9f, 0.1f, 8));
    CHECK(PointKey(0, 3.0f, 3.0f, 8) != PointKey(0, 8.1f, 3.0f, 8));
    CHECK(PointKey(0, -0.5f, 1.0f, 8) != PointKey(0, 0.5f, 1.0f, 8));   // the cell below zero is its own cell
    CHECK(PointKey(0, 1.0f, 1.0f, 8) != PointKey(1, 1.0f, 1.0f, 8));    // another map
    CHECK((PointKey(0, -5000.0f, -9000.0f, 8) & kSpotBit) == 0 && (SpotKey(3) & kSpotBit) != 0);
}

void ExploreBackoffGrowsAndStops()
{
    Settings s = On();
    CHECK(ExploreIdleMs(s, 10000, 0) == 10000 && ExploreIdleMs(s, 10000, 2) == 10000);
    CHECK(ExploreIdleMs(s, 10000, 3) == 20000 && ExploreIdleMs(s, 10000, 6) == 40000);
    CHECK(ExploreIdleMs(s, 10000, 300) == 600000);
    s.ExploreBackoffMaxSec = 0;
    CHECK(ExploreIdleMs(s, 10000, 30) == 10000);
    CHECK(ExploreIdleMs(Settings{}, 10000, 30) == 10000);
}

// ---- the policy ----------------------------------------------------------------------------------------------------

GrindData Parse(char const* text)
{
    std::vector<std::string> warnings;
    std::istringstream in(text);
    return ParseGrindData(in, warnings);
}

WorldTickSnapshotView Bots(std::uint32_t level, std::size_t count, float x)
{
    WorldTickSnapshotView view;
    view.tick_number = 1;
    view.max_nearby_units = 2;
    for (std::size_t i = 0; i < count; ++i)
    {
        BotSnapshot bot;
        bot.guid = 7 + i;
        bot.health = 1000;
        bot.max_health = 1000;
        bot.level = level;
        bot.map_id = 0;
        bot.pos_x = x;
        view.bots.push_back(bot);
    }
    view.nearby.resize(2 * count);
    return view;
}

PolicyOptions Options(bool filter, std::vector<std::string>* events)
{
    PolicyOptions options;
    options.AggressiveStart = true;
    options.RouteFilter.Enabled = filter;
    if (events)
        options.OnEvent = [events](std::string const& line) { events->push_back(line); };
    return options;
}

bool IsSpotWalk(BotAction const& action)
{
    return action.type == ActionType::kMoveTo && action.dest_y == 0.0f && (action.dest_x == 60.0f || action.dest_x == 90.0f);
}

// Runs one idle bot for `seconds`; every walk to a spot is answered "no route" on the next tick. Returns the walks to spots, in order.
std::vector<float> RunRefusingSpots(BotPolicy& policy, WorldTickSnapshotView& view, std::uint64_t& now, int seconds)
{
    std::vector<float> picks;
    std::uint32_t sequence = view.bots[0].last_action_sequence;
    for (int i = 0; i < seconds; ++i, now += kSec)
        for (BotAction const& action : policy.Decide(view, now))
            if (IsSpotWalk(action))
            {
                picks.push_back(action.dest_x);
                view.bots[0].last_action_type = std::uint8_t(ActionType::kMoveTo);
                view.bots[0].last_action_status = BotActionStatus::kNoRoute;
                view.bots[0].last_action_sequence = ++sequence;
            }
    return picks;
}

std::vector<float> GrindRun(bool filter, std::vector<std::string>* events, std::uint32_t levelAtEnd, std::vector<float>* afterLevelUp)
{
    PolicyOptions options = Options(filter, events);
    options.Grind = std::make_shared<GrindData>(Parse("spot 0 60 0 0 1 5 0\nspot 0 90 0 0 1 5 0\n"));
    BotPolicy policy(options);
    WorldTickSnapshotView view = Bots(3, 1, 0.0f);
    std::uint64_t now = 1000;
    policy.Decide(view, now);
    now += 30 * kSec;
    std::vector<float> picks = RunRefusingSpots(policy, view, now, 200);
    if (afterLevelUp)
    {
        view.bots[0].level = levelAtEnd;
        *afterLevelUp = RunRefusingSpots(policy, view, now, 100);
    }
    return picks;
}

void RefusedSpotsAreNotPickedAgain()
{
    std::vector<float> const off = GrindRun(false, nullptr, 3, nullptr);
    CHECK(off.size() >= 4);                      // as before: A, B, A, B, ... (the visited list only keeps the last two out)
    CHECK(off.size() >= 3 && off[0] == 60.0f && off[1] == 90.0f && off[2] == 60.0f);

    std::vector<std::string> events;
    std::vector<float> after;
    std::vector<float> const on = GrindRun(true, &events, 4, &after);
    CHECK(on.size() == 2 && on[0] == 60.0f && on[1] == 90.0f);   // both refused: neither is chosen again within the lifetime; the bot explores instead
    bool skipped = false;
    for (std::string const& line : events)
        skipped = skipped || line.find("route_filter_skip:") != std::string::npos;
    CHECK(skipped);
    CHECK(!after.empty());                       // a level-up drops the bot's refusals: the spots are offered again
}

void NextCandidateIsOfferedWhenOneSpotIsRefused()
{
    PolicyOptions options = Options(true, nullptr);
    options.Grind = std::make_shared<GrindData>(Parse("spot 0 60 0 0 1 5 0\nspot 0 90 0 0 1 5 0\n"));
    BotPolicy policy(options);
    WorldTickSnapshotView view = Bots(3, 1, 0.0f);
    std::uint64_t now = 1000;
    policy.Decide(view, now);
    now += 30 * kSec;
    std::vector<float> const picks = RunRefusingSpots(policy, view, now, 20);
    CHECK(picks.size() == 1 && picks[0] == 60.0f);
    // Mid-run: the bot is still refused-for-A; with B free it must take B next (and not A, which the visited list would also allow after B).
    std::vector<float> const next = RunRefusingSpots(policy, view, now, 45);
    CHECK(!next.empty() && next[0] == 90.0f);
}

// Three bots standing at the spot (within 8 yards) when the walk is answered "no route": that is an arrival. Without the filter it counted as a failure of the spot, and three
// bots blocked the spot for everybody for an hour.
void ArrivalRefusedAsNoRouteIsNotAFailure()
{
    for (bool filter : { false, true })
    {
        std::vector<std::string> events;
        PolicyOptions options = Options(filter, &events);
        options.Grind = std::make_shared<GrindData>(Parse("spot 0 60 0 0 1 5 0\n"));
        BotPolicy policy(options);
        WorldTickSnapshotView view = Bots(3, 3, 35.0f);   // 25 yards from the spot: a walk, not an arrival
        policy.Decide(view, 1000);
        bool sent = false;
        for (BotAction const& action : policy.Decide(view, 1000 + 31 * kSec))
            sent = sent || IsSpotWalk(action);
        CHECK(sent);
        for (BotSnapshot& bot : view.bots)
        {
            bot.pos_x = 57.0f;                              // standing at it (3 yards)
            bot.last_action_type = std::uint8_t(ActionType::kMoveTo);
            bot.last_action_status = BotActionStatus::kNoRoute;
            bot.last_action_sequence = 1;
        }
        policy.Decide(view, 1000 + 32 * kSec);
        int arrived = 0, blocked = 0;
        for (std::string const& line : events)
        {
            arrived += line.find("route_filter_arrived:") != std::string::npos;
            blocked += line.find("grind_spot_blocked:") != std::string::npos;
        }
        if (filter)
            CHECK(arrived == 3 && blocked == 0);
        else
            CHECK(arrived == 0 && blocked == 1);
    }
}

int ExploreWalks(bool filter, int seconds)
{
    PolicyOptions options = Options(filter, nullptr);
    BotPolicy policy(options);
    WorldTickSnapshotView view = Bots(3, 1, 0.0f);
    std::uint64_t now = 1000;
    std::uint32_t sequence = 0;
    int walks = 0;
    for (int i = 0; i < seconds; ++i, now += kSec)
        for (BotAction const& action : policy.Decide(view, now))
            if (action.type == ActionType::kMoveTo)
            {
                ++walks;
                view.bots[0].last_action_type = std::uint8_t(ActionType::kMoveTo);
                view.bots[0].last_action_status = BotActionStatus::kNoRoute;
                view.bots[0].last_action_sequence = ++sequence;
            }
    return walks;
}

void ExploreBacksOffAfterRefusals()
{
    int const off = ExploreWalks(false, 1800);
    int const on = ExploreWalks(true, 1800);
    CHECK(off > 60);
    CHECK(on > 0 && on * 3 < off);   // the pause doubles after each cycle of refusals up to ten minutes
}

} // namespace

int main()
{
    OffNeverBlocks();
    RefusedKeyIsSkippedByTheBotUntilItsLifetimeEnds();
    RepeatsDoubleTheLifetimeUpToTheCap();
    EnoughBotsShareTheKey();
    SharedKeyCanBeFinerThanTheOwnKey();
    PendingReportsFadeAndNeverShareWhenOff();
    ReachedEraseTheKeyForEveryone();
    ForgetBotDropsOnlyItsOwnEntries();
    MemoryIsBounded();
    PointKeysAreCells();
    ExploreBackoffGrowsAndStops();
    RefusedSpotsAreNotPickedAgain();
    NextCandidateIsOfferedWhenOneSpotIsRefused();
    ArrivalRefusedAsNoRouteIsNotAFailure();
    ExploreBacksOffAfterRefusals();
    if (failures == 0)
        std::printf("route_filter_test: ok\n");
    return failures == 0 ? 0 : 1;
}
