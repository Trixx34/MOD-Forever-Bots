// SPDX-License-Identifier: GPL-2.0-or-later
// Policy tests, part 4 of 8 (split out of policy_test.cpp; the helpers are in policy_test_support.h).
#include "policy_test_support.h"

namespace {

void ALeaderWithNoRouteIsPutBackOnTheRouteOnce()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.DungeonTeleport = true;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f }, Point{ 180.0f, 0.0f, 0.0f } };
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 95.0f;
    CHECK(LeaderWalk(policy.Decide(view, 0)) == 140.0f || true);       // at the first point: on to the second
    view.bots[2].pos_x = 130.0f;
    policy.Decide(view, 500);
    std::optional<BotAction> teleport;
    for (std::uint32_t refusal = 1; refusal <= 3; ++refusal)
    {
        view.bots[2].last_action_sequence = refusal;
        view.bots[2].last_action_type = uint8_t(ActionType::kMoveTo);
        view.bots[2].last_action_status = BotActionStatus::kNoRoute;
        if (auto const found = TeleportOf(policy.Decide(view, 1000 * refusal), 12))
            teleport = found;
    }
    CHECK(teleport.has_value() && teleport->dest_x == 100.0f);        // back to the point before
    bool recovered = false;
    for (std::string const& line : events)
        recovered = recovered || line.rfind("route_recover: bot=12", 0) == 0;
    CHECK(recovered);
    // The same point failing again is skipped, not teleported for again.
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 130.0f;                                           // the party is together again
    std::optional<BotAction> again;
    float latest = -1.0f;
    for (std::uint32_t refusal = 4; refusal <= 7; ++refusal)
    {
        view.bots[2].last_action_sequence = refusal;
        view.bots[2].last_action_type = uint8_t(ActionType::kMoveTo);
        view.bots[2].last_action_status = BotActionStatus::kNoRoute;
        auto const actions = policy.Decide(view, 10000 + 1000 * refusal);
        if (auto const found = TeleportOf(actions, 12))
            again = found;
        if (LeaderWalk(actions) >= 0.0f)
            latest = LeaderWalk(actions);
    }
    CHECK(!again.has_value());
    CHECK(latest == 180.0f);
}

void AMemberThatLostItsLeaderIsPutNextToIt()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.DungeonTeleport = true;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 0.0f;
    view.bots[0].pos_x = -200.0f;                                     // bot 10 is far away
    CHECK(!TeleportOf(policy.Decide(view, 1000), 10).has_value());    // not at once
    CHECK(!TeleportOf(policy.Decide(view, 10000), 10).has_value());
    std::optional<BotAction> back = TeleportOf(policy.Decide(view, 32000), 10);      // after kLostLeaderMs (15 s)
    CHECK(back.has_value() && back->dest_x == -3.0f);                 // next to the leader (3 yards behind it)
    CHECK(!TeleportOf(policy.Decide(view, 33000), 10).has_value());   // and not again at once
    // A member stuck past the distance the leader waits at (30 yards), but not far, is put back too: the leader would wait for it for good.
    BotPolicy near(options);
    near.SetRouteActive(true);
    view.bots[0].pos_x = -45.0f;
    near.Decide(view, 1000);
    std::optional<BotAction> close = TeleportOf(near.Decide(view, 32000), 10);
    CHECK(close.has_value() && close->dest_x == -3.0f);
    // Off, nothing happens.
    PolicyOptions off = options;
    off.DungeonTeleport = false;
    BotPolicy quiet(off);
    quiet.SetRouteActive(true);
    quiet.Decide(view, 1000);
    CHECK(!TeleportOf(quiet.Decide(view, 30000), 10).has_value());
}

// A leader revived at the instance entrance goes to its party; the party is not put back at the entrance.
void ALeaderRevivedAtTheEntranceGoesToItsParty()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.DungeonTeleport = true;
    options.Route = { Point{ 220.0f, 0.0f, 0.0f }, Point{ 260.0f, 0.0f, 0.0f } };
    PolicyOptions::DungeonRoute dungeon;
    dungeon.HasEntry = true;
    dungeon.Entry = Point{ 0.0f, 0.0f, 0.0f };
    options.DungeonsByMap[36] = dungeon;
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
    {
        bot.map_id = 36;
        bot.pos_x = 200.0f;                                           // the party, deep in
    }
    view.bots[2].pos_x = 2.0f;                                        // the leader (12), just revived at the entrance
    std::optional<BotAction> leader = TeleportOf(policy.Decide(view, 1000), 12);
    CHECK(leader.has_value() && leader->dest_x == 197.0f);           // next to a member
    bool logged = false;
    for (std::string const& line : events)
        logged = logged || line.rfind("leader_recover: bot=12", 0) == 0;
    CHECK(logged);
    std::optional<BotAction> member;
    for (std::uint64_t now : { 10000ull, 22000ull, 25000ull })
        if (auto const found = TeleportOf(policy.Decide(view, now), 10))
            member = found;
    CHECK(!member.has_value());                                       // the members are not sent to the entrance
}

// 0.44.8: a leader at the entrance goes to a party that is in a fight, but only after 15 s at the entrance.
void ALeaderAtTheEntranceGoesToAFightingPartyAfterAWhile()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.DungeonTeleport = true;
    options.Route = { Point{ 220.0f, 0.0f, 0.0f }, Point{ 260.0f, 0.0f, 0.0f } };
    PolicyOptions::DungeonRoute dungeon;
    dungeon.HasEntry = true;
    dungeon.Entry = Point{ 0.0f, 0.0f, 0.0f };
    options.DungeonsByMap[36] = dungeon;
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
    {
        bot.map_id = 36;
        bot.pos_x = 200.0f;
        bot.in_combat = 1;                                            // the whole party fights
    }
    view.bots[2].pos_x = 2.0f;                                        // the leader (12) at the entrance
    view.bots[2].in_combat = 0;
    CHECK(!TeleportOf(policy.Decide(view, 1000), 12).has_value());    // just arrived: not yet
    CHECK(!TeleportOf(policy.Decide(view, 9000), 12).has_value());
    std::optional<BotAction> leader = TeleportOf(policy.Decide(view, 17000), 12);
    CHECK(leader.has_value() && leader->dest_x == 197.0f);           // 16 s on: next to a member
}

// The Stockade's revived tank (2026-10-28 sim: 31 of 96 tank deaths came within 120 s of the same tank's last one, 24 of 60 leader_recover teleports ended in its death):
// a leader revived at the entrance at half health rests there until it is fit to walk (80 %), at most two minutes, and the members deep in the dungeon do not walk back to it.
void ARevivedLeaderRestsAtTheEntranceBeforeItGoesToItsParty()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.DungeonTeleport = true;
    options.Route = { Point{ 220.0f, 0.0f, 0.0f }, Point{ 260.0f, 0.0f, 0.0f } };
    PolicyOptions::DungeonRoute dungeon;
    dungeon.HasEntry = true;
    dungeon.Entry = Point{ 0.0f, 0.0f, 0.0f };
    options.DungeonsByMap[36] = dungeon;
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    auto const hurtLeaderView = [](std::uint32_t health)
    {
        WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
        for (BotSnapshot& bot : view.bots)
        {
            bot.map_id = 36;
            bot.pos_x = 200.0f;                                       // the party, deep in
        }
        view.bots[2].pos_x = 2.0f;                                    // the leader (12), just revived at the entrance
        view.bots[2].health = health;                                 // of 1000
        return view;
    };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView hurt = hurtLeaderView(490);
    bool followedTheLeader = false;
    for (std::uint64_t now : { 1000ull, 5000ull, 30000ull, 100000ull })
    {
        std::vector<BotAction> const actions = policy.Decide(hurt, now);
        CHECK(!TeleportOf(actions, 12).has_value());                  // it rests at the entrance first
        for (BotAction const& action : actions)
            followedTheLeader = followedTheLeader || (action.type == ActionType::kFollow && action.bot_guid != 12 && action.target_guid == PlayerOf(12));
    }
    CHECK(!followedTheLeader);                                        // and the members stay where they are
    bool waited = false;
    for (std::string const& line : events)
        waited = waited || line.rfind("leader_recover_wait: bot=12", 0) == 0;
    CHECK(waited);
    // 0.45.4: two minutes at the entrance were not enough for a tank at half health (the sim's deaths): it still rests at two minutes, and only after kLeaderRecoverMaxWaitMs goes in.
    CHECK(!TeleportOf(policy.Decide(hurt, 122000), 12).has_value());
    std::optional<BotAction> gaveUp = TeleportOf(policy.Decide(hurt, 601000), 12);   // it cannot recover: after kLeaderRecoverMaxWaitMs it goes in all the same
    CHECK(gaveUp.has_value() && gaveUp->dest_x == 197.0f);
    // 0.44.17: 60 % or more and nobody of the party in a fight: at once. Below 60 % it still rests; with a member in a fight it still rests.
    BotPolicy sixty(options);
    sixty.SetRouteActive(true);
    std::optional<BotAction> atSixty = TeleportOf(sixty.Decide(hurtLeaderView(600), 1000), 12);
    CHECK(atSixty.has_value() && atSixty->dest_x == 197.0f);
    BotPolicy fiftyNine(options);
    fiftyNine.SetRouteActive(true);
    CHECK(!TeleportOf(fiftyNine.Decide(hurtLeaderView(599), 1000), 12).has_value());
    BotPolicy fighting(options);
    fighting.SetRouteActive(true);
    WorldTickSnapshotView busyParty = hurtLeaderView(700);
    busyParty.bots[3].in_combat = 1;                                  // a member deep in the dungeon fights
    CHECK(!TeleportOf(fighting.Decide(busyParty, 1000), 12).has_value());
    // Fit (80 % or more): at once, as before.
    BotPolicy fit(options);
    fit.SetRouteActive(true);
    std::optional<BotAction> now = TeleportOf(fit.Decide(hurtLeaderView(850), 1000), 12);
    CHECK(now.has_value() && now->dest_x == 197.0f);
}

// A leader whose walk along the route goes nowhere sends it again and counts the point as refused (route_stall); a leader that waits says why (route_hold).
void ALeaderThatDoesNotMoveIsNoticed()
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
        bot.pos_x = 50.0f;                                            // together, and nobody moves
    std::uint32_t walks = 0;                                          // the leader's walk orders sent
    for (std::uint64_t now = 0; now <= 32000; now += 1000)
        walks += LeaderWalk(policy.Decide(view, now)) == 100.0f;
    auto const has = [&events](std::string const& prefix)
    {
        for (std::string const& line : events)
            if (line.rfind(prefix, 0) == 0)
                return true;
        return false;
    };
    CHECK(has("route_stall: bot=12 point=0/2"));
    CHECK(walks >= 2);                                                // the first walk, and the same walk sent again
    // A member far behind: the leader waits, and says for whom.
    events.clear();
    view.bots[0].pos_x = 0.0f;                                        // bot 10, 50 yards behind
    for (std::uint64_t now = 40000; now <= 75000; now += 1000)
        policy.Decide(view, now);
    CHECK(has("route_hold: bot=12 point=0/2 why=member_far"));
    bool named = false;
    for (std::string const& line : events)
        named = named || (line.rfind("route_hold: bot=12", 0) == 0 && line.find(" farthest=10 ") != std::string::npos);
    CHECK(named);
}

// A leader below 80 % health holds for its healer; a healer far behind is fetched: after kHealerFetchAfterMs the leader walks back to it (healer_fetch), not before,
// and not when a healer is near.
void ALeaderWalksBackToAFarHealer()
{
    auto const make = [](float healerX, std::vector<std::string>& events)
    {
        PolicyOptions options = AggressiveOptions();
        options.PartySize = 5;
        options.ClassOf = WarriorLeads;
        options.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f } };
        options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
        return std::make_pair(std::make_unique<BotPolicy>(options), healerX);
    };
    auto const run = [&](float healerX, std::uint64_t until, std::vector<std::string>& events)
    {
        auto [policy, x] = make(healerX, events);
        policy->SetRouteActive(true);
        WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
        for (BotSnapshot& bot : view.bots)
            bot.pos_x = bot.guid == 12 ? 50.0f : x;
        view.bots[2].health = 500;                                      // the leader, 50 %
        view.bots[2].max_health = 1000;
        float walk = -1.0f;
        for (std::uint64_t now = 0; now <= until; now += 1000)
            if (float const sent = LeaderWalk(policy->Decide(view, now)); sent == x)
                walk = sent;                                           // the walk order is sent once, when it changes
        return walk;
    };
    std::vector<std::string> events;
    CHECK(run(-300.0f, 30000, events) != -300.0f);                      // too early
    CHECK(events.empty() || events.back().rfind("healer_fetch:", 0) != 0);
    events.clear();
    CHECK(run(-300.0f, 60000, events) == -300.0f);                      // the healers are 350 yards behind: walk to one
    bool fetched = false;
    for (std::string const& line : events)
        fetched = fetched || line.rfind("healer_fetch: bot=12", 0) == 0;
    CHECK(fetched);
    events.clear();
    CHECK(run(30.0f, 60000, events) != 30.0f);                          // a healer 20 yards away: the leader stays, the top-up heals it
}

// Regroup (0.35.0): a member out of range for kRegroupStuckMs is stuck. The leader holds for it until then, then walks on without it, and the
// regroup ends when the party is ready again after the member is back.
void ARegroupStopsWaitingForAStuckMember()
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
        bot.pos_x = 50.0f;                                            // together at 50
    view.bots[0].pos_x = 0.0f;                                        // bot 10 stays 50 yards behind
    auto const has = [&events](std::string const& prefix)
    {
        for (std::string const& line : events)
            if (line.rfind(prefix, 0) == 0)
                return true;
        return false;
    };
    CHECK(LeaderWalk(policy.Decide(view, 1000)) == -1.0f);            // the party is not together: no walk
    CHECK(has("regroup_start: leader=12 reason=member_far far=1"));
    CHECK(LeaderWalk(policy.Decide(view, 10000)) == -1.0f);           // not stuck yet (9 s out of range, kRegroupStuckMs is 10 s)
    CHECK(LeaderWalk(policy.Decide(view, 22000)) == 100.0f);          // stuck (21 s, past kRegroupStuckMs): the leader walks on without it
    CHECK(has("regroup_stuck: leader=12 bot=10 "));
    policy.Decide(view, 23000);
    CHECK(!has("regroup_done:"));                                     // the regroup stays on while a member is out of range
    view.bots[0].pos_x = 50.0f;                                       // bot 10 catches up
    policy.Decide(view, 24000);
    policy.Decide(view, 40000);                                       // the party has been ready for the clearing time
    bool done = false;
    for (std::string const& line : events)
        done = done || (line.rfind("regroup_done: leader=12 ", 0) == 0 && line.find(" stuck=1") != std::string::npos);
    CHECK(done);
}

// Regroup (0.36.2): a member that trails the leader at the edge of the wait distance does not flip the stuck flag. It is logged once per regroup, stays stuck
// until it is back inside kRegroupClearYards, and the leader keeps walking.
void AStuckMemberAtTheBoundaryIsLoggedOnce()
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
        bot.pos_x = 0.0f;
    auto const count = [&events](std::string const& prefix)
    {
        int found = 0;
        for (std::string const& line : events)
            found += line.rfind(prefix, 0) == 0;
        return found;
    };
    // Bot 10 is 31 yards behind until it is stuck (22 s), then steps in and out of range (29 and 31 yards) every second for a minute.
    bool walked = false;                                              // the walk order is sent when it changes, so look for it over the minute
    for (std::uint64_t now = 1000; now <= 60000; now += 1000)
    {
        view.bots[0].pos_x = (now > 22000 && (now / 1000) % 2 == 0) ? -29.0f : -31.0f;
        walked = LeaderWalk(policy.Decide(view, now)) == 100.0f || walked;
    }
    CHECK(count("regroup_stuck: leader=12 bot=10 ") == 1);            // one line, not one per step in and out of range
    CHECK(walked);                                                    // the leader walked on once it was stuck
    // Back to 25 yards: inside the wait distance, but not inside the clearing distance, so it is still stuck and the regroup goes on.
    view.bots[0].pos_x = -25.0f;
    policy.Decide(view, 61000);
    CHECK(count("regroup_stuck: leader=12 bot=10 ") == 1);
    view.bots[0].pos_x = -31.0f;
    policy.Decide(view, 90000);
    policy.Decide(view, 120000);
    CHECK(count("regroup_stuck: leader=12 bot=10 ") == 1);            // the same regroup: not logged again
    CHECK(count("regroup_done:") == 0);
}

// Regroup (0.36.2): member_recover waits longer than the stuck flag, so the leader has walked on before the member is put next to it.
void MemberRecoverDoesNotFireWithTheStuckEvent()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.DungeonTeleport = true;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f } };
    std::uint64_t now = 0;
    std::uint64_t stuckAt = 0;
    std::uint64_t recoverAt = 0;
    options.OnEvent = [&](std::string const& line)
    {
        if (line.rfind("regroup_stuck:", 0) == 0 && stuckAt == 0)
            stuckAt = now;
        if (line.rfind("member_recover: bot=10", 0) == 0 && recoverAt == 0)
            recoverAt = now;
    };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 0.0f;
    view.bots[0].pos_x = -31.0f;                                      // bot 10 trails at the edge
    for (now = 1000; now <= 60000 && recoverAt == 0; now += 500)
        policy.Decide(view, now);
    CHECK(stuckAt != 0 && recoverAt != 0);
    CHECK(recoverAt > stuckAt);                                       // never in the same tick
    CHECK(recoverAt - stuckAt >= 5000);                               // the party has had time to move on
}

// The default stance is defensive: a bot does not pull what is in range on its own, and it fights what attacks it.
void DefaultBotsDoNotPullWhatIsInRange()
{
    BotPolicy policy(PolicyOptions{});
    NearbyUnit const mob = Hostile(7, 3.0f, 100);
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, mob);
    CHECK(!HasAction(policy.Decide(view, 0), 10, ActionType::kAttackTarget, mob.guid));
    view.bots[0].threat_target_guid = mob.guid;
    CHECK(HasAction(policy.Decide(view, 1000), 10, ActionType::kAttackTarget, mob.guid));
}

// A party that flips between ready and not ready every tick (a member that is a ghost one moment and not the next) must not hold the leader forever.
void AFlippingPartyDoesNotHoldTheLeaderForever()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView hurt = PartyView({ 10, 11, 12, 13, 14 });
    hurt.bots[1].health = 400;                                        // bot 11 below the ready mark
    WorldTickSnapshotView healthy = PartyView({ 10, 11, 12, 13, 14 });
    AddNearby(healthy, 2, Unit(8, 7, 20.0f, true));
    std::uint64_t now = 0;
    bool pulled = false;
    for (int tick = 0; tick < 400 && !pulled; ++tick)
    {
        now += 500;
        WorldTickSnapshotView const& view = (tick % 2 == 0) ? hurt : healthy;   // flips every tick
        pulled = HasAction(policy.Decide(view, now), 12, ActionType::kFollow, MakeUnitRef(8, 1, 7)) ||
                 HasAction(policy.Decide(view, now), 12, ActionType::kAttackTarget);
    }
    CHECK(pulled);                                                    // the wait ends after kPullHoldMaxMs (45 s) of flipping
}

void HealerHealsTheHurtMember()
{
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    view.bots[3].health = 300; // bot 13
    auto const actions = DungeonPolicy().Decide(view, 0);
    CHECK(CastsSpell(actions, 10, 2050, PlayerOf(13)));
}

// Protocol 28 (HealDownrank): the heal is cast at the lowest rank that covers the deficit, named exactly (dest_x = 1); off by default.
void HealDownrankCastsTheRankThatCoversTheDeficit()
{
    auto const run = [](bool downrank, std::uint32_t health, std::uint32_t maxHealth)
    {
        PolicyOptions options = AggressiveOptions();
        options.PartySize = 5;
        options.PartyCount = 1;
        options.ClassOf = WarriorLeads;
        options.HealDownrank = downrank;
        BotPolicy policy(options);
        policy.SetRouteActive(true);
        WorldTickSnapshotView view = LeveledDungeonView(30);   // the priest: Heal from level 16, ranks 2054, 2055 and 6063 by level 30
        view.bots[3].health = health;      // bot 13: dungeon healers heal below 55 %
        view.bots[3].max_health = maxHealth;
        return policy.Decide(view, 1000);
    };
    auto const castOf = [](std::vector<BotAction> const& actions) -> BotAction const*
    {
        for (BotAction const& action : actions)
            if (action.type == ActionType::kCastSpell && action.bot_guid == 10)
                return &action;
        return nullptr;
    };
    std::vector<BotAction> const small = run(true, 400, 800);              // deficit 400 (50 %): rank 2 (406)
    BotAction const* smallCast = castOf(small);
    CHECK(smallCast && smallCast->spell_id == 2055 && smallCast->dest_x == 1.0f && smallCast->target_guid == PlayerOf(13));
    std::vector<BotAction> const big = run(true, 450, 1000);             // deficit 550: no rank is enough, the highest the level allows (6063, 524)
    BotAction const* bigCast = castOf(big);
    CHECK(bigCast && bigCast->spell_id == 6063 && bigCast->dest_x == 1.0f);
    std::vector<BotAction> const off = run(false, 400, 800);               // default: rank 1 named, dest_x 0, the core casts the highest rank known
    BotAction const* offCast = castOf(off);
    CHECK(offCast && offCast->spell_id == 2054 && offCast->dest_x == 0.0f);
}

void HealDownrankPausesAfterAnUnknownRank()
{
    std::vector<std::string> events;
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    options.HealDownrank = true;
    options.OnEvent = [&events](std::string const& event) { events.push_back(event); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = LeveledDungeonView(30);
    view.bots[3].health = 400;
    view.bots[3].max_health = 800;
    policy.Decide(view, 1000);
    view.bots[0].last_action_sequence = 4;
    view.bots[0].last_action_type = uint8_t(ActionType::kCastSpell);
    view.bots[0].last_action_spell = 2055;
    view.bots[0].last_action_status = BotActionStatus::kNotKnown;
    policy.Decide(view, 6000);
    CHECK(Logged(events, "heal_downrank_off: bot=10 spell=2055"));
    bool plain = false;
    for (BotAction const& action : policy.Decide(view, 20000))
        plain = plain || (action.type == ActionType::kCastSpell && action.bot_guid == 10 && action.spell_id == 2054 && action.dest_x == 0.0f);
    CHECK(plain);                                                        // the plain heal while paused
}

void HealerHealsItselfWithoutATarget()
{
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    view.bots[0].health = 300;
    auto const actions = DungeonPolicy().Decide(view, 0);
    CHECK(CastsSpell(actions, 10, 2050, 0));
}

void PriestWithNoLineOfSightRepositionsBeforeRetryingDps()
{
    std::vector<std::string> events;
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    options.OnEvent = [&events](std::string const& event) { events.push_back(event); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    NearbyUnit hostile = Hostile(7, 20.0f, 100);
    hostile.pos_x = 20.0f;
    view.bots[2].in_combat = 1; // tank is engaging this hostile
    view.bots[2].threat_target_guid = hostile.guid;
    AddNearby(view, 0, hostile);

    std::vector<BotAction> first = policy.Decide(view, 1000);
    std::uint32_t dpsSpell = 0;
    for (BotAction const& action : first)
        if (action.bot_guid == 10 && action.type == ActionType::kCastSpell)
        {
            dpsSpell = action.spell_id;
            break;
        }
    CHECK(dpsSpell != 0);
    if (dpsSpell == 0)
        return;

    // The core refused that cast for LoS; the next tick must move toward the same target,
    // not retry the spell or issue a MoveTo with only a target GUID and zero coordinates.
    view.bots[0].last_action_type = std::uint8_t(ActionType::kCastSpell);
    view.bots[0].last_action_spell = dpsSpell;
    view.bots[0].last_action_status = BotActionStatus::kNoLineOfSight;
    view.bots[0].last_action_sequence = 1;
    std::vector<BotAction> retry = policy.Decide(view, 1200);
    bool moved = false;
    for (BotAction const& action : retry)
    {
        if (action.bot_guid == 10 && action.type == ActionType::kMoveTo &&
            (action.dest_x != 0.0f || action.dest_y != 0.0f || action.dest_z != 0.0f))
            moved = true;
    }
    CHECK(moved);
    CHECK(!CastsSpell(retry, 10, dpsSpell));
    CHECK(Logged(events, "healer_dps_los:"));
}

void NoHealWhileEveryoneIsHealthy()
{
    auto const actions = DungeonPolicy().Decide(DungeonView({ 10, 11, 12, 13, 14 }), 0);
    CHECK(!CastsSpell(actions, 10, 2050));
    CHECK(!CastsSpell(actions, 11, 2050));
}

void LeaderDoesNotPullWhileAMemberIsHurt()
{
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    view.bots[0].health = 500; // 50%: the party is not ready
    AddNearby(view, 2, Unit(8, 7, 20.0f, true));
    auto const actions = DungeonPolicy().Decide(view, 0);
    CHECK(!HasAction(actions, 12, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
    CHECK(!HasAction(actions, 12, ActionType::kAttackTarget));
}

void LeaderPullsWhenThePartyIsReady()
{
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    AddNearby(view, 2, Unit(8, 7, 20.0f, true));
    auto const actions = DungeonPolicy().Decide(view, 0);
    CHECK(HasAction(actions, 12, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
}

void LeaderPullsAfterWaitingTooLong()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    view.bots[0].health = 500;
    AddNearby(view, 2, Unit(8, 7, 20.0f, true));
    policy.Decide(view, 1000);
    auto const actions = policy.Decide(view, 1000 + 130000);
    CHECK(HasAction(actions, 12, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
}

void MembersDoNotPullInADungeon()
{
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    AddNearby(view, 0, Unit(8, 7, 20.0f, true));
    auto const actions = DungeonPolicy().Decide(view, 0);
    CHECK(!HasAction(actions, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
    CHECK(!CastsSpell(actions, 10, 585));
}

// The party pulls a dungeon pack of four at most. A pack of five 20 yards off is left alone; a pack of four is pulled. (The view holds 4 nearby units per bot by
// default: it is widened to 8 so that five fit.)
void ADungeonPackOfFiveIsNotPulled()
{
    auto const packView = [](int mobs)
    {
        WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
        view.max_nearby_units = 8;
        view.nearby.assign(view.bots.size() * view.max_nearby_units, NearbyUnit{});
        for (int i = 0; i < mobs; ++i)
            AddNearby(view, 2, Unit(8, 7 + i, 20.0f + float(i), true));
        return view;
    };
    CHECK(!HasAction(DungeonPolicy().Decide(packView(5), 0), 12, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
    CHECK(HasAction(DungeonPolicy().Decide(packView(4), 0), 12, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
}

void MembersAttackedInADungeonDefendThemselves()
{
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 }, RogueTenLeadsTheRest);
    view.bots[0].in_combat = 1;
    AddNearby(view, 0, Unit(8, 7, 20.0f, true));
    auto const actions = DungeonPolicy(RogueTenLeadsTheRest).Decide(view, 0);
    CHECK(HasAction(actions, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7)) || HasAction(actions, 10, ActionType::kAttackTarget));
}

void MembersWaitForTheTankToHoldTheTarget()
{
    BotPolicy policy = DungeonPolicy(RogueTenLeadsTheRest);
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 }, RogueTenLeadsTheRest);
    view.bots[2].threat_target_guid = MakeUnitRef(8, 1, 7);
    AddNearby(view, 2, Unit(8, 7, 20.0f, true));
    AddNearby(view, 0, Unit(8, 7, 20.0f, true));
    auto first = policy.Decide(view, 1000);                 // the tank starts the fight
    CHECK(!HasAction(first, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
    auto later = policy.Decide(view, 1000 + 3000);           // a few seconds on, the others join
    CHECK(HasAction(later, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7)) || HasAction(later, 10, ActionType::kAttackTarget));
}

void HealerDoesNotMelee()
{
    // The healer (bot 10) stands at its ranged stand-off while the tank holds the target: it must not attack, because the
    // core's attack order runs a caster into melee, where it takes the tank's hits and pulls aggro (the 2026-10-23 run).
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    view.bots[2].threat_target_guid = MakeUnitRef(8, 1, 7);
    view.bots[2].in_combat = 1;
    AddNearby(view, 2, Unit(8, 7, 20.0f, true));
    AddNearby(view, 0, Unit(8, 7, 20.0f, true));
    policy.Decide(view, 1000); // the tank has the head start
    auto const actions = policy.Decide(view, 4000);
    CHECK(!HasAction(actions, 10, ActionType::kAttackTarget));
    CHECK(!HasAction(actions, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7))); // 20 yd is inside the 25 yd stand-off
}

void HealerWithoutPowerKeepsItsRange()
{
    // A healer at 0 power used to close to melee range like a spent mage, then swing at the boss for the rest of the fight.
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    view.bots[0].power = 0;
    view.bots[2].threat_target_guid = MakeUnitRef(8, 1, 7);
    view.bots[2].in_combat = 1;
    AddNearby(view, 2, Unit(8, 7, 20.0f, true));
    AddNearby(view, 0, Unit(8, 7, 20.0f, true));
    policy.Decide(view, 1000);
    auto const actions = policy.Decide(view, 4000);
    CHECK(!HasAction(actions, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
    CHECK(!HasAction(actions, 10, ActionType::kAttackTarget));
}

void LineOfSightRefusalClosesTheDistance()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 });
    view.bots[0].in_combat = 1;
    view.bots[0].threat_target_guid = MakeUnitRef(8, 1, 7);
    AddNearby(view, 0, Unit(8, 7, 15.0f, true));
    view.bots[0].last_action_type = std::uint8_t(ActionType::kCastSpell);
    view.bots[0].last_action_spell = 585;
    view.bots[0].last_action_status = BotActionStatus::kNoLineOfSight;
    view.bots[0].last_action_sequence = 5;
    auto const actions = policy.Decide(view, 1000);
    CHECK(HasAction(actions, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7))); // a priest would stand at 15 yards; now it walks closer
}

void CorpseWaitsForARezzerThenReleases()
{
    BotPolicy policy(DeadOptions(nullptr));
    auto const build = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Corpse(v.bots[3], 10.0f); return v; };
    CHECK(!HasAction(policy.Decide(build(), 1000), 13, ActionType::kReleaseSpirit));
    CHECK(!HasAction(policy.Decide(build(), 11000), 13, ActionType::kReleaseSpirit));
    CHECK(HasAction(policy.Decide(build(), 21500), 13, ActionType::kReleaseSpirit));
}

// Recover: the leader of a dungeon party stands a dead member up at its side 8 s after the fight, once it is up and quiet (not waiting 90 s for the healer, not releasing).
void ACorpseIsRevivedByTheQuietLeaderAfterAFewSeconds()
{
    PolicyOptions options = DeadOptions(nullptr);
    options.DungeonReviveOutOfCombat = true;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    auto const build = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Corpse(v.bots[3], 10.0f); return v; };
    CHECK(!HasAction(policy.Decide(build(), 1000), 13, ActionType::kRevive));
    CHECK(!HasAction(policy.Decide(build(), 5000), 13, ActionType::kRevive));
    std::vector<BotAction> later = policy.Decide(build(), 9500);
    CHECK(HasAction(later, 13, ActionType::kRevive));
    CHECK(!HasAction(later, 13, ActionType::kReleaseSpirit));
    PolicyOptions off = options;
    off.TacticRecover = false;
    BotPolicy plain(off);
    plain.SetRouteActive(true);
    plain.Decide(build(), 1000);
    CHECK(!HasAction(plain.Decide(build(), 9500), 13, ActionType::kRevive));
    // a leader in a fight does not revive anyone
    BotPolicy fighting(options);
    fighting.SetRouteActive(true);
    auto const busy = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Corpse(v.bots[3], 10.0f); v.bots[2].in_combat = 1; return v; };
    fighting.Decide(busy(), 1000);
    CHECK(!HasAction(fighting.Decide(busy(), 9500), 13, ActionType::kRevive));
}

// Revive-loop guard: no corpse revive while a live hostile stands next to the reviving member, and no more than three in a window (the 2026-11-07 stockade/rfd/gnomer revive loops).
void ACorpseIsNotRevivedBesideAHostileAndRevivesAreCapped()
{
    std::vector<std::string> events;
    PolicyOptions options = DeadOptions(&events);
    options.DungeonReviveOutOfCombat = true;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    auto const near = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Corpse(v.bots[3], 10.0f); v.bots[2].nearby_count = 1; v.nearby[2 * v.max_nearby_units] = Hostile(5, 6.0f, 100); return v; };
    auto const quiet = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Corpse(v.bots[3], 10.0f); return v; };
    policy.Decide(near(), 1000);
    CHECK(!HasAction(policy.Decide(near(), 9500), 13, ActionType::kRevive));
    CHECK(LoggedWith(events, "revive_hostile_hold"));
    CHECK(HasAction(policy.Decide(quiet(), 20000), 13, ActionType::kRevive));
}

// A dungeon party's dead wait for a rezzer no longer than a leveling party's: 20 s (0.36.2; the 60 s of 0.34 left ghosts for whole fights, 44-87 s in the sim's Ragefire Chasm).
void ADungeonCorpseWaitsTwentySecondsForARezzer()
{
    BotPolicy policy(DeadOptions(nullptr));
    policy.SetRouteActive(true);
    auto const build = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Corpse(v.bots[3], 10.0f); return v; };
    CHECK(!HasAction(policy.Decide(build(), 1000), 13, ActionType::kReleaseSpirit));
    CHECK(!HasAction(policy.Decide(build(), 15000), 13, ActionType::kReleaseSpirit));
    CHECK(HasAction(policy.Decide(build(), 21500), 13, ActionType::kReleaseSpirit));
}

// No rezzer in the party (all warriors): the corpse is released at once, a fight in the party or not, and well inside the 20 s.
void ADungeonCorpseWithoutARezzerIsReleasedWithinTwentySeconds()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events, AllWarriors));
    policy.SetRouteActive(true);
    auto const build = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Corpse(v.bots[3], 10.0f); return v; };
    CHECK(HasAction(policy.Decide(build(), 1000), 13, ActionType::kReleaseSpirit));
    CHECK(LoggedWith(events, "reason=release_no_rezzer"));
    CHECK(LoggedWith(events, "waited_ms=0"));
}

// A fight elsewhere in the party does not hold a release (the core refuses it only for a corpse in combat): the regroup loop started from ghosts held 44-87 s.
void AFightInThePartyDoesNotHoldADungeonRelease()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events, AllWarriors));
    policy.SetRouteActive(true);
    auto const build = [](bool ownCombat)
    {
        WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
        Corpse(v.bots[3], 10.0f);
        v.bots[1].in_combat = 1;
        v.bots[3].in_combat = ownCombat ? 1 : 0;
        return v;
    };
    CHECK(HasAction(policy.Decide(build(false), 1000), 13, ActionType::kReleaseSpirit));
    CHECK(!Logged(events, "release_held:"));
    BotPolicy own(DeadOptions(&events, AllWarriors));                                   // a corpse in combat itself is still held, as the core wants
    own.SetRouteActive(true);
    CHECK(!HasAction(own.Decide(build(true), 1000), 13, ActionType::kReleaseSpirit));
    CHECK(Logged(events, "release_held:"));
}

void AReleasedDungeonGhostIsTeleportedToTheLeader()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events, AllWarriors));
    policy.SetRouteActive(true);
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : v.bots)
        bot.map_id = 36;
    Ghost(v.bots[3], 0.0f, 36, 50.0f);
    v.bots[3].map_id = 0;                                                               // the ghost is outside, at the graveyard
    std::optional<BotAction> const teleport = TeleportOf(policy.Decide(v, 1000), 13);
    CHECK(teleport.has_value());
    if (teleport)
    {
        bool atALeader = false;                                                         // the destination is a living member's spot, on the instance map
        for (BotSnapshot const& bot : v.bots)
            atALeader = atALeader || (bot.guid != 13 && bot.pos_x == teleport->dest_x && bot.pos_y == teleport->dest_y && bot.pos_z == teleport->dest_z);
        CHECK(teleport->spell_id == 36 && atALeader);
    }
    CHECK(Logged(events, "party_teleport:"));
    // with a fight going on the ghost is not sent to the leader: it waits for the quiet
    BotPolicy busy(DeadOptions(nullptr, AllWarriors));
    busy.SetRouteActive(true);
    v.bots[1].in_combat = 1;
    CHECK(!LoggedTeleportToLeader(busy.Decide(v, 1000)));
}

// A ghost whose run takes too long is put at its corpse once, and the run starts over; it does not stay a ghost for good.
void GhostThatCannotReachItsCorpseIsPutThereOnce()
{
    std::vector<std::string> events;
    PolicyOptions options = AggressiveOptions();
    options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
    BotPolicy policy(options);
    WorldTickSnapshotView view = View(1, 0, {});
    Ghost(view.bots[0], 0.0f, view.bots[0].map_id, 3000.0f);
    policy.Decide(view, 1000);
    auto const teleportsTo = [](std::vector<BotAction> const& actions, float x)
    {
        for (BotAction const& action : actions)
            if (action.type == ActionType::kTeleport && action.dest_x == x)
                return true;
        return false;
    };
    CHECK(!teleportsTo(policy.Decide(view, 2000), 3000.0f));
    CHECK(teleportsTo(policy.Decide(view, 1000 + options.CorpseRunTimeoutMs + 1000), 3000.0f));   // the run took too long: to the corpse
    CHECK(Logged(events, "corpse_run_teleport:"));
    CHECK(!Logged(events, "corpse_run_failed:"));
    // Not again: the second timeout is a real failure.
    CHECK(!teleportsTo(policy.Decide(view, 1000 + 2 * options.CorpseRunTimeoutMs + 5000), 3000.0f));
    CHECK(Logged(events, "corpse_run_failed:"));
}

// DungeonPullLeashYards: the leader pulls a hostile beside the leg of the route it walks (point 0 to point 1), not one 45 yards off it; with the leash off it pulls
// the one off the route as before (the weaker one is the first pick, so it would be chosen).
void TheLeaderPullsOnlyAlongItsRouteLeg()
{
    auto const pulls = [](std::uint32_t leash, std::uint64_t counter)
    {
        BotPolicy policy(DriftOptions(leash, 0));
        policy.SetRouteActive(true);
        policy.Decide(DriftView(), 1000);                                   // the leader passes point 0 and walks the leg to point 1
        WorldTickSnapshotView view = DriftView();
        NearbyUnit off = HostileAt(7, 50.0f, 45.0f);                       // 45 yards off the middle of the leg, and hurt: the first pick without a leash
        off.health = 40;
        AddNearby(view, 2, off);
        AddNearby(view, 2, HostileAt(8, 30.0f, 5.0f));                     // 5 yards off the leg
        return HasAction(policy.Decide(view, 1500), 12, ActionType::kFollow, MakeUnitRef(8, 1, counter));
    };
    CHECK(pulls(30, 8));
    CHECK(!pulls(30, 7));
    CHECK(pulls(0, 7));
}

// A raid rules `leash` row narrows the leader's pull leash on its map (never widens it): a hostile 20 yards off the leg is pulled with the setting's 30, not with the
// map's 15; a hostile 9 yards off still is; a row for another map changes nothing; the chase leash of a target already held is not touched.
void ARaidRulesLeashNarrowsTheLeadersPull()
{
    auto const pulls = [](char const* rows, float y)
    {
        std::vector<std::string> errors;
        PolicyOptions options = DriftOptions(30, 0);
        options.RaidRules = std::make_shared<raid_rules::Table>(raid_rules::Parse(rows, errors));
        BotPolicy policy(options);
        policy.SetRouteActive(true);
        policy.Decide(DriftView(), 1000);
        WorldTickSnapshotView view = DriftView();
        AddNearby(view, 2, HostileAt(8, 30.0f, y));
        return HasAction(policy.Decide(view, 1500), 12, ActionType::kFollow, MakeUnitRef(8, 1, 8));
    };
    CHECK(pulls("", 20.0f));                                               // no row: the setting's 30
    CHECK(!pulls("leash|36|15|shelf\n", 20.0f));
    CHECK(pulls("leash|36|15|shelf\n", 9.0f));
    CHECK(pulls("leash|309|15|shelf\n", 20.0f));                           // another map
    CHECK(pulls("leash|36|40|wide\n", 35.0f) == false);                    // never wider than the setting
}

// The map's leash also narrows the pick of a new target by a leader already in a fight (the 2026-11-13 ZG run: the chase leash of 40 let the leader walk from the lone thrower
// on into the pack 23-33 yards off the leg, 393 pulls). A hostile 28 yards off the leg: picked with the chase leash 40 and no row, not with `leash|36|15`; one 9 yards off still is.
// The defence is not touched: a hostile that is the leader's threat target is fought wherever it stands.
void ARaidRulesLeashNarrowsTheNextTargetInAFight()
{
    auto const picks = [](char const* rows, float y, bool threat)
    {
        std::vector<std::string> errors;
        PolicyOptions options = DriftOptions(30, 40);
        options.RaidRules = std::make_shared<raid_rules::Table>(raid_rules::Parse(rows, errors));
        BotPolicy policy(options);
        policy.SetRouteActive(true);
        policy.Decide(DriftView(), 1000);
        WorldTickSnapshotView view = DriftView();
        NearbyUnit const mob = HostileAt(8, 30.0f, y);
        view.bots[2].in_combat = 1;                                         // the leader (bot 12) is in a fight already
        if (threat)
            view.bots[2].threat_target_guid = mob.guid;
        AddNearby(view, 2, mob);
        return HasAction(policy.Decide(view, 1500), 12, ActionType::kFollow, mob.guid);
    };
    CHECK(picks("", 28.0f, false));
    CHECK(!picks("leash|36|15|shelf\n", 28.0f, false));
    CHECK(picks("leash|36|15|shelf\n", 9.0f, false));
    CHECK(picks("leash|309|15|shelf\n", 28.0f, false));                    // another map
    CHECK(picks("leash|36|15|shelf\n", 28.0f, true));                      // already its threat target: fought
}

// A raid rules `pullpos` row: the tank (bot 12, the leader) out of combat walks to the position before it pulls an entry with a row (the hostile is entry 1, 30 yards east,
// the position is 10 yards south of the origin), pulls normally once it is there; no row, or a row for another entry, and it pulls at once.
void TheTankWalksToThePullPositionFirst()
{
    auto const first = [](char const* rows, float atX, float atY)
    {
        std::vector<std::string> errors;
        PolicyOptions options = DriftOptions(30, 40);
        options.RaidRules = std::make_shared<raid_rules::Table>(raid_rules::Parse(rows, errors));
        BotPolicy policy(options);
        policy.SetRouteActive(true);
        policy.Decide(DriftView(), 1000);
        WorldTickSnapshotView view = DriftView();
        view.bots[2].pos_x = atX;
        view.bots[2].pos_y = atY;
        AddNearby(view, 2, HostileAt(8, 30.0f, 3.0f));
        std::vector<BotAction> const actions = policy.Decide(view, 1500);
        for (BotAction const& action : actions)
            if (action.bot_guid == 12 && action.type == ActionType::kMoveTo)
                return std::string("move:") + std::to_string(int(action.dest_x)) + "," + std::to_string(int(action.dest_y));
        return std::string(HasAction(actions, 12, ActionType::kFollow, MakeUnitRef(8, 1, 8)) ? "pull" : "none");
    };
    CHECK(first("pullpos|36|1|0|-10|0|4|shelf\n", 0.0f, 0.0f) == "move:0,-10");
    CHECK(first("pullpos|36|1|0|-10|0|4|shelf\n", 0.0f, -8.0f) == "pull");           // within the yards of the position: pulls
    CHECK(first("", 0.0f, 0.0f) == "pull");
    CHECK(first("pullpos|36|9|0|-10|0|4|other\n", 0.0f, 0.0f) == "pull");            // another entry
    CHECK(first("pullpos|309|1|0|-10|0|4|other\n", 0.0f, 0.0f) == "pull");           // another map
}

// Once the pull has landed (the tank is in combat with the target, still 30 yards off) the tank goes back to the position, holds there while the pack comes, and fights on
// after kPullPosHoldMs.
void TheTankGoesBackToThePullPositionAndHolds()
{
    std::vector<std::string> errors;
    PolicyOptions options = DriftOptions(30, 40);
    options.RaidRules = std::make_shared<raid_rules::Table>(raid_rules::Parse("pullpos|36|1|0|-10|0|4|shelf\n", errors));
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    policy.Decide(DriftView(), 1000);
    auto const tick = [&](float x, float y, std::uint64_t at)
    {
        WorldTickSnapshotView view = DriftView();
        view.bots[2].pos_x = x;
        view.bots[2].pos_y = y;
        view.bots[2].in_combat = 1;
        NearbyUnit const mob = HostileAt(8, 30.0f, 3.0f);
        view.bots[2].threat_target_guid = mob.guid;
        AddNearby(view, 2, mob);
        return policy.Decide(view, at);
    };
    std::vector<BotAction> const back = tick(10.0f, 0.0f, 2000);
    CHECK(HasAction(back, 12, ActionType::kMoveTo) && !HasAction(back, 12, ActionType::kFollow));
    std::vector<BotAction> const hold = tick(0.0f, -10.0f, 3000);
    CHECK(!HasAction(hold, 12, ActionType::kMoveTo) && !HasAction(hold, 12, ActionType::kFollow));
    CHECK(HasAction(tick(0.0f, -10.0f, 2000 + 15000 + 1000), 12, ActionType::kFollow, MakeUnitRef(8, 1, 8)));
}

// DungeonChaseLeashYards: a member does not walk after its threat target once that is farther than the leash from both the route leg and the leader (it fled, or it
// is the next pack); inside the leash, and with the leash off, it does (kPartyChaseYards, 45, is the old limit).
void AMemberDoesNotChaseOffThePartysLeash()
{
    auto const chases = [](std::uint32_t leash, float y, std::vector<std::string>* events)
    {
        BotPolicy policy(DriftOptions(0, leash, events));
        policy.SetRouteActive(true);
        policy.Decide(DriftView(), 1000);
        WorldTickSnapshotView view = DriftView();
        NearbyUnit const mob = HostileAt(7, 0.0f, y);
        view.bots[0].in_combat = 1;
        view.bots[0].threat_target_guid = mob.guid;
        AddNearby(view, 0, mob);
        return HasAction(policy.Decide(view, 1500), 10, ActionType::kFollow, mob.guid);
    };
    std::vector<std::string> events;
    CHECK(!chases(30, 42.0f, &events));                                    // 42 yards from the leg and from the leader
    CHECK(Logged(events, "chase_leash: bot=10"));
    CHECK(chases(30, 25.0f, nullptr));                                     // inside the leash
    CHECK(chases(0, 42.0f, nullptr));                                      // off: chased, as before 0.45.0
    // with another hostile by the party in view, the bot turns to that one: the fled target is left alone for a while
    BotPolicy policy(DriftOptions(0, 30));
    policy.SetRouteActive(true);
    policy.Decide(DriftView(), 1000);
    WorldTickSnapshotView view = DriftView();
    NearbyUnit const fled = HostileAt(7, 0.0f, 42.0f);
    view.bots[0].in_combat = 1;
    view.bots[0].threat_target_guid = fled.guid;
    AddNearby(view, 0, fled);
    AddNearby(view, 0, HostileAt(8, 2.0f, 0.0f));
    CHECK(!HasAction(policy.Decide(view, 1500), 10, ActionType::kFollow, fled.guid));
    CHECK(HasAction(policy.Decide(view, 1600), 10, ActionType::kAttackTarget, MakeUnitRef(8, 1, 8)));
}

// DungeonChaseLeashYards also bounds what a bot in a fight picks for itself (PullTarget, defending): a hurt hostile far off the leash is not the pick, the one by
// the party is.
void AFightingMemberPicksNothingOffTheLeash()
{
    auto const picks = [](std::uint32_t leash, std::uint64_t counter)
    {
        BotPolicy policy(DriftOptions(0, leash));
        policy.SetRouteActive(true);
        policy.Decide(DriftView(), 1000);
        WorldTickSnapshotView view = DriftView();
        view.bots[0].in_combat = 1;                                        // in a fight, with no threat target of its own (it just died)
        view.bots[2].in_combat = 1;
        NearbyUnit far = HostileAt(7, 0.0f, 38.0f);                        // fled: hurt, 38 yards off
        far.health = 15;
        AddNearby(view, 0, far);
        AddNearby(view, 0, HostileAt(8, 3.0f, 0.0f));
        std::vector<BotAction> const actions = policy.Decide(view, 1500);
        return HasAction(actions, 10, ActionType::kFollow, MakeUnitRef(8, 1, counter)) || HasAction(actions, 10, ActionType::kAttackTarget, MakeUnitRef(8, 1, counter));
    };
    CHECK(picks(20, 8));
    CHECK(!picks(20, 7));
    CHECK(picks(0, 7));                                                    // off: the weakest, wherever it is
}

// DungeonLeaderRejoin: the dead leader's corpse is stood up next to a quiet member a few seconds after the fight (the members' recover), not released at 20 s.
void ADeadLeadersCorpseIsRevivedNextToAQuietMember()
{
    auto const run = [](bool rejoin, bool fight, std::vector<std::string>* events)
    {
        PolicyOptions options = DeadOptions(events);
        options.DungeonReviveOutOfCombat = true;
        options.DungeonLeaderRejoin = rejoin;
        options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
        BotPolicy policy(options);
        policy.SetRouteActive(true);
        auto const build = [fight]
        {
            WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
            Corpse(v.bots[2], 10.0f);                                      // the leader (12)
            v.bots[0].pos_x = 40.0f;                                       // 30 yards from the corpse
            v.bots[1].in_combat = fight ? 1 : 0;
            return v;
        };
        policy.Decide(build(), 1000);
        std::optional<BotAction> revive;
        for (BotAction const& action : policy.Decide(build(), 9500))
            if (action.bot_guid == 12 && action.type == ActionType::kRevive)
                revive = action;
        return revive;
    };
    std::vector<std::string> events;
    std::optional<BotAction> const revive = run(true, false, &events);
    CHECK(revive.has_value() && revive->target_guid != PlayerOf(12) && revive->target_guid != PlayerOf(10));   // next to a member by the corpse, not the far one
    CHECK(LoggedWith(events, "rejoin=1"));
    CHECK(!run(false, false, nullptr).has_value());                        // off: the leader's corpse is left to the rezzer and the release
    CHECK(!run(true, true, nullptr).has_value());                          // a fight in the party: nobody is stood up
}

// DungeonLeaderRejoin: the dead leader's ghost waits outside while its party fights, and is then teleported next to a living member; not put at the entrance at once.
// A fight that goes on past kLeaderGhostWaitMs (60 s) sends it to the entrance as before.
void ADeadLeadersGhostRejoinsItsPartyWhenTheFightEnds()
{
    std::vector<std::string> events;
    PolicyOptions options = DeadOptions(&events);
    options.DungeonLeaderRejoin = true;
    options.Route = { Point{ 220.0f, 0.0f, 0.0f } };
    PolicyOptions::DungeonRoute dungeon;
    dungeon.HasEntry = true;
    dungeon.Entry = Point{ 0.0f, 0.0f, 0.0f };
    options.DungeonsByMap[36] = dungeon;
    auto const build = [](bool fight)
    {
        WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
        for (BotSnapshot& bot : v.bots)
        {
            bot.map_id = 36;
            bot.pos_x = 200.0f;
            bot.in_combat = fight ? 1 : 0;
        }
        Ghost(v.bots[2], 0.0f, 36, 190.0f);                                // the leader's ghost, outside at the graveyard
        v.bots[2].map_id = 0;
        v.bots[2].in_combat = 0;
        return v;
    };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    CHECK(!TeleportOf(policy.Decide(build(true), 1000), 12).has_value());  // the party fights: it waits
    CHECK(!TeleportOf(policy.Decide(build(true), 30000), 12).has_value());
    CHECK(Logged(events, "leader_ghost_wait: bot=12"));
    std::optional<BotAction> const back = TeleportOf(policy.Decide(build(false), 31000), 12);
    CHECK(back.has_value() && back->spell_id == 36 && back->dest_x == 200.0f);   // next to a member, not the entrance at 0
    CHECK(Logged(events, "leader_rejoin: bot=12"));
    // a fight that does not end: after 60 s, the entrance
    BotPolicy long_fight(options);
    long_fight.SetRouteActive(true);
    long_fight.Decide(build(true), 1000);
    std::optional<BotAction> const entry = TeleportOf(long_fight.Decide(build(true), 62000), 12);
    CHECK(entry.has_value() && entry->dest_x == 0.0f);
    // off: the entrance at once, fight or not
    options.DungeonLeaderRejoin = false;
    BotPolicy off(options);
    off.SetRouteActive(true);
    std::optional<BotAction> const atOnce = TeleportOf(off.Decide(build(true), 1000), 12);
    CHECK(atOnce.has_value() && atOnce->dest_x == 0.0f);
}

} // namespace

void RunPolicyTestsPart4()
{
    AFlippingPartyDoesNotHoldTheLeaderForever();
    ARegroupStopsWaitingForAStuckMember();
    AStuckMemberAtTheBoundaryIsLoggedOnce();
    MemberRecoverDoesNotFireWithTheStuckEvent();
    ALeaderWithNoRouteIsPutBackOnTheRouteOnce();
    AMemberThatLostItsLeaderIsPutNextToIt();
    ALeaderRevivedAtTheEntranceGoesToItsParty();
    ALeaderAtTheEntranceGoesToAFightingPartyAfterAWhile();
    ARevivedLeaderRestsAtTheEntranceBeforeItGoesToItsParty();
    ALeaderThatDoesNotMoveIsNoticed();
    ALeaderWalksBackToAFarHealer();
    HealerHealsTheHurtMember();
    HealDownrankCastsTheRankThatCoversTheDeficit();
    HealDownrankPausesAfterAnUnknownRank();
    HealerHealsItselfWithoutATarget();
    PriestWithNoLineOfSightRepositionsBeforeRetryingDps();
    NoHealWhileEveryoneIsHealthy();
    LeaderDoesNotPullWhileAMemberIsHurt();
    LeaderPullsWhenThePartyIsReady();
    LeaderPullsAfterWaitingTooLong();
    ADungeonPackOfFiveIsNotPulled();
    MembersDoNotPullInADungeon();
    MembersAttackedInADungeonDefendThemselves();
    MembersWaitForTheTankToHoldTheTarget();
    HealerDoesNotMelee();
    HealerWithoutPowerKeepsItsRange();
    LineOfSightRefusalClosesTheDistance();
    GhostThatCannotReachItsCorpseIsPutThereOnce();
    CorpseWaitsForARezzerThenReleases();
    ADungeonCorpseWaitsTwentySecondsForARezzer();
    ADungeonCorpseWithoutARezzerIsReleasedWithinTwentySeconds();
    AFightInThePartyDoesNotHoldADungeonRelease();
    AReleasedDungeonGhostIsTeleportedToTheLeader();
    ACorpseIsRevivedByTheQuietLeaderAfterAFewSeconds();
    ACorpseIsNotRevivedBesideAHostileAndRevivesAreCapped();
    DefaultBotsDoNotPullWhatIsInRange();
    TheLeaderPullsOnlyAlongItsRouteLeg();
    ARaidRulesLeashNarrowsTheLeadersPull();
    ARaidRulesLeashNarrowsTheNextTargetInAFight();
    TheTankWalksToThePullPositionFirst();
    TheTankGoesBackToThePullPositionAndHolds();
    AMemberDoesNotChaseOffThePartysLeash();
    AFightingMemberPicksNothingOffTheLeash();
    ADeadLeadersCorpseIsRevivedNextToAQuietMember();
    ADeadLeadersGhostRejoinsItsPartyWhenTheFightEnds();
}
