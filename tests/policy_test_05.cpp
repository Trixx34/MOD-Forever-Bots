// SPDX-License-Identifier: GPL-2.0-or-later
// Policy tests, part 5 of 8 (split out of policy_test.cpp; the helpers are in policy_test_support.h).
#include "policy_test_support.h"

namespace {

// The leader's own ghost (the tank) is never held by a Warder, near a living member or near its corpse: without the tank nobody can fight that Warder (2026-11-08: bot 1
// held 3-4 min after each death; bot 121 held 46 min by a Warder at its corpse). A member's ghost is still held. The leader comes back next to a member with no Warder
// inside its Fire Nova reach, even one farther from the corpse.
void ADeadLeadersGhostIsNotHeldByAWarderNearAMember()
{
    auto const run = [](float warderX, std::vector<std::string>& events, bool clearMember = false)
    {
        PolicyOptions options = DeadOptions(&events);
        options.DungeonLeaderRejoin = true;
        options.Route = { Point{ 220.0f, 0.0f, 0.0f } };
        PolicyOptions::DungeonRoute dungeon;
        dungeon.HasEntry = true;
        dungeon.Entry = Point{ 0.0f, 0.0f, 0.0f };
        options.DungeonsByMap[36] = dungeon;
        WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
        for (BotSnapshot& bot : v.bots)
        {
            bot.map_id = 36;
            bot.pos_x = 200.0f;
        }
        Ghost(v.bots[2], 0.0f, 36, 190.0f);                                // the leader's ghost, outside; its corpse at 190
        v.bots[2].map_id = 0;
        Ghost(v.bots[4], 0.0f, 36, 195.0f);                                // a member's ghost (14), its corpse at 195
        v.bots[4].map_id = 0;
        NearbyUnit warder = Hostile(2, warderX, 100);
        warder.guid = MakeUnitRef(8, 12129, 2);
        for (std::size_t i : { 0, 1, 3 })
            if (!(clearMember && i == 3))
                AddNearby(v, i, warder);
        if (clearMember)
            v.bots[3].pos_x = 300.0f;                                      // 13: 100 yards off, no Warder in its view
        BotPolicy policy(options);
        policy.SetRouteActive(true);
        return TeleportOf(policy.Decide(v, 40000), 12);   // past the first 30 s, so the hold is logged
    };
    std::vector<std::string> events;
    std::optional<BotAction> const back = run(220.0f, events);             // 20 yards from the members, 30 from the leader's corpse
    CHECK(back.has_value() && back->spell_id == 36);
    CHECK(!Logged(events, "revive_held: bot=12"));
    CHECK(Logged(events, "revive_held: bot=14"));                          // 25 yards from 14's corpse, but 20 from the members: still held
    std::vector<std::string> atCorpse;
    std::optional<BotAction> const fromCorpse = run(200.0f, atCorpse);     // 10 yards from the leader's corpse: not held either
    CHECK(fromCorpse.has_value() && fromCorpse->spell_id == 36);
    CHECK(!Logged(atCorpse, "revive_held: bot=12"));
    std::vector<std::string> clear;
    std::optional<BotAction> const toClear = run(200.0f, clear, true);     // the members by the corpse stand in the Warder: the clear one, farther off
    CHECK(toClear.has_value() && toClear->dest_x == 300.0f);
}

// A corpse does not release while a party rezzer is casting its resurrect on it (the 2026-11-07 sim cut 14 casts so), and releases once the cast is over or
// kRezCastMs past the release delay.
void ACorpseDoesNotReleaseUnderARunningResurrect()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events));
    auto const build = [](std::uint32_t casting)
    {
        WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
        Corpse(v.bots[3], 10.0f);
        v.bots[1].in_combat = 1;                                           // a fight holds every rez cast until the cast below
        v.bots[0].casting_spell = casting;
        return v;
    };
    policy.Decide(build(0), 1000);
    WorldTickSnapshotView quiet = build(0);
    quiet.bots[1].in_combat = 0;
    std::uint32_t rezSpell = 0;
    for (BotAction const& action : policy.Decide(quiet, 19000))            // the fight ends: the nearest priest (10) starts its resurrect on 13
        if (action.bot_guid == 10 && action.type == ActionType::kCastSpell && action.target_guid == PlayerOf(13))
            rezSpell = action.spell_id;
    CHECK(rezSpell != 0);
    WorldTickSnapshotView casting = build(rezSpell);
    casting.bots[1].in_combat = 0;
    CHECK(!HasAction(policy.Decide(casting, 21500), 13, ActionType::kReleaseSpirit));
    CHECK(LoggedWith(events, "reason=rez_casting"));
    CHECK(HasAction(policy.Decide(casting, 32500), 13, ActionType::kReleaseSpirit));   // capped
    BotPolicy other(DeadOptions(nullptr));
    other.Decide(build(0), 1000);
    CHECK(HasAction(other.Decide(build(0), 21500), 13, ActionType::kReleaseSpirit));    // nobody casting: the 20 s rule
}

// A corpse run that takes longer than CorpseRunTeleportMs (60 s, the owner's rule of 2026-10-25) ends in a teleport to the corpse; no party revive is asked
// for. The ghost walks first, and the teleport comes once.
void ALongCorpseRunEndsInATeleportToTheCorpse()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events));
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 0.0f, 0, 3000.0f);                                                  // the corpse is far away on the same map
    CHECK(HasAction(policy.Decide(v, 1000), 13, ActionType::kMoveTo));                   // the run starts on foot
    CHECK(!TeleportOf(policy.Decide(v, 30000), 13).has_value());
    CHECK(!TeleportOf(policy.Decide(v, 60500), 13).has_value());                         // not before 60 s
    std::vector<BotAction> const later = policy.Decide(v, 61500);
    std::optional<BotAction> const teleport = TeleportOf(later, 13);
    CHECK(teleport.has_value() && teleport->dest_x == 3000.0f);                           // to the corpse
    CHECK(!HasAction(later, 13, ActionType::kRevive));                                    // no revive order from the party
    for (BotAction const& action : later)
        CHECK(action.type != ActionType::kRevive);
    CHECK(Logged(events, "corpse_run_teleport:"));
    CHECK(LoggedWith(events, "reason=run_limit"));
    CHECK(!TeleportOf(policy.Decide(v, 70000), 13).has_value());                         // once

    // A ghost at its corpse past 60 s (the core's reclaim delay) is not on a run: no teleport.
    BotPolicy waiting(DeadOptions(nullptr));
    WorldTickSnapshotView there = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(there.bots[3], 0.0f, 0, 10.0f);
    there.bots[3].reclaim_wait_ms = 90000;
    waiting.Decide(there, 1000);
    CHECK(!TeleportOf(waiting.Decide(there, 61500), 13).has_value());

    // CorpseRunTeleportMs = 0: the rule is off, the ghost walks on.
    PolicyOptions off = DeadOptions(nullptr);
    off.CorpseRunTeleportMs = 0;
    BotPolicy plain(off);
    plain.Decide(v, 1000);
    CHECK(!TeleportOf(plain.Decide(v, 61500), 13).has_value());
}

// A wiped party (the sim's Deadmines run: five ghosts at the Sentinel Hill graveyard, the walk to the door refused for "no route") puts each ghost at its
// corpse after 60 s, with no party revive first.
void AWipedPartysGhostsArePutAtTheirCorpsesAfterAMinute()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events));
    policy.SetRouteActive(true);
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : v.bots)
        Ghost(bot, 0.0f, 36, 50.0f);                                                     // all outside on map 0, corpses in the dungeon
    std::vector<BotAction> const first = policy.Decide(v, 1000);
    CHECK(HasAction(first, 13, ActionType::kMoveTo) && !TeleportOf(first, 13).has_value()); // to the door first
    CHECK(!TeleportOf(policy.Decide(v, 50000), 13).has_value());
    // GhostTeleportPerSecond = 2: the five ghosts go two, two and one in successive seconds, each once.
    std::set<std::uint64_t> sent;
    for (std::uint64_t at : { 61500ull, 61700ull, 62600ull, 63700ull })
    {
        std::vector<BotAction> const later = policy.Decide(v, at);
        std::size_t inTick = 0;
        for (std::uint64_t guid : { 10, 11, 12, 13, 14 })
        {
            std::optional<BotAction> const teleport = TeleportOf(later, guid);
            if (!teleport)
                continue;
            ++inTick;
            CHECK(teleport->spell_id == 36 && teleport->dest_x == 50.0f);
            CHECK(sent.insert(guid).second);                                             // not twice
        }
        CHECK(inTick <= 2);
        CHECK(at != 61500ull || inTick == 2);
        CHECK(at != 61700ull || inTick == 0);                                            // the same second: no third
        for (std::uint64_t guid : { 10, 11, 12, 13, 14 })
            CHECK(!HasAction(later, guid, ActionType::kRevive));
    }
    CHECK(sent.size() == 5);
    CHECK(Logged(events, "corpse_run_teleport:"));
    CHECK(LoggedWith(events, "reason=run_limit"));
    CHECK(!TeleportOf(policy.Decide(v, 70000), 13).has_value());                         // once
}

// A ghost teleport the core refused (the instance it died in is gone) does not leave the ghost without a later try: RunTimeoutTeleported is cleared and the next
// teleport comes a minute after the refusal.
void ARefusedGhostTeleportIsTriedAgainLater()
{
    std::vector<std::string> events;
    PolicyOptions options = DeadOptions(&events);
    options.GhostTeleportPerSecond = 0;
    BotPolicy policy(options);
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 0.0f, 0, 3000.0f);
    policy.Decide(v, 1000);
    CHECK(TeleportOf(policy.Decide(v, 61500), 13).has_value());
    v.bots[3].last_action_type = std::uint8_t(ActionType::kTeleport);
    v.bots[3].last_action_status = forever_module::BotActionStatus::kRefused;
    v.bots[3].last_action_sequence = 7;
    std::vector<BotAction> const refused = policy.Decide(v, 62500);
    CHECK(!TeleportOf(refused, 13).has_value());
    CHECK(Logged(events, "corpse_run_teleport_refused:"));
    CHECK(!TeleportOf(policy.Decide(v, 100000), 13).has_value());                        // not before the minute is up again
    CHECK(TeleportOf(policy.Decide(v, 123000), 13).has_value());                         // tried again
}

// A bot that stands up among what killed it and dies again three times within two minutes is put at a grind spot for its level, away from the corpse.
void DeathLoopMovesTheBotToAGrindSpot(std::uint64_t cycleMs = 8000)
{
    std::vector<std::string> events;
    std::vector<std::string> warnings;
    std::istringstream spots("spot 0 40 0 0 1 10 0\nspot 0 5000 0 0 1 10 0\n");   // one spot beside the corpse, one far away
    PolicyOptions options = AggressiveOptions();
    options.Grind = std::make_shared<GrindData>(ParseGrindData(spots, warnings));
    options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
    BotPolicy policy(options);
    auto const escapeTo = [](std::vector<BotAction> const& actions)
    {
        for (BotAction const& action : actions)
            if (action.type == ActionType::kTeleport)
                return action.dest_x;
        return -1.0f;
    };
    std::uint64_t now = 1000;
    float escape = -1.0f;
    for (int death = 0; death < 3; ++death)
    {
        WorldTickSnapshotView dead = View(1, 0, {});
        dead.bots[0].level = 6;
        Corpse(dead.bots[0], 10.0f);
        policy.Decide(dead, now);
        now += 3000;
        WorldTickSnapshotView up = View(1, 0, {});
        up.bots[0].level = 6;
        up.bots[0].pos_x = 10.0f;
        escape = escapeTo(policy.Decide(up, now));
        now += cycleMs - 3000;
        if (death < 2)
            CHECK(escape < 0.0f);   // the first two deaths are ordinary
    }
    CHECK(escape == 5000.0f);       // the third: away, to the spot that is not beside the corpse
    CHECK(Logged(events, "death_loop_escape:"));
}

// Prod 2026-11-12: a bot that dies twice to the same creature entry within kDeathLoopMs is moved on at its second death, not its third.
void ASecondDeathToTheSameCreatureMovesTheBot(std::uint32_t secondKiller)
{
    std::vector<std::string> warnings;
    std::istringstream spots("spot 0 40 0 0 1 10 0\nspot 0 5000 0 0 1 10 0\n");
    PolicyOptions options = AggressiveOptions();
    options.Grind = std::make_shared<GrindData>(ParseGrindData(spots, warnings));
    BotPolicy policy(options);
    std::uint64_t now = 1000;
    bool teleported = false;
    for (int death = 0; death < 2; ++death)
    {
        std::uint32_t const entry = death == 0 ? 1998u : secondKiller;
        WorldTickSnapshotView dead = View(1, 0, { Hostile(entry, 3.0f, 100) });
        dead.nearby[0].guid = MakeUnitRef(8, entry, 1);
        dead.bots[0].level = 2;
        Corpse(dead.bots[0], 10.0f);
        policy.Decide(dead, now);
        now += 3000;
        WorldTickSnapshotView up = View(1, 0, {});
        up.bots[0].level = 2;
        up.bots[0].pos_x = 10.0f;
        for (BotAction const& action : policy.Decide(up, now))
            teleported = teleported || (death == 1 && action.type == ActionType::kTeleport && action.dest_x == 5000.0f);
        now += 60000;
    }
    CHECK(teleported == (secondKiller == 1998u));
}

// Prod 2026-11-12: a bot a reclaim brought up at 50% health pulled at once and died again. A solo bot out of combat under kRecoverPullPct rests first.
void AHurtSoloBotRestsBeforeItPulls()
{
    auto const build = [](std::uint32_t health, float mobX)
    {
        WorldTickSnapshotView view = View(1, 0, { Hostile(7, mobX, 100) });
        view.bots[0].health = health;
        view.bots[0].level = 3;
        view.nearby[0].level = 3;
        return view;
    };
    // 50% health, a creature 40 yards off (outside kRecoverSafeYards): no attack, and it sits down.
    {
        BotPolicy policy(AggressiveOptions());
        auto const actions = policy.Decide(build(500, 40.0f), 1000);
        CHECK(!HasAction(actions, kBot, ActionType::kAttackTarget));
        bool sits = false;
        for (BotAction const& action : actions)
            sits = sits || (action.type == ActionType::kRest && action.spell_id == 1);
        CHECK(sits);
    }
    // 50% health, a creature 10 yards off: the bot walks away from it, does not attack.
    {
        BotPolicy policy(AggressiveOptions());
        auto const actions = policy.Decide(build(500, 10.0f), 1000);
        CHECK(!HasAction(actions, kBot, ActionType::kAttackTarget));
        CHECK(HasAction(actions, kBot, ActionType::kMoveTo));
    }
    // Healthy: it pulls.
    {
        BotPolicy policy(AggressiveOptions());
        CHECK(HasAction(policy.Decide(build(1000, 5.0f), 1000), kBot, ActionType::kAttackTarget));
    }
    // The hold has a limit: after kRecoverHoldMs (150 s) of staying hurt the bot pulls anyway.
    {
        BotPolicy policy(AggressiveOptions());
        policy.Decide(build(500, 5.0f), 1000);
        CHECK(!HasAction(policy.Decide(build(500, 5.0f), 1000 + 149000ull), kBot, ActionType::kAttackTarget));
        CHECK(HasAction(policy.Decide(build(500, 5.0f), 1000 + 151000ull), kBot, ActionType::kAttackTarget));
    }
    // Attacked while hurt: it defends itself.
    {
        BotPolicy policy(AggressiveOptions());
        WorldTickSnapshotView view = build(500, 5.0f);
        view.bots[0].in_combat = 1;
        CHECK(HasAction(policy.Decide(view, 1000), kBot, ActionType::kAttackTarget));
    }
}

void CorpseReleasesAtOnceWithoutARezzer()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events, AllWarriors));
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Corpse(v.bots[3], 10.0f);
    CHECK(HasAction(policy.Decide(v, 1000), 13, ActionType::kReleaseSpirit));
    CHECK(Logged(events, "died:") && Logged(events, "release:"));
}

void CorpseReleasesWhenTheWholePartyIsDown()
{
    BotPolicy policy(DeadOptions(nullptr));
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    for (std::size_t i = 0; i < v.bots.size(); ++i)
        Corpse(v.bots[i], float(i));
    auto const actions = policy.Decide(v, 1000);
    for (std::uint64_t guid = 10; guid <= 14; ++guid)
        CHECK(HasAction(actions, guid, ActionType::kReleaseSpirit));
}

void ReleaseIsHeldInCombatUpToTheCap()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events, AllWarriors));
    auto const build = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Corpse(v.bots[3], 10.0f); v.bots[3].in_combat = 1; return v; };
    CHECK(!HasAction(policy.Decide(build(), 1000), 13, ActionType::kReleaseSpirit));
    CHECK(!HasAction(policy.Decide(build(), 30000), 13, ActionType::kReleaseSpirit));
    CHECK(Logged(events, "release_held:"));
    CHECK(HasAction(policy.Decide(build(), 62000), 13, ActionType::kReleaseSpirit));
}

void GhostWalksToItsCorpseAndIgnoresCombat()
{
    BotPolicy policy(DeadOptions(nullptr));
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 200.0f, 0, 0.0f);
    v.bots[3].in_combat = 1;
    AddNearby(v, 3, Unit(8, 2, 5.0f, true));
    auto const actions = policy.Decide(v, 1000);
    CHECK(HasAction(actions, 13, ActionType::kMoveTo));
    CHECK(!HasAction(actions, 13, ActionType::kAttackTarget));
}

void GhostOnTheOutsideMapWalksToThePortal()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events));
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 0.0f, 36, 50.0f);
    auto const actions = policy.Decide(v, 1000);
    bool towardPortal = false;
    for (BotAction const& action : actions)
        towardPortal = towardPortal || (action.type == ActionType::kMoveTo && action.bot_guid == 13 && action.dest_x == 500.0f);
    CHECK(towardPortal);
}

// A dead member left on the outside map with its corpse in the dungeon and no portal (run 9): it is teleported to the corpse, paced, and walks on from there.
void ADeadMemberOnMapZeroIsTeleportedBackToItsCorpse()
{
    std::vector<std::string> events;
    PolicyOptions options = DeadOptions(&events);
    options.HasPortal = false;
    BotPolicy policy(options);
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 0.0f, 36, 50.0f);
    v.bots[3].map_id = 0;
    std::optional<BotAction> teleport = TeleportOf(policy.Decide(v, 1000), 13);
    CHECK(teleport.has_value() && teleport->spell_id == 36 && teleport->dest_x == 50.0f); // to the corpse's map and place
    CHECK(Logged(events, "corpse_teleport:"));
    CHECK(!TeleportOf(policy.Decide(v, 2000), 13).has_value());                             // not again at once
    CHECK(TeleportOf(policy.Decide(v, 16000), 13).has_value());                             // again if the core did not apply it
    v.bots[3].map_id = 36;                                                                  // now on the corpse's map: the corpse run goes on
    std::vector<BotAction> onFoot = policy.Decide(v, 40000);
    CHECK(HasAction(onFoot, 13, ActionType::kMoveTo) && !TeleportOf(onFoot, 13).has_value());
    CHECK(!Logged(events, "corpse_run_failed:"));
}

// A dead member is revived by its leader out of combat only in the dungeon it died in (the owner's rule of 2026-10-22: a ghost walks back to
// the instance and is revived on entering it).
void ADeadMemberIsRevivedByItsLeaderInsideTheDungeon()
{
    std::vector<std::string> events;
    PolicyOptions options = DeadOptions(&events);
    options.DungeonReviveOutOfCombat = true;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 0.0f, 36, 50.0f);
    v.bots[3].map_id = 36;                                                                // back inside, at its corpse's map
    std::vector<BotAction> first = policy.Decide(v, 1000);
    CHECK(HasAction(first, 13, ActionType::kRevive));                                     // the leader is up and quiet
    CHECK(!HasAction(first, 13, ActionType::kMoveTo));                                    // and the ghost waits for it, not walking off
    CHECK(Logged(events, "revive_out_of_combat:"));
    CHECK(!HasAction(policy.Decide(v, 2000), 13, ActionType::kRevive));                   // not again before the retry
    CHECK(HasAction(policy.Decide(v, 11000), 13, ActionType::kRevive));                   // again if the core refused it
    CHECK(HasAction(policy.Decide(v, 22000), 13, ActionType::kRevive));                   // third try
    std::vector<BotAction> given = policy.Decide(v, 33000);
    CHECK(!HasAction(given, 13, ActionType::kRevive));                                    // given up after three tries
    CHECK(!HasAction(given, 13, ActionType::kMoveTo));                                    // and it stays by the leader: no corpse run (0.34.3)

    BotPolicy busy(options);
    busy.SetRouteActive(true);
    WorldTickSnapshotView fighting = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(fighting.bots[3], 0.0f, 36, 50.0f);
    fighting.bots[3].map_id = 36;
    fighting.bots[2].in_combat = 1;                                                       // the leader (bot 12) fights: no revive
    CHECK(!HasAction(busy.Decide(fighting, 1000), 13, ActionType::kRevive));
}

// A ghost of a dungeon party still on the outside map is not revived there (it stood up at the graveyard and died outside the dungeon);
// out of combat it is teleported to its leader inside the instance (0.34.3); it is still not revived outside.
void AGhostOnTheOutsideMapIsNotRevivedBeforeItEnters()
{
    std::vector<std::string> events;
    PolicyOptions options = DeadOptions(&events);
    options.DungeonReviveOutOfCombat = true;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 0.0f, 36, 50.0f);
    v.bots[3].map_id = 0;                                                                 // the graveyard outside
    v.bots[2].map_id = 36;                                                                // the leader (bot 12) is inside the instance
    std::vector<BotAction> first = policy.Decide(v, 1000);
    CHECK(!HasAction(first, 13, ActionType::kRevive));
    CHECK(!Logged(events, "revive_out_of_combat:"));
    bool toLeader = false;
    for (BotAction const& action : first)
        toLeader = toLeader || (action.type == ActionType::kTeleport && action.bot_guid == 13 && action.spell_id == 36);
    CHECK(toLeader);                                                                     // teleported to the leader in the instance
    CHECK(!HasAction(policy.Decide(v, 12000), 13, ActionType::kRevive));                 // still outside: still not revived
}

// A living member of a dungeon party left on the outside map (a wipe put it up at the graveyard) walks back to the instance door and starts
// no fight on the way: it must not be able to die outside the dungeon (the owner's rule of 2026-10-22). A bot that stopped to clear the
// road was killed by the stable master of Sentinel Hill and the rescue teleport was refused in combat behind it.
void AnAliveMemberOutsideWalksBackToTheDungeon()
{
    std::vector<std::string> events;
    PolicyOptions options = DeadOptions(&events);
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : v.bots)
        bot.map_id = 0;                                                                  // the whole party up at the graveyard outside
    v.bots[3].pos_x = 10.0f;
    AddNearby(v, 3, Hostile(7, 15.0f, 100));                                            // something worth a fight on the way
    std::vector<BotAction> actions = policy.Decide(v, 1000);                            // this tick it mounts for the walk
    std::vector<BotAction> more = policy.Decide(v, 2000);
    actions.insert(actions.end(), more.begin(), more.end());
    CHECK(!HasAction(actions, 13, ActionType::kAttackTarget));
    bool towardPortal = false;
    for (BotAction const& action : actions)
        towardPortal = towardPortal || (action.type == ActionType::kMoveTo && action.bot_guid == 13 && action.dest_x == 500.0f);
    CHECK(towardPortal);
}

// A ghost at its corpse with a live hostile within 30 yards waits (a ghost cannot be killed, the reclaimed body can); after two minutes it reclaims anyway.
void AGhostDoesNotReclaimNextToAHostile()
{
    BotPolicy policy(DeadOptions(nullptr));
    auto const build = [](bool hostile)
    {
        WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
        Ghost(v.bots[3], 0.0f, 0, 10.0f);
        v.bots[3].reclaim_wait_ms = 0;
        if (hostile)
            AddNearby(v, 3, Hostile(7, 12.0f, 100));
        return v;
    };
    CHECK(!HasAction(policy.Decide(build(true), 1000), 13, ActionType::kReclaimCorpse));
    CHECK(!HasAction(policy.Decide(build(true), 60000), 13, ActionType::kReclaimCorpse));
    CHECK(HasAction(policy.Decide(build(false), 61000), 13, ActionType::kReclaimCorpse));   // the hostile left: back to life
    PolicyOptions stubbornOptions = DeadOptions(nullptr);
    stubbornOptions.SpiritHealerAfterMs = 0;                   // the spirit healer off: the old hold of two minutes (SpiritHealerAfterAHostileStaysNearTheCorpse has the new rule)
    BotPolicy stubborn(stubbornOptions);
    CHECK(!HasAction(stubborn.Decide(build(true), 1000), 13, ActionType::kReclaimCorpse));
    CHECK(HasAction(stubborn.Decide(build(true), 122000), 13, ActionType::kReclaimCorpse)); // two minutes: it waits no longer
}

// Protocol 28: kResurrect spell_id 2 asks the core to take the ghost to the spirit healer.
bool SpiritHealerAsked(std::vector<BotAction> const& actions, std::uint64_t bot)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kResurrect && action.bot_guid == bot && action.spell_id == 2)
            return true;
    return false;
}

WorldTickSnapshotView GhostAtCorpse(bool hostile)
{
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 0.0f, 0, 10.0f);
    v.bots[3].reclaim_wait_ms = 0;
    if (hostile)
        AddNearby(v, 3, Hostile(7, 12.0f, 100));
    return v;
}

// A ghost whose corpse a live hostile camps for SpiritHealerAfterMs (45 s) takes the spirit healer, asked again every 3 s, and never reclaims next to the hostile.
void SpiritHealerAfterAHostileStaysNearTheCorpse()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events));
    CHECK(!SpiritHealerAsked(policy.Decide(GhostAtCorpse(true), 1000), 13));
    CHECK(!SpiritHealerAsked(policy.Decide(GhostAtCorpse(true), 30000), 13));              // 29 s: still waiting
    std::vector<BotAction> const taken = policy.Decide(GhostAtCorpse(true), 47000);        // 46 s of a hostile near the corpse
    CHECK(SpiritHealerAsked(taken, 13));
    CHECK(!HasAction(taken, 13, ActionType::kReclaimCorpse));
    CHECK(Logged(events, "spirit_healer: bot=13"));
    CHECK(!SpiritHealerAsked(policy.Decide(GhostAtCorpse(true), 48000), 13));              // asked again after 3 s, not before
    std::vector<BotAction> const again = policy.Decide(GhostAtCorpse(true), 50500);
    CHECK(SpiritHealerAsked(again, 13));
    CHECK(!HasAction(again, 13, ActionType::kStop));                                       // no stop: the core walks the ghost to the healer
    CHECK(!HasAction(policy.Decide(GhostAtCorpse(true), 60000), 13, ActionType::kReclaimCorpse));
}

void SpiritHealerIsNotTakenWithoutAHostile()
{
    BotPolicy policy(DeadOptions(nullptr));
    CHECK(!SpiritHealerAsked(policy.Decide(GhostAtCorpse(false), 1000), 13));
    CHECK(!SpiritHealerAsked(policy.Decide(GhostAtCorpse(false), 100000), 13));
    CHECK(HasAction(policy.Decide(GhostAtCorpse(false), 104000), 13, ActionType::kReclaimCorpse));   // the reclaim of 100 s was sent: retried after 3 s
    PolicyOptions off = DeadOptions(nullptr);
    off.SpiritHealerAfterMs = 0;
    off.SpiritHealerOnRepeat = false;
    BotPolicy never(off);
    never.Decide(GhostAtCorpse(true), 1000);
    CHECK(!SpiritHealerAsked(never.Decide(GhostAtCorpse(true), 100000), 13));              // both rules off
}

// The second death to the same creature entry within five minutes takes the spirit healer at once (a hostile near the corpse), without the 45 s wait.
void SpiritHealerOnARepeatedKiller()
{
    BotPolicy policy(DeadOptions(nullptr));
    policy.Decide(GhostAtCorpse(true), 1000);                                              // first death to creature 7
    policy.Decide(PartyView({ 10, 11, 12, 13, 14 }), 2000);                                // alive again
    CHECK(SpiritHealerAsked(policy.Decide(GhostAtCorpse(true), 100000), 13));              // second death, 99 s later, same killer
    PolicyOptions noRepeat = DeadOptions(nullptr);
    noRepeat.SpiritHealerOnRepeat = false;
    BotPolicy other(noRepeat);
    other.Decide(GhostAtCorpse(true), 1000);
    other.Decide(PartyView({ 10, 11, 12, 13, 14 }), 2000);
    CHECK(!SpiritHealerAsked(other.Decide(GhostAtCorpse(true), 100000), 13));
    BotPolicy late(DeadOptions(nullptr));                                                  // the same killer, but six minutes later
    late.Decide(GhostAtCorpse(true), 1000);
    late.Decide(PartyView({ 10, 11, 12, 13, 14 }), 2000);
    CHECK(!SpiritHealerAsked(late.Decide(GhostAtCorpse(true), 400000), 13));
}

// A refused answer (the core's key is off, a hardcore realm) puts the ghost back on the old hold and reclaim for two minutes.
void ARefusedSpiritHealerFallsBackToTheReclaim()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events));
    policy.Decide(GhostAtCorpse(true), 1000);
    CHECK(SpiritHealerAsked(policy.Decide(GhostAtCorpse(true), 47000), 13));
    WorldTickSnapshotView refused = GhostAtCorpse(true);
    refused.bots[3].last_action_sequence = 5;
    refused.bots[3].last_action_type = uint8_t(ActionType::kResurrect);
    refused.bots[3].last_action_spell = 2;
    refused.bots[3].last_action_status = BotActionStatus::kRefused;
    std::vector<BotAction> const next = policy.Decide(refused, 50500);
    CHECK(!SpiritHealerAsked(next, 13));
    CHECK(Logged(events, "death_action_refused: bot=13 action=spirit_healer"));
    CHECK(!SpiritHealerAsked(policy.Decide(GhostAtCorpse(true), 100000), 13));             // not asked again while the refusal lasts
    CHECK(!HasAction(policy.Decide(GhostAtCorpse(true), 100000), 13, ActionType::kReclaimCorpse)); // the hold of 120 s since 1 s goes on
    CHECK(HasAction(policy.Decide(GhostAtCorpse(true), 125000), 13, ActionType::kReclaimCorpse));  // the old rule: the hold ended, it reclaims
    // kMoving (walking to the healer) is no refusal
    BotPolicy walking(DeadOptions(nullptr));
    walking.Decide(GhostAtCorpse(true), 1000);
    walking.Decide(GhostAtCorpse(true), 47000);
    WorldTickSnapshotView moving = GhostAtCorpse(true);
    moving.bots[3].last_action_sequence = 6;
    moving.bots[3].last_action_type = uint8_t(ActionType::kResurrect);
    moving.bots[3].last_action_spell = 2;
    moving.bots[3].last_action_status = BotActionStatus::kMoving;
    walking.Decide(moving, 50500);
    CHECK(SpiritHealerAsked(walking.Decide(GhostAtCorpse(true), 54000), 13));
}

void GhostReclaimsInRadiusAndRetries()
{
    BotPolicy policy(DeadOptions(nullptr));
    auto const build = [](std::uint32_t wait) { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Ghost(v.bots[3], 0.0f, 0, 10.0f); v.bots[3].reclaim_wait_ms = wait; return v; };
    CHECK(!HasAction(policy.Decide(build(5000), 1000), 13, ActionType::kReclaimCorpse)); // still waiting
    CHECK(HasAction(policy.Decide(build(0), 2000), 13, ActionType::kReclaimCorpse));
    CHECK(!HasAction(policy.Decide(build(0), 3000), 13, ActionType::kReclaimCorpse));    // retried after 3 s, not before
    CHECK(HasAction(policy.Decide(build(0), 5500), 13, ActionType::kReclaimCorpse));
}

void GhostCorpseRunTimesOut()
{
    std::vector<std::string> events;
    PolicyOptions options = DeadOptions(&events);
    options.CorpseRunTeleportMs = 0;                           // the 60 s teleport off: the four-minute timeout alone (ALongCorpseRunEndsInATeleportToTheCorpse has the rule)
    BotPolicy policy(options);
    auto const build = [] { WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }); Ghost(v.bots[3], 5000.0f, 0, 0.0f); return v; };
    policy.Decide(build(), 1000);
    policy.Decide(build(), 250000);                          // the first timeout puts the ghost at its corpse and starts the run over
    CHECK(Logged(events, "corpse_run_teleport:") && !Logged(events, "corpse_run_failed:"));
    policy.Decide(build(), 500000);                          // the second one is a failure
    CHECK(Logged(events, "corpse_run_failed:"));
}

void DeadBotAcceptsAResurrectOffer()
{
    BotPolicy policy(DeadOptions(nullptr));
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Corpse(v.bots[3], 10.0f);
    v.bots[3].resurrect_from = PlayerOf(10);
    CHECK(HasAction(policy.Decide(v, 1000), 13, ActionType::kResurrect, PlayerOf(10)));
    WorldTickSnapshotView none = PartyView({ 10, 11, 12, 13, 14 });
    Corpse(none.bots[3], 10.0f);
    BotPolicy other(DeadOptions(nullptr));
    CHECK(!HasAction(other.Decide(none, 1000), 13, ActionType::kResurrect));
}

void PriestResurrectsTheDeadMember()
{
    BotPolicy policy(DeadOptions(nullptr));
    auto const actions = policy.Decide(CorpseNextToPriest(), 1000);
    CHECK(CastsSpell(actions, 10, 2006, PlayerOf(13)));
    CHECK(!CastsSpell(actions, 11, 2006)); // only the closest rezzer casts
    CHECK(!CastsSpell(actions, 14, 2006));
}

void NoResurrectInCombat()
{
    BotPolicy policy(DeadOptions(nullptr));
    WorldTickSnapshotView v = CorpseNextToPriest();
    v.bots[2].in_combat = 1;
    CHECK(!CastsSpell(policy.Decide(v, 1000), 10, 2006));
}

void NoResurrectBelowTheManaReserve()
{
    BotPolicy policy(DeadOptions(nullptr));
    WorldTickSnapshotView v = CorpseNextToPriest();
    for (std::size_t i : { 0, 1, 4 })
        v.bots[i].power = 300;
    CHECK(!CastsSpell(policy.Decide(v, 1000), 10, 2006));
}

void ResurrectRetriesAfterARefusal()
{
    BotPolicy policy(DeadOptions(nullptr));
    CHECK(CastsSpell(policy.Decide(CorpseNextToPriest(), 1000), 10, 2006));
    WorldTickSnapshotView refused = CorpseNextToPriest();
    refused.bots[0].last_action_type = std::uint8_t(ActionType::kCastSpell);
    refused.bots[0].last_action_spell = 2006;
    refused.bots[0].last_action_status = forever_module::BotActionStatus::kRefused;
    refused.bots[0].last_action_sequence = 1;
    CHECK(!CastsSpell(policy.Decide(refused, 2000), 10, 2006));
    // 5 s after the refusal the next healer in line (or the same) tries again.
    auto const later = policy.Decide(CorpseNextToPriest(), 8000);
    CHECK(CastsSpell(later, 10, 2006) || CastsSpell(later, 11, 2006));
}

void OnlyTheRightClassesAtTheRightLevelsResurrect()
{
    auto const spellOf = [](std::uint32_t classId, std::uint32_t level)
    {
        BotPolicy policy(DeadOptions(nullptr, [classId](std::uint64_t guid) { return guid == 12 ? 1u : classId; }));
        WorldTickSnapshotView v = CorpseNextToPriest();
        for (BotSnapshot& bot : v.bots)
            bot.level = level;
        auto const actions = policy.Decide(v, 1000);
        for (std::uint32_t spell : { 2006u, 7328u, 2008u, 20484u })
            if (CastsSpell(actions, 10, spell))
                return spell;
        return 0u;
    };
    CHECK(spellOf(5, 10) == 2006 && spellOf(5, 9) == 0);
    CHECK(spellOf(2, 12) == 7328 && spellOf(2, 11) == 0);
    CHECK(spellOf(7, 12) == 2008 && spellOf(7, 11) == 0);
    CHECK(spellOf(11, 60) == 0); // druid
}

void HealBeatsResurrect()
{
    BotPolicy policy(DeadOptions(nullptr));
    WorldTickSnapshotView v = CorpseNextToPriest();
    v.bots[4].health = 300; // a hurt member
    auto const actions = policy.Decide(v, 1000);
    CHECK(CastsSpell(actions, 10, 2050) && !CastsSpell(actions, 10, 2006));
}

void PartyWaitIsLoggedForARezAndForAGhost()
{
    std::vector<std::string> events;
    BotPolicy policy(DeadOptions(&events));
    policy.Decide(CorpseNextToPriest(), 1000);
    CHECK(Logged(events, "party_wait:"));
    events.clear();
    BotPolicy second(DeadOptions(&events));
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
    Ghost(v.bots[3], 0.0f, 0, 100.0f);
    second.Decide(v, 1000);
    CHECK(Logged(events, "party_wait:"));
}

// A target that cannot be hurt (out of reach): the fight is given up after the watchdog time, and the target is left alone for minutes, not
// seconds, more each time (the soak's mage went back to the same two creatures for an hour and a half).
void StalledTargetIsLeftAloneForMinutes()
{
    NearbyUnit const mob = Hostile(7, 3.0f, 100);
    BotPolicy policy(AggressiveOptions());
    auto const view = [&mob]
    {
        WorldTickSnapshotView v = PartyView({ 10 });
        AddNearby(v, 0, mob);
        v.bots[0].threat_target_guid = mob.guid;
        v.bots[0].in_combat = 1;
        return v;
    };
    std::uint64_t now = 0;
    bool given = false;
    for (; now < 70000 && !given; now += 5000)
        for (BotAction const& action : policy.Decide(view(), now))
            given = given || (action.type == ActionType::kStop && action.bot_guid == 10);
    CHECK(given);                                    // given up well before a minute
    CHECK(now <= 60000);
    // Still avoided four minutes later, free again after five.
    CHECK(!HasAction(policy.Decide(view(), now + 240000), 10, ActionType::kAttackTarget, mob.guid));
    CHECK(HasAction(policy.Decide(view(), now + 310000), 10, ActionType::kAttackTarget, mob.guid));
    // Stalls again: this time it is left alone for ten minutes.
    std::uint64_t later = now + 310000;
    bool second = false;
    for (std::uint64_t t = later; t < later + 60000 && !second; t += 5000)
    {
        for (BotAction const& action : policy.Decide(view(), t))
            second = second || (action.type == ActionType::kStop && action.bot_guid == 10);
        later = t;
    }
    CHECK(second);
    CHECK(!HasAction(policy.Decide(view(), later + 540000), 10, ActionType::kAttackTarget, mob.guid));
    CHECK(HasAction(policy.Decide(view(), later + 610000), 10, ActionType::kAttackTarget, mob.guid));
}

// A late snapshot must not restart the stall timer. Steps of 15 s used to reset it (the gap was over 10 s), so a fight whose health never moved was never given up.
void SparseSnapshotsStillEndAStalledFight()
{
    NearbyUnit const mob = Hostile(7, 3.0f, 100);
    BotPolicy policy(AggressiveOptions());
    auto const view = [&mob]
    {
        WorldTickSnapshotView v = PartyView({ 10 });
        AddNearby(v, 0, mob);
        v.bots[0].threat_target_guid = mob.guid;
        v.bots[0].in_combat = 1;
        return v;
    };
    policy.Decide(view(), 0);
    policy.Decide(view(), 15000);
    policy.Decide(view(), 30000);
    bool given = false;
    for (BotAction const& action : policy.Decide(view(), 60000))
        given = given || (action.type == ActionType::kStop && action.bot_guid == 10);
    CHECK(given);
    CHECK(!HasAction(policy.Decide(view(), 70000), 10, ActionType::kAttackTarget, mob.guid));
}

// A threat guid the snapshot keeps set, but that is not in the nearby list, used to hold the fight open for hours. It is given up like a stall.
void UnseenThreatIsGivenUp()
{
    std::uint64_t const threat = MakeUnitRef(8, 1, 9);
    BotPolicy policy(AggressiveOptions());
    auto const view = [&threat]
    {
        WorldTickSnapshotView v = PartyView({ 10 });
        v.bots[0].threat_target_guid = threat;
        v.bots[0].in_combat = 1;
        v.bots[0].target_distance = 0;
        return v;
    };
    CHECK(HasAction(policy.Decide(view(), 0), 10, ActionType::kAttackTarget, threat));
    auto const given = policy.Decide(view(), 46000);
    CHECK(!HasAction(given, 10, ActionType::kAttackTarget, threat));
    CHECK(!HasAction(given, 10, ActionType::kFollow, threat));
    CHECK(!HasAction(policy.Decide(view(), 50000), 10, ActionType::kAttackTarget, threat));
}

// A hostile almost on top of the bot in the plane but 20 yards above it (a patrol on a wall walk: the Shadowfang Keep courtyard's Tormented Officers) is
// not pulled: the bot would stand under it, its follow order finds no way and its spells have no line of sight. A hostile a short climb above, a gentle slope
// away, or one that is already attacking the bot, is still fought.
void HostileOnAnotherLevelIsNotPulled()
{
    auto const act = [](NearbyUnit const& unit, bool attacked)
    {
        WorldTickSnapshotView view = PartyView({ 10 });
        view.bots[0].in_combat = attacked ? 1 : 0;
        AddNearby(view, 0, unit);
        BotPolicy policy(AggressiveOptions());
        auto const actions = policy.Decide(view, 0);
        return HasAction(actions, 10, ActionType::kAttackTarget, unit.guid) || HasAction(actions, 10, ActionType::kFollow, unit.guid);
    };
    NearbyUnit above = Hostile(7, 3.0f, 100);
    above.pos_z = 20.0f;               // 3 yards along the ground, 20 up
    CHECK(!act(above, false));
    CHECK(act(above, true));           // it is attacking the bot: the stall watchdog and the approach timeout deal with it, not this rule

    NearbyUnit farAbove = Hostile(8, 30.0f, 100);
    farAbove.pos_z = 20.0f;            // 20 up over 30 along: steeper than 0.5
    CHECK(!act(farAbove, false));

    NearbyUnit stairs = Hostile(9, 30.0f, 100);
    stairs.pos_z = 12.0f;              // 12 up over 30 along: a stair or a ramp
    CHECK(act(stairs, false));

    NearbyUnit step = Hostile(10, 3.0f, 100);
    step.pos_z = 8.0f;                 // a short climb
    CHECK(act(step, false));

    NearbyUnit below = Hostile(11, 4.0f, 100);
    below.pos_z = -18.0f;              // the floor under the bot counts the same
    CHECK(!act(below, false));
}

// A creature far enough below the bot gives no experience: it is not pulled, and its presence does not keep the bot from walking on.
void GreyCreatureIsNotPulled()
{
    NearbyUnit grey = Hostile(7, 3.0f, 100);
    grey.level = 3; // seven below a level-10 bot: grey (zero experience from 7 below at level 10)
    NearbyUnit green = Hostile(8, 6.0f, 100);
    green.level = 4; // six below: still gives a little
    WorldTickSnapshotView view = PartyView({ 10 });
    view.bots[0].level = 10;
    AddNearby(view, 0, grey);
    BotPolicy policy(AggressiveOptions());
    auto actions = policy.Decide(view, 0);
    CHECK(!HasAction(actions, 10, ActionType::kAttackTarget, grey.guid) && !HasAction(actions, 10, ActionType::kFollow, grey.guid));

    WorldTickSnapshotView withGreen = PartyView({ 10 });
    withGreen.bots[0].level = 10;
    AddNearby(withGreen, 0, grey);
    AddNearby(withGreen, 0, green);
    BotPolicy other(AggressiveOptions());
    actions = other.Decide(withGreen, 0);
    CHECK(HasAction(actions, 10, ActionType::kAttackTarget, green.guid) || HasAction(actions, 10, ActionType::kFollow, green.guid));

    // The grey creature attacks the bot: it is fought whatever its level.
    WorldTickSnapshotView attacked = PartyView({ 10 });
    attacked.bots[0].level = 10;
    attacked.bots[0].in_combat = 1;
    AddNearby(attacked, 0, grey);
    BotPolicy defender(AggressiveOptions());
    actions = defender.Decide(attacked, 0);
    CHECK(HasAction(actions, 10, ActionType::kAttackTarget, grey.guid) || HasAction(actions, 10, ActionType::kFollow, grey.guid));
}

void GreyCreatureDoesNotKeepTheBotFromExploring()
{
    NearbyUnit grey = Hostile(7, 30.0f, 100);
    grey.level = 1;
    WorldTickSnapshotView view = PartyView({ 10 });
    view.bots[0].level = 20;
    AddNearby(view, 0, grey);
    BotPolicy policy; // no hubs, no grind spots: the bot explores on foot after a quiet spell
    policy.Decide(view, 0);
    bool walks = false;
    for (BotAction const& action : policy.Decide(view, 11000))
        walks = walks || (action.type == ActionType::kMoveTo && action.bot_guid == 10);
    CHECK(walks);
}

void IdleCasterDrinksWhenLowOnMana()
{
    BotPolicy policy(AggressiveOptions());
    std::uint32_t spell = 99;
    CHECK(RestAt(policy.Decide(ManaView(20), 1000), spell) >= 0 && spell == 1); // sits
    CHECK(RestAt(policy.Decide(ManaView(60), 5000), spell) < 0);               // keeps sitting: no new action
    // No walk while it drinks, even after a long quiet spell.
    for (BotAction const& action : policy.Decide(ManaView(70), 40000))
        CHECK(action.type != ActionType::kMoveTo);
    CHECK(RestAt(policy.Decide(ManaView(95), 45000), spell) >= 0 && spell == 0); // enough: stands
}

void ManaRestIsCappedAndNotRepeatedAtOnce()
{
    BotPolicy policy(AggressiveOptions());
    std::uint32_t spell = 99;
    CHECK(RestAt(policy.Decide(ManaView(10), 1000), spell) >= 0 && spell == 1);
    CHECK(RestAt(policy.Decide(ManaView(10), 100000), spell) < 0);              // still drinking
    CHECK(RestAt(policy.Decide(ManaView(10), 125000), spell) >= 0 && spell == 0); // 2 minutes: stands, the mana is not coming back
    CHECK(RestAt(policy.Decide(ManaView(10), 130000), spell) < 0);              // and does not sit again at once
}

void FighterWithoutManaDoesNotDrink()
{
    BotPolicy policy(AggressiveOptions());
    WorldTickSnapshotView view = ManaView(0);
    view.bots[0].power_type = 1; // rage
    std::uint32_t spell = 99;
    CHECK(RestAt(policy.Decide(view, 1000), spell) < 0);
}

void AutoShotIsNotReissuedWhileActive()
{
    // Level 1: only Auto Shot is learned. It is cast once; once accepted it is not cast again at the next global cooldown.
    BotPolicy policy = ClassPolicy(3);
    NearbyUnit const mob = Hostile(2, 20.0f, 100);
    CHECK(HasCast(policy.Decide(HunterView(1, 1, { mob }), 1000), 75));

    WorldTickSnapshotView accepted = HunterView(2, 1, { mob }, MakeUnitRef(8, 1, 2));
    Result(accepted, 75, BotActionStatus::kApplied, 1);
    CHECK(!HasCast(policy.Decide(accepted, 2600), 75));
    for (std::uint64_t i = 0; i < 5; ++i)
        CHECK(!HasCast(policy.Decide(HunterView(3 + i, 1, { mob }, MakeUnitRef(8, 1, 2)), 4200 + 1600 * i), 75));

    // A new target: Auto Shot is cast on it.
    NearbyUnit const other = Hostile(3, 20.0f, 100);
    CHECK(HasCast(policy.Decide(HunterView(10, 1, { other }, MakeUnitRef(8, 1, 3)), 20000), 75));
}

void AutoShotIsReissuedAfterTheFightAndWhenRefused()
{
    BotPolicy policy = ClassPolicy(3);
    NearbyUnit const mob = Hostile(2, 20.0f, 100);
    policy.Decide(HunterView(1, 1, { mob }), 1000);
    WorldTickSnapshotView accepted = HunterView(2, 1, { mob }, MakeUnitRef(8, 1, 2));
    Result(accepted, 75, BotActionStatus::kApplied, 1);
    policy.Decide(accepted, 2600);
    // The fight ends (nothing to fight), then a new fight with the same creature reference: shot again.
    policy.Decide(HunterView(3, 1, {}), 5000);
    CHECK(HasCast(policy.Decide(HunterView(4, 1, { mob }, MakeUnitRef(8, 1, 2)), 8000), 75));

    // A refused Auto Shot is not marked active: it is retried.
    BotPolicy refused = ClassPolicy(3);
    refused.Decide(HunterView(1, 1, { mob }), 1000);
    WorldTickSnapshotView no = HunterView(2, 1, { mob }, MakeUnitRef(8, 1, 2));
    Result(no, 75, BotActionStatus::kRefused, 1);
    refused.Decide(no, 2600);
    CHECK(HasCast(refused.Decide(HunterView(3, 1, { mob }, MakeUnitRef(8, 1, 2)), 4000), 75));
}

void AutoShotIsReissuedAfterMoving()
{
    BotPolicy policy = ClassPolicy(3);
    NearbyUnit const mob = Hostile(2, 20.0f, 100);
    policy.Decide(HunterView(1, 1, { mob }), 1000);
    WorldTickSnapshotView accepted = HunterView(2, 1, { mob }, MakeUnitRef(8, 1, 2));
    Result(accepted, 75, BotActionStatus::kApplied, 1);
    policy.Decide(accepted, 2600);
    WorldTickSnapshotView moving = HunterView(3, 1, { mob }, MakeUnitRef(8, 1, 2));
    moving.bots[0].moving = 1; // moving stops Auto Shot
    policy.Decide(moving, 4200);
    CHECK(HasCast(policy.Decide(HunterView(4, 1, { mob }, MakeUnitRef(8, 1, 2)), 5800), 75));
}

void HunterCallsItsPet()
{
    BotPolicy policy = ClassPolicy(3);
    // Below level 10 there is no pet spell.
    CHECK(!HasCast(policy.Decide(HunterView(1, 9, {}), 1000), 883));
    // From level 10: Call Pet when nothing is to be fought, on the bot (the pet is the spell's own target).
    auto actions = policy.Decide(HunterView(2, 10, {}), 2000);
    CHECK(HasCast(actions, 883));
    CHECK(CastTargetOf(actions, 883) == 0);
    // Accepted: not called again.
    WorldTickSnapshotView accepted = HunterView(3, 10, {});
    Result(accepted, 883, BotActionStatus::kApplied, 1);
    CHECK(policy.Decide(accepted, 8000).empty());
    CHECK(policy.Decide(HunterView(4, 10, {}), 20000).empty());
    // Not with a creature to fight.
    CHECK(!HasCast(ClassPolicy(3).Decide(HunterView(1, 10, { Hostile(2, 20.0f, 100) }), 1000), 883));
}

void HunterRevivesADeadPetThenBacksOff()
{
    BotPolicy policy = ClassPolicy(3);
    policy.Decide(HunterView(1, 20, {}), 1000);
    WorldTickSnapshotView refused = HunterView(2, 20, {});
    Result(refused, 883, BotActionStatus::kRefused, 1); // Call Pet refused: the pet is dead
    auto actions = policy.Decide(refused, 7000);
    CHECK(HasCast(actions, 982) && !HasCast(actions, 883));

    // Revive Pet refused too: nothing for 30 s, then Call Pet again.
    WorldTickSnapshotView no = HunterView(3, 20, {});
    Result(no, 982, BotActionStatus::kRefused, 2);
    CHECK(!HasCast(policy.Decide(no, 20000), 982));
    CHECK(policy.Decide(HunterView(4, 20, {}), 30000).empty());
    CHECK(HasCast(policy.Decide(HunterView(5, 20, {}), 51000), 883));

    // A Revive Pet that is accepted ends it.
    BotPolicy ok = ClassPolicy(3);
    ok.Decide(HunterView(1, 20, {}), 1000);
    WorldTickSnapshotView dead = HunterView(2, 20, {});
    Result(dead, 883, BotActionStatus::kRefused, 1);
    ok.Decide(dead, 7000);
    WorldTickSnapshotView revived = HunterView(3, 20, {});
    Result(revived, 982, BotActionStatus::kApplied, 2);
    CHECK(ok.Decide(revived, 20000).empty());
    CHECK(ok.Decide(HunterView(4, 20, {}), 40000).empty());
}

void HunterChecksItsPetAfterAFight()
{
    BotPolicy policy = ClassPolicy(3);
    NearbyUnit const mob = Hostile(2, 20.0f, 100);
    // Pet out (Call Pet accepted), then a fight.
    policy.Decide(HunterView(1, 20, {}), 1000);
    WorldTickSnapshotView out = HunterView(2, 20, {});
    Result(out, 883, BotActionStatus::kApplied, 1);
    policy.Decide(out, 7000);
    policy.Decide(HunterView(3, 20, { mob }, MakeUnitRef(8, 1, 2)), 9000);
    // After the fight Mend Pet checks and heals it.
    auto actions = policy.Decide(HunterView(4, 20, {}), 15000);
    CHECK(HasCast(actions, 136));
    CHECK(CastTargetOf(actions, 136) == 0);
    // Accepted: the pet is there, no more casts.
    WorldTickSnapshotView mended = HunterView(5, 20, {});
    Result(mended, 136, BotActionStatus::kApplied, 2);
    CHECK(policy.Decide(mended, 22000).empty());
    CHECK(policy.Decide(HunterView(6, 20, {}), 30000).empty());

    // Mend Pet refused (no pet to heal): Call Pet follows.
    BotPolicy gone = ClassPolicy(3);
    gone.Decide(HunterView(1, 20, {}), 1000);
    gone.Decide(out, 7000);
    gone.Decide(HunterView(3, 20, { mob }, MakeUnitRef(8, 1, 2)), 9000);
    gone.Decide(HunterView(4, 20, {}), 15000);
    WorldTickSnapshotView no = HunterView(5, 20, {});
    Result(no, 136, BotActionStatus::kRefused, 2);
    CHECK(HasCast(gone.Decide(no, 22000), 883));

    // Below level 12 there is no Mend Pet: Call Pet is the check.
    BotPolicy low = ClassPolicy(3);
    low.Decide(HunterView(1, 10, {}), 1000);
    WorldTickSnapshotView lowOut = HunterView(2, 10, {});
    Result(lowOut, 883, BotActionStatus::kApplied, 1);
    low.Decide(lowOut, 7000);
    low.Decide(HunterView(3, 10, { mob }, MakeUnitRef(8, 1, 2)), 9000);
    CHECK(HasCast(low.Decide(HunterView(4, 10, {}), 15000), 883));
}

void HunterMendsItsPetInALongFight()
{
    BotPolicy policy = ClassPolicy(3);
    NearbyUnit const mob = Hostile(2, 20.0f, 100);
    policy.Decide(HunterView(1, 20, {}), 1000);
    WorldTickSnapshotView out = HunterView(2, 20, {});
    Result(out, 883, BotActionStatus::kApplied, 1);
    policy.Decide(out, 7000);
    // The fight starts at 10 s: no Mend Pet early, one after 15 s, not again within 30 s.
    policy.Decide(HunterView(3, 20, { mob }, MakeUnitRef(8, 1, 2)), 10000);
    CHECK(!HasCast(policy.Decide(HunterView(4, 20, { mob }, MakeUnitRef(8, 1, 2)), 14000), 136));
    CHECK(HasCast(policy.Decide(HunterView(5, 20, { mob }, MakeUnitRef(8, 1, 2)), 26000), 136));
    CHECK(!HasCast(policy.Decide(HunterView(6, 20, { mob }, MakeUnitRef(8, 1, 2)), 30000), 136));
    // Not without mana.
    BotPolicy dry = ClassPolicy(3);
    dry.Decide(HunterView(1, 20, {}), 1000);
    dry.Decide(out, 7000);
    dry.Decide(HunterView(3, 20, { mob }, MakeUnitRef(8, 1, 2)), 10000);
    WorldTickSnapshotView low = HunterView(5, 20, { mob }, MakeUnitRef(8, 1, 2));
    low.bots[0].power = 10;
    low.bots[0].max_power = 100;
    CHECK(!HasCast(dry.Decide(low, 26000), 136));
}

// A ranged bot with a melee target inside kiting distance backs off instead of attacking.
void RangedBotKitesACloseTarget()
{
    BotPolicy policy = ClassPolicy(8);
    WorldTickSnapshotView view = View(1, MakeUnitRef(8, 1, 2), { Hostile(2, 5.0f, 100) });
    view.bots[0].in_combat = 1;
    auto actions = policy.Decide(view, 1000);
    bool kited = false;
    for (BotAction const& action : actions)
        kited = kited || (action.type == ActionType::kMoveTo && action.target_guid == 0);
    CHECK(kited);
    for (BotAction const& action : actions)
        CHECK(action.type != ActionType::kAttackTarget);
}

// In a party the caster stands and casts: the tank holds the target, backing off would keep it moving and out of spells.
void PartyCasterDoesNotKite()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 2;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : 8u; };   // 10 the warrior leads, 11 is a mage
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11 });
    NearbyUnit mob = Hostile(2, 5.0f, 100);
    view.bots[1].threat_target_guid = MakeUnitRef(8, 1, 2);
    view.bots[1].in_combat = 1;
    view.bots[0].in_combat = 1;
    view.bots[0].threat_target_guid = MakeUnitRef(8, 1, 2);
    AddNearby(view, 0, mob);
    AddNearby(view, 1, mob);
    int kites = 0, casts = 0;
    for (std::uint64_t at = 1000; at < 6000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
        {
            kites += action.bot_guid == 11 && action.type == ActionType::kMoveTo && action.target_guid == 0;
            casts += action.bot_guid == 11 && action.type == ActionType::kCastSpell;
        }
    }
    CHECK(kites == 0);
    CHECK(casts > 0);
}

// Onyxia (10184) bands by role: a healer nearer than 21 yards backs off to the middle of its 21-25 band (23).
void HealerKitesToOnyxiaBand()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : guid == 11 ? 5u : 4u; };   // 10 the warrior tank, 11 a priest, 12 a rogue
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 }, options.ClassOf);
    NearbyUnit boss = Hostile(2, 20.0f, 100);
    boss.guid = MakeUnitRef(8, 10184, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].threat_target_guid = boss.guid;
        view.bots[i].in_combat = 1;
        AddNearby(view, i, boss);
    }
    float healerAt = -1.0f;
    for (std::uint64_t at = 1000; at < 3000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11 && action.type == ActionType::kMoveTo && action.target_guid == 0)
                healerAt = std::fabs(action.dest_x - boss.pos_x);
    }
    CHECK(healerAt >= 22.0f && healerAt <= 24.0f);
}

} // namespace

// Ghost teleport hold: a dungeon ghost is not put back beside a live hostile at its destination (the leader), for GhostTeleportHoldMaxMs at most; a bot that died
// DeathLockoutCount times in the window waits DeathLockoutHoldMs more; GhostTeleportHold = 0 is the old behaviour.
void AGhostTeleportWaitsForAHostileAndALockout()
{
    auto const build = [](bool hostile, bool ghost)
    {
        WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
        for (BotSnapshot& bot : v.bots)
            bot.map_id = 36;
        if (ghost)
        {
            Ghost(v.bots[3], 0.0f, 36, 50.0f);
            v.bots[3].map_id = 0;                                              // the ghost is outside, at the graveyard
        }
        if (hostile)
            AddNearby(v, 2, HostileAt(7, 10.0f, 0.0f));                        // 10 yards from the leader, the destination
        return v;
    };
    auto const policyOf = [](std::vector<std::string>* events, bool hold)
    {
        PolicyOptions options = DeadOptions(events, AllWarriors);
        options.GhostTeleportHold = hold;
        return options;
    };
    // a hostile at the destination: held, one line in 10 s; teleports once it is gone
    std::vector<std::string> events;
    BotPolicy policy(policyOf(&events, true));
    policy.SetRouteActive(true);
    CHECK(!TeleportOf(policy.Decide(build(true, true), 1000), 13).has_value());
    CHECK(Logged(events, "ghost_teleport_hold: bot=13 reason=hostile"));
    CHECK(!TeleportOf(policy.Decide(build(true, true), 5000), 13).has_value());
    std::size_t holds = 0;                                                     // one line in the first 10 s
    for (std::string const& event : events)
        holds += event.rfind("ghost_teleport_hold:", 0) == 0 ? 1 : 0;
    CHECK(holds == 1);
    CHECK(!TeleportOf(policy.Decide(build(true, true), 100000), 13).has_value());   // still held 99 s in
    CHECK(TeleportOf(policy.Decide(build(false, true), 101000), 13).has_value());   // the hostile is gone
    // the hold ends at the maximum: 120 s after the first decision the ghost goes anyway
    BotPolicy expiring(policyOf(nullptr, true));
    expiring.SetRouteActive(true);
    CHECK(!TeleportOf(expiring.Decide(build(true, true), 1000), 13).has_value());
    CHECK(!TeleportOf(expiring.Decide(build(true, true), 120000), 13).has_value());
    CHECK(TeleportOf(expiring.Decide(build(true, true), 121500), 13).has_value());
    // no hostile and few deaths: no hold, no line
    std::vector<std::string> quiet;
    BotPolicy calm(policyOf(&quiet, true));
    calm.SetRouteActive(true);
    CHECK(TeleportOf(calm.Decide(build(false, true), 1000), 13).has_value());
    CHECK(!Logged(quiet, "ghost_teleport_hold:"));
    // off: the old behaviour, a hostile does not hold
    BotPolicy off(policyOf(nullptr, false));
    off.SetRouteActive(true);
    CHECK(TeleportOf(off.Decide(build(true, true), 1000), 13).has_value());
    // lockout: the 4th death in 10 minutes waits 90 s with no hostile near; the first three do not
    std::vector<std::string> lockEvents;
    BotPolicy lock(policyOf(&lockEvents, true));
    lock.SetRouteActive(true);
    for (std::uint64_t death = 0; death < 3; ++death)
    {
        CHECK(TeleportOf(lock.Decide(build(false, true), 1000 + death * 20000), 13).has_value());
        lock.Decide(build(false, false), 10000 + death * 20000);               // revived
    }
    CHECK(!Logged(lockEvents, "ghost_teleport_hold:"));
    CHECK(!TeleportOf(lock.Decide(build(false, true), 61000), 13).has_value());
    CHECK(Logged(lockEvents, "ghost_teleport_hold: bot=13 reason=lockout"));
    CHECK(!TeleportOf(lock.Decide(build(false, true), 150000), 13).has_value());
    CHECK(TeleportOf(lock.Decide(build(false, true), 152000), 13).has_value());
    // DeathLockoutCount = 0: no lockout
    PolicyOptions noLock = policyOf(nullptr, true);
    noLock.DeathLockoutCount = 0;
    BotPolicy none(noLock);
    none.SetRouteActive(true);
    for (std::uint64_t death = 0; death < 4; ++death)
    {
        CHECK(TeleportOf(none.Decide(build(false, true), 1000 + death * 20000), 13).has_value());
        none.Decide(build(false, false), 10000 + death * 20000);
    }
}

void RunPolicyTestsPart5()
{
    AutoShotIsNotReissuedWhileActive();
    AutoShotIsReissuedAfterTheFightAndWhenRefused();
    AutoShotIsReissuedAfterMoving();
    HunterCallsItsPet();
    HunterRevivesADeadPetThenBacksOff();
    HunterChecksItsPetAfterAFight();
    HunterMendsItsPetInALongFight();
    HostileOnAnotherLevelIsNotPulled();
    ALongCorpseRunEndsInATeleportToTheCorpse();
    ARefusedGhostTeleportIsTriedAgainLater();
    AWipedPartysGhostsArePutAtTheirCorpsesAfterAMinute();
    DeathLoopMovesTheBotToAGrindSpot();
    DeathLoopMovesTheBotToAGrindSpot(62000);   // prod: one death a minute, three deaths span two minutes
    ASecondDeathToTheSameCreatureMovesTheBot(1998);
    ASecondDeathToTheSameCreatureMovesTheBot(3099);
    AHurtSoloBotRestsBeforeItPulls();
    CorpseReleasesAtOnceWithoutARezzer();
    CorpseReleasesWhenTheWholePartyIsDown();
    ReleaseIsHeldInCombatUpToTheCap();
    GhostWalksToItsCorpseAndIgnoresCombat();
    GhostOnTheOutsideMapWalksToThePortal();
    ADeadMemberOnMapZeroIsTeleportedBackToItsCorpse();
    ADeadMemberIsRevivedByItsLeaderInsideTheDungeon();
    AGhostOnTheOutsideMapIsNotRevivedBeforeItEnters();
    AnAliveMemberOutsideWalksBackToTheDungeon();
    AGhostDoesNotReclaimNextToAHostile();
    GhostReclaimsInRadiusAndRetries();
    SpiritHealerAfterAHostileStaysNearTheCorpse();
    SpiritHealerIsNotTakenWithoutAHostile();
    SpiritHealerOnARepeatedKiller();
    ARefusedSpiritHealerFallsBackToTheReclaim();
    GhostCorpseRunTimesOut();
    DeadBotAcceptsAResurrectOffer();
    PriestResurrectsTheDeadMember();
    NoResurrectInCombat();
    NoResurrectBelowTheManaReserve();
    ResurrectRetriesAfterARefusal();
    OnlyTheRightClassesAtTheRightLevelsResurrect();
    HealBeatsResurrect();
    PartyWaitIsLoggedForARezAndForAGhost();
    StalledTargetIsLeftAloneForMinutes();
    SparseSnapshotsStillEndAStalledFight();
    UnseenThreatIsGivenUp();
    GreyCreatureIsNotPulled();
    GreyCreatureDoesNotKeepTheBotFromExploring();
    IdleCasterDrinksWhenLowOnMana();
    ManaRestIsCappedAndNotRepeatedAtOnce();
    FighterWithoutManaDoesNotDrink();
    RangedBotKitesACloseTarget();
    PartyCasterDoesNotKite();
    HealerKitesToOnyxiaBand();
    ADeadLeadersGhostIsNotHeldByAWarderNearAMember();
    ACorpseDoesNotReleaseUnderARunningResurrect();
    AGhostTeleportWaitsForAHostileAndALockout();
}
