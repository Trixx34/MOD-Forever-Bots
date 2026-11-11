// SPDX-License-Identifier: GPL-2.0-or-later
// Policy tests, part 8 of 8 (split out of policy_test.cpp; the helpers are in policy_test_support.h).
#include "policy_test_support.h"

namespace {

void NoShieldOnAWarriorOrADruid()
{
    // Bot 12 is a warrior at 40% and in a fight with no target of its own: a shield candidate for any other class. For a warrior (rage) it is not.
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = LeveledDungeonView(40);
    view.bots[2].health = 400;
    view.bots[2].in_combat = 1;
    CHECK(!CastsSpell(policy.Decide(view, 1000), 10, 17, PlayerOf(12)));
    // The same member as a priest is shielded.
    auto const allPriests = [](std::uint64_t) { return 5u; };
    BotPolicy priests = DungeonPolicy(allPriests);
    WorldTickSnapshotView same = LeveledDungeonView(40, allPriests);
    same.bots[2].health = 400;
    same.bots[2].in_combat = 1;
    CHECK(CastsSpell(priests.Decide(same, 1000), 10, 17, PlayerOf(12)));
    // And a druid is not shielded either.
    auto const druidTwelve = [](std::uint64_t guid) { return guid == 12 ? 11u : 5u; };
    BotPolicy druid = DungeonPolicy(druidTwelve);
    WorldTickSnapshotView drood = LeveledDungeonView(40, druidTwelve);
    drood.bots[2].health = 400;
    drood.bots[2].in_combat = 1;
    CHECK(!CastsSpell(druid.Decide(drood, 1000), 10, 17, PlayerOf(12)));
}

// Weakened Soul keeps a second shield off the same member for 15 s: the priest heals the hurt member meanwhile (the Stockade priest ordered 15 shields in a row on a mage held at 44 %).
void NoSecondShieldOnTheSameMemberWhileWeakenedSoulHolds()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = LeveledDungeonView(40);
    view.bots[3].health = 440;                           // bot 13 (a priest, as the rest) at 44 %, in the fight
    view.bots[3].in_combat = 1;
    CHECK(CastsSpell(policy.Decide(view, 1000), 10, 17, PlayerOf(13)));        // shielded
    std::vector<BotAction> const next = policy.Decide(view, 4000);
    CHECK(!CastsSpell(next, 10, 17, PlayerOf(13)));                            // not again at once
    CHECK(CastsSpell(next, 10, 2054, PlayerOf(13)));                           // it gets the plain heal instead
    CHECK(CastsSpell(policy.Decide(view, 17000), 10, 17, PlayerOf(13)));       // the lock has run out: shielded again
}

void ARosterSpecSetsTheRoleOfAPriest()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    options.SpecOf = [](std::uint64_t guid) { return guid == 10 ? std::string("shadow") : std::string(); };
    BotPolicy shadow(options);
    shadow.SetRouteActive(true);
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    view.bots[3].health = 300; // bot 13 is hurt
    CHECK(!CastsSpell(shadow.Decide(view, 0), 10, 2050, PlayerOf(13)));        // a shadow priest does not heal
    options.SpecOf = [](std::uint64_t guid) { return guid == 10 ? std::string("holy") : std::string(); };
    BotPolicy holy(options);
    holy.SetRouteActive(true);
    CHECK(CastsSpell(holy.Decide(view, 0), 10, 2050, PlayerOf(13)));           // a holy priest does
}

// consume_check is one line per held bot per kConsumeLogMs (30 s), and none for a party that is not held.
void ConsumeCheckIsThrottledAndOnlyForAHeldParty()
{
    auto const held = HeldPartyLog(true, 44000);
    std::size_t const lines = CountLines(held, "consume_check: bot=12 ");
    CHECK(lines == 1);                                                // five ticks inside the hold, one line (the 30 s throttle)
    CHECK(CountLines(held, "consume_check: bot=10 ") == 0);           // a bot with no mana is not a drinker
    CHECK(CountLines(HeldPartyLog(false, 44000), "consume_check:") == 0);
}

// consume_check repeats once kConsumeLogMs (30 s) have passed, on the fake clock (ticks from 40000 ms). The line is written inside the consume step, which
// runs every ConsumeEveryMs (24 s), so the repeat comes on the first consume tick at least 30 s after the first line: the consumes are at 40, 64 and 88 s,
// the lines at 40 s and 88 s (not at 64 s, only 24 s after the first).
void ConsumeCheckRepeatsAfterThirtySeconds()
{
    CHECK(CountLines(HeldPartyLog(true, 64000, true), "consume_check: bot=12 ") == 1);   // the consume at 64 s is 24 s after the first line: throttled
    CHECK(CountLines(HeldPartyLog(true, 87000, true), "consume_check: bot=12 ") == 1);   // 47 s after the first: no consume tick yet
    CHECK(CountLines(HeldPartyLog(true, 88000, true), "consume_check: bot=12 ") == 2);   // the first consume tick past 30 s: the line repeats
}

// regroup_members names every member of the party, and comes once per kRegroupLogMs (15 s) while the regroup lasts.
void RegroupMembersLogsEveryMemberAndIsThrottled()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f } };
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 50.0f;
    view.bots[0].pos_x = 0.0f;                                        // bot 10 stays 50 yards behind
    for (std::uint64_t now = 1000; now <= 9000; now += 1000)
        policy.Decide(view, now);
    CHECK(CountLines(events, "regroup_members: leader=12 ") == 1);    // 8 s of regroup: the first line only
    std::string line;
    for (std::string const& e : events)
        if (e.rfind("regroup_members: leader=12 ", 0) == 0)
            line = e;
    for (char const* member : { " | 10 yd=50 ", " | 11 yd=0 ", " | 13 yd=0 ", " | 14 yd=0 " })
        CHECK(line.find(member) != std::string::npos);
    for (std::uint64_t now = 10000; now <= 20000; now += 1000)
        policy.Decide(view, now);
    CHECK(CountLines(events, "regroup_members: leader=12 ") == 2);    // the second line after 15 s, not before
}

// want_cost is only logged for a Deadmines cast result, and is -1 with no cost file at all.
void WantCostIsAbsentOffTheDeadminesAndMinusOneWithNoCostFile()
{
    for (int variant = 0; variant < 2; ++variant)
    {
        PolicyOptions options = AggressiveOptions();
        std::vector<std::string> events;
        options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
        PolicyOptions::ScenarioRoute route;
        route.Route = { Point{ 100.0f, 0.0f, 0.0f } };
        route.RouteMap = variant == 0 ? 36 : 1;
        BotPolicy policy(options);                                    // no SpellCosts
        policy.SetRouteActiveFor([](std::uint64_t) { return true; });
        policy.SetScenarioRouteFor([&](std::uint64_t) -> PolicyOptions::ScenarioRoute const* { return &route; });
        WorldTickSnapshotView view = PartyView({ 10 });
        view.bots[0].level = 23;
        view.bots[0].last_action_sequence = 1;
        view.bots[0].last_action_type = uint8_t(ActionType::kCastSpell);
        view.bots[0].last_action_spell = 1126;
        view.bots[0].last_action_status = BotActionStatus::kApplied;
        policy.Decide(view, 1000);
        bool minusOne = false, any = false;
        for (std::string const& e : events)
            if (e.rfind("cast_result:", 0) == 0)
            {
                any = true;
                minusOne = minusOne || e.find(" want_cost=-1 ") != std::string::npos;
            }
        if (variant == 0)
            CHECK(any && minusOne);
        else
            CHECK(!any);
    }
}

// The pull hold gives up kPullHoldMaxMs (45 s) after the party last rose, not earlier, and never later than a tick after.
void ThePullHoldCapIsReachedAndNotExceededWithFlatMana()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 }, WarriorLeads);
    for (BotSnapshot& bot : view.bots)
    {
        bot.group_role = bot.guid == 12 ? 2 : 1;
        bot.group_leader_guid = (std::uint64_t(2) << 56) | 12;
        bot.max_power = 0;
        if (bot.guid == 10)
        {
            bot.power_type = 0;
            bot.max_power = 100000;
            bot.power = 5000;
        }
    }
    std::uint64_t walkedAt = 0;
    for (std::uint64_t at = 1000; at <= 120000 && walkedAt == 0; at += 1000)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 12 && action.type == ActionType::kMoveTo && action.dest_x == 100.0f)
                walkedAt = at;
    CHECK(walkedAt != 0);
    CHECK(walkedAt >= 30000);                                         // it waited for the mana first
    CHECK(walkedAt <= 47000);                                         // and not past the 45 s cap
}

// No scenario, no director: the policy with none of the per-party hooks set must decide for a party, a lone bot and an empty view without crashing.
void NoScenarioNoDirectorDoesNotCrash()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    BotPolicy policy(options);                                        // no SetRouteActiveFor, SetTamingFor, SetStagingHoldFor, SetKilledBossesFor
    WorldTickSnapshotView const party = PartyView({ 10, 11, 12, 13, 14 }, WarriorLeads);
    WorldTickSnapshotView const lone = PartyView({ 10 });
    WorldTickSnapshotView empty;
    for (std::uint64_t at = 0; at < 20000; at += 1000)
    {
        policy.Decide(party, at);
        policy.Decide(lone, at);
        policy.Decide(empty, at);
    }
}

} // namespace

void RunPolicyTestsPart8()
{
    NoShieldOnAWarriorOrADruid();
    NoSecondShieldOnTheSameMemberWhileWeakenedSoulHolds();
    ARosterSpecSetsTheRoleOfAPriest();
    ConsumeCheckIsThrottledAndOnlyForAHeldParty();
    ConsumeCheckRepeatsAfterThirtySeconds();
    RegroupMembersLogsEveryMemberAndIsThrottled();
    WantCostIsAbsentOffTheDeadminesAndMinusOneWithNoCostFile();
    ThePullHoldCapIsReachedAndNotExceededWithFlatMana();
    NoScenarioNoDirectorDoesNotCrash();
}
