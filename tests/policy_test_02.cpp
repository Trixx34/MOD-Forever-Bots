// SPDX-License-Identifier: GPL-2.0-or-later
// Policy tests, part 2 of 8 (split out of policy_test.cpp; the helpers are in policy_test_support.h).
#include "policy_test_support.h"

namespace {

void UnknownSideBotsAreLeftOutOfParties()
{
    // Bot 14's race is not known (side 0) while the module can tell the sides of the others: it is not put into a party, so it is never invited
    // across the sides. The other four party up as before.
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    options.PartySideOf = [](std::uint64_t guid) { return std::uint8_t(guid == 14 ? 0 : 1); };
    BotPolicy policy(options);
    auto const actions = policy.Decide(PartyView({ 10, 11, 12, 13, 14 }), 0);
    CHECK(HasGroup(actions, 12, 1, PlayerOf(10)) && HasGroup(actions, 12, 1, PlayerOf(11)) && HasGroup(actions, 12, 1, PlayerOf(13)));
    CHECK(!HasGroup(actions, 12, 1, PlayerOf(14)));
    CHECK(!HasAnyGroup(actions, 14));
}

void ABotInAHumansPartyIsLeftAlone()
{
    // Bot 10 is invited by a human player (not a bot in the view): the bot party assignment leaves it out, and nobody invites or declines for it.
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[0].group_invite_from = PlayerOf(900);
    auto const actions = PartyPolicy(5).Decide(view, 0);
    CHECK(!HasGroup(actions, 12, 1, PlayerOf(10)));
    CHECK(!HasGroup(actions, 10, 3));
}

void OnlyTheFirstPartyIsFormed()
{
    // Ten bots, two parties of five: the first party's leader (bot 12) invites its members; bots 15..19 are not
    // partied, so nobody invites them.
    std::vector<std::uint64_t> guids = { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 };
    auto const actions = PartyPolicy(5, 1).Decide(PartyView(guids), 0);
    CHECK(HasGroup(actions, 12, 1, PlayerOf(10)));
    for (std::uint64_t unpartied : { 15, 16, 17, 18, 19 })
        CHECK(!HasAnyGroup(actions, unpartied));
}

void InvitesWaitForTheRetry()
{
    BotPolicy policy = PartyPolicy(5);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    CHECK(HasGroup(policy.Decide(view, 0), 12, 1));
    CHECK(!HasGroup(policy.Decide(view, 1000), 12, 1)); // still waiting for the answer
    CHECK(HasGroup(policy.Decide(view, 16000), 12, 1)); // invited again after the retry
}

void InvitedMemberIsNotInvitedAgain()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[0].group_invite_from = PlayerOf(12); // bot 10 has the invite waiting
    CHECK(!HasGroup(PartyPolicy(5).Decide(view, 0), 12, 1, PlayerOf(10)));
}

void ATankLeadsThePartyWhateverItsGuid()
{
    // Bots 10 and 13 are warriors. Bot 10 has no role, so by guid order it would lead; bot 13 has a prot spec, so its role is tank from the first tick on.
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 || guid == 13 ? 1u : 5u; };
    options.SpecOf = [](std::uint64_t guid) { return guid == 13 ? std::string("prot") : std::string(); };
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    policy.Decide(view, 0); // the first tick sets the tank role from the spec
    auto const actions = policy.Decide(view, 1000);
    CHECK(HasGroup(actions, 13, 1));  // the tank leads and invites
    CHECK(!HasGroup(actions, 10, 1)); // the warrior with no role follows
}

void MemberAcceptsItsLeadersInvite()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[1].group_invite_from = PlayerOf(12);
    auto const actions = PartyPolicy(5).Decide(view, 0);
    CHECK(HasGroup(actions, 11, 2));
    CHECK(!HasAnyGroup(actions, 10));
}

void MemberLeavesAForeignParty()
{
    // Bot 20 is another bot of the module (online): bot 11 leaves its party for its own leader's.
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14, 20 });
    view.bots[1].group_role = 1;
    view.bots[1].group_leader_guid = PlayerOf(20);
    CHECK(HasGroup(PartyPolicy(5).Decide(view, 0), 11, 3));

    // A party led by a human player (99 is not a bot of the module) is left alone: the bot stays and is not invited.
    view.bots[1].group_leader_guid = PlayerOf(99);
    auto const human = PartyPolicy(5).Decide(view, 0);
    CHECK(!HasAnyGroup(human, 11) && !HasGroup(human, 12, 1, PlayerOf(11)));

    // In its own party already: nothing to do.
    view.bots[1].group_leader_guid = PlayerOf(12);
    auto const actions = PartyPolicy(5).Decide(view, 0);
    CHECK(!HasGroup(actions, 11, 3));
    CHECK(!HasAnyGroup(actions, 11));
}

void MemberFollowsItsLeaderUntilClose()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[1].pos_x = 30.0f; // bot 11, far from the leader at the origin
    CHECK(HasAction(PartyPolicy(5).Decide(view, 0), 11, ActionType::kFollow, PlayerOf(12)));

    view.bots[1].pos_x = 3.0f; // close enough: it holds where it is
    CHECK(!HasAction(PartyPolicy(5).Decide(view, 0), 11, ActionType::kFollow));
}

// 0.45.3: a leader on another map (the Stockade leader's ghost released to Stormwind, 2026-11-07 22:20) is not followed: the core's follow walked the members
// towards the leader's coordinates on the other map, out of the prison and off the route.
void MemberDoesNotFollowALeaderOnAnotherMap()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.map_id = 34;
    view.bots[1].pos_x = 30.0f;                // bot 11, far from the leader
    view.bots[2].map_id = 0;                   // the leader, bot 12, on map 0
    view.bots[2].pos_x = -8700.0f;
    auto const actions = PartyPolicy(5).Decide(view, 0);
    CHECK(!HasAction(actions, 11, ActionType::kFollow));
    view.bots[2].map_id = 34;                  // back on the same map: followed again
    view.bots[2].pos_x = 0.0f;
    CHECK(HasAction(PartyPolicy(5).Decide(view, 0), 11, ActionType::kFollow, PlayerOf(12)));
}

void MemberFightsItsLeadersTarget()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit const wolf = Hostile(7, 2.0f, 100);
    view.bots[2].threat_target_guid = wolf.guid; // the leader is fighting the wolf
    AddNearby(view, 1, wolf);                    // bot 11 sees it
    auto const actions = PartyPolicy(5).Decide(view, 0);
    // Bot 11 is a priest, and the wolf is 2 yards away: a melee target inside kiting distance, so it backs off (kMoveTo) and casts at
    // the wolf instead of walking into it. Either way it acts on the leader's target; a melee member attacks it outright.
    bool engages = false;
    for (BotAction const& action : actions)
        if (action.bot_guid == 11 && action.target_guid == wolf.guid &&
            (action.type == ActionType::kAttackTarget || action.type == ActionType::kCastSpell))
            engages = true;
    CHECK(engages);
}

// Focus fire: with a kill priority a damage dealer takes the priority caster of the tank's pack, not only the tank's own target.
void ADamageDealerFightsThePriorityCasterOfThePack()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; }; // bot 11 is the mage, 12 the warrior, the rest priests
    options.KillPriority = { { 1729, 0 } };                                                    // Defias Evoker first
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit tankTarget = Hostile(7, 2.0f, 100);
    tankTarget.guid = MakeUnitRef(8, 598, 7);        // a miner, the tank's own target
    NearbyUnit caster = Hostile(8, 5.0f, 100);
    caster.guid = MakeUnitRef(8, 1729, 8);           // an evoker of the same pack
    view.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(view, 1, tankTarget);
    AddNearby(view, 1, caster);
    bool onCaster = false, onMiner = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11)
            {
                onCaster = onCaster || action.target_guid == caster.guid;
                onMiner = onMiner || action.target_guid == tankTarget.guid;
            }
    CHECK(onCaster);
    CHECK(!onMiner);
    // Without a priority list it assists the tank's target as before.
    PolicyOptions plain = options;
    plain.KillPriority.clear();
    BotPolicy plainPolicy(plain);
    plainPolicy.SetRouteActive(true);
    bool plainOnMiner = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : plainPolicy.Decide(view, at))
            plainOnMiner = plainOnMiner || (action.bot_guid == 11 && action.target_guid == tankTarget.guid);
    CHECK(plainOnMiner);
}

// Deadmines Foundry: once a Remote-Controlled Golem (2520) is up, every damage dealer takes the Goblin Engineer (622) that summoned
// it over the tank's own target, with no tactic switched on (this is not optional: the golem is immune to most abilities and only
// despawns when its engineer dies - Wowhead's Deadmines guide). Without a golem up it assists the tank's target as usual.
void ADamageDealerFocusesTheEngineerWhenAGolemIsUp()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; }; // bot 11 is the mage, 12 the warrior, the rest priests
    options.TacticFocus = false;
    options.TacticInterrupt = false;
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit tankTarget = Hostile(7, 2.0f, 100);
    tankTarget.guid = MakeUnitRef(8, 598, 7);         // a miner, the tank's own target
    NearbyUnit golem = Hostile(8, 5.0f, 100);
    golem.guid = MakeUnitRef(8, 2520, 8);             // a Remote-Controlled Golem is already up
    NearbyUnit engineer = Hostile(9, 6.0f, 100);
    engineer.guid = MakeUnitRef(8, 622, 9);           // the Goblin Engineer that summoned it
    view.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(view, 1, tankTarget);
    AddNearby(view, 1, golem);
    AddNearby(view, 1, engineer);
    bool onEngineer = false, onMiner = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11)
            {
                onEngineer = onEngineer || action.target_guid == engineer.guid;
                onMiner = onMiner || action.target_guid == tankTarget.guid;
            }
    CHECK(onEngineer);
    CHECK(!onMiner);

    // No golem nearby: no special case, it assists the tank's target as usual.
    WorldTickSnapshotView noGolem = PartyView({ 10, 11, 12, 13, 14 });
    noGolem.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(noGolem, 1, tankTarget);
    AddNearby(noGolem, 1, engineer);
    BotPolicy noGolemPolicy(options);
    noGolemPolicy.SetRouteActive(true);
    bool onMinerNow = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : noGolemPolicy.Decide(noGolem, at))
            onMinerNow = onMinerNow || (action.bot_guid == 11 && action.target_guid == tankTarget.guid);
    CHECK(onMinerNow);
}

// Interrupt: a mage (Counterspell in its rotation) turns on a unit of the tank's pack that is casting; with the tactic off it keeps to the tank's target.
void ACasterOfThePackIsTargetedForAnInterrupt()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; };
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit tankTarget = Hostile(7, 2.0f, 100);
    tankTarget.guid = MakeUnitRef(8, 598, 7);
    NearbyUnit caster = Hostile(8, 5.0f, 100);
    caster.guid = MakeUnitRef(8, 1729, 8);
    caster.casting = 1;
    view.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(view, 1, tankTarget);
    AddNearby(view, 1, caster);
    auto const targets = [&](PolicyOptions const& o, std::uint64_t unit)
    {
        BotPolicy policy(o);
        policy.SetRouteActive(true);
        for (std::uint64_t at = 0; at < 6000; at += 500)
            for (BotAction const& action : policy.Decide(view, at))
                if (action.bot_guid == 11 && action.target_guid == unit)
                    return true;
        return false;
    };
    CHECK(targets(options, caster.guid));
    PolicyOptions off = options;
    off.TacticInterrupt = false;
    CHECK(!targets(off, caster.guid));
    CHECK(targets(off, tankTarget.guid));
}

// The 2026-10-27 Ragefire Chasm soak: kMaxDungeonEngaged caps the pack-focus widening once the party is already fighting two distinct
// hostiles, so it does not reach for a third, *unlisted* one nobody has engaged yet. It still reaches for any of the dungeon file's own
// kill_priority entries whatever the count already engaged (the Searing Blade Warlock must still be reachable even with two lower-priority
// trash already in the fight: a follow-up investigation found its "summon" is a one-time, out-of-combat cast, not a mid-fight resummon --
// see TheVoidwalkerCompanionStaysReachablePastTheEngagementCapToo below for the actual fourth pack member this cap must also reach).
void ADamageDealerDoesNotWidenPastTwoAlreadyEngagedHostiles()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; }; // bot 11 is the mage, 12 the warrior, the rest priests
    options.KillPriority = { { 11324, 0 } }; // the Searing Blade Warlock only: the Cultist and an unlisted trash below are not in the list
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });

    // The tank (12) is already fighting the Cultist, and a priest (13) is already fighting an unlisted trash (11999): two distinct hostiles engaged.
    NearbyUnit cultist = Hostile(7, 2.0f, 100);
    cultist.guid = MakeUnitRef(8, 11322, 7);
    view.bots[2].in_combat = 1;
    view.bots[2].threat_target_guid = cultist.guid;
    NearbyUnit trash = Hostile(8, 6.0f, 100);
    trash.guid = MakeUnitRef(8, 11999, 8);
    view.bots[3].in_combat = 1;
    view.bots[3].threat_target_guid = trash.guid;
    // A second unlisted trash stands within kFocusPackYards of the tank's target, but nobody has engaged it yet: not reachable past the cap.
    NearbyUnit unlistedAdd = Hostile(9, 10.0f, 100);
    unlistedAdd.guid = MakeUnitRef(8, 12000, 9);
    // The Warlock (kill_priority rank 0) is in the same spot, also unengaged: it must stay reachable regardless of the cap.
    NearbyUnit warlock = Hostile(10, 10.0f, 100);
    warlock.guid = MakeUnitRef(8, 11324, 10);
    AddNearby(view, 1, cultist);
    AddNearby(view, 1, trash);
    AddNearby(view, 1, unlistedAdd);
    AddNearby(view, 1, warlock);

    bool onUnlistedAdd = false, onWarlock = false, onTrash = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11)
            {
                onUnlistedAdd = onUnlistedAdd || action.target_guid == unlistedAdd.guid;
                onWarlock = onWarlock || action.target_guid == warlock.guid;
                onTrash = onTrash || action.target_guid == trash.guid;
            }
    CHECK(!onUnlistedAdd); // an unlisted hostile nobody has engaged yet is not pulled in once two others already are
    CHECK(onWarlock);      // the dungeon's own kill_priority still reaches past the cap, whatever is already engaged
    CHECK(!onTrash);       // (the mage prefers the Warlock over re-targeting the already engaged, unlisted trash: rank 0 beats rank 1000)
}

// 2026-10-27 follow-up to the test above: the Searing Blade Warlock's "Summon Voidwalker" turned out to be SmartAI event UPDATE_OOC with no
// repeat timer (this fork's own smart_scripts: entryorguid 11324, a one-time out-of-combat cast, spell 12746) -- it fires once before the party
// ever reaches the pack, not as a resummon mid-fight, and entry 8996 (Voidwalker Minion) has no spawn of its own anywhere in the world
// database. The Voidwalker is really a fourth, passive pack member that already exists when the party arrives and simply joins the melee late
// (the 2026-10-26 soak's combat log: the tank fought the Cultist and the Enforcer for 30+ seconds before the Voidwalker engaged too, and died 3
// seconds later). 389-ragefire-chasm.json now lists 8996 in kill_priority for exactly the reason the test above exists: without it, a late-
// joining, unengaged Voidwalker is just as "unlisted" as any other trash once two pack members are already fought, and kMaxDungeonEngaged would
// keep the party from ever turning on it.
void TheVoidwalkerCompanionStaysReachablePastTheEngagementCapToo()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; }; // bot 11 is the mage, 12 the warrior, the rest priests
    options.KillPriority = { { 11324, 0 }, { 8996, 1 }, { 11322, 2 }, { 11323, 3 } }; // the shipped dungeon file's order
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });

    // The party is already fighting the Cultist and the Enforcer (two distinct hostiles engaged, the cap), the same as a tank-and-two-adds
    // fight already 30+ seconds in.
    NearbyUnit cultist = Hostile(7, 2.0f, 100);
    cultist.guid = MakeUnitRef(8, 11322, 7);
    view.bots[2].in_combat = 1;
    view.bots[2].threat_target_guid = cultist.guid;
    NearbyUnit enforcer = Hostile(8, 6.0f, 100);
    enforcer.guid = MakeUnitRef(8, 11323, 8);
    view.bots[3].in_combat = 1;
    view.bots[3].threat_target_guid = enforcer.guid;
    // The Voidwalker joins late: unengaged, standing near the pack, the same as the soak's combat log.
    NearbyUnit voidwalker = Hostile(9, 10.0f, 100);
    voidwalker.guid = MakeUnitRef(8, 8996, 9);
    // An unlisted trash in the same spot must still not be reached, same as the test above: the cap is not simply lifted outright.
    NearbyUnit unlistedAdd = Hostile(10, 10.0f, 100);
    unlistedAdd.guid = MakeUnitRef(8, 12001, 10);
    AddNearby(view, 1, cultist);
    AddNearby(view, 1, enforcer);
    AddNearby(view, 1, voidwalker);
    AddNearby(view, 1, unlistedAdd);

    bool onVoidwalker = false, onUnlistedAdd = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11)
            {
                onVoidwalker = onVoidwalker || action.target_guid == voidwalker.guid;
                onUnlistedAdd = onUnlistedAdd || action.target_guid == unlistedAdd.guid;
            }
    CHECK(onVoidwalker);    // the Voidwalker (kill_priority rank 1) is reached past the cap once it joins, same as the Warlock
    CHECK(!onUnlistedAdd);  // an unrelated unlisted hostile in the same spot is still not pulled in: only the dungeon's own list is exempt
}

// Ragefire Chasm, the Searing Blade camp (2026-10-28 soak data): the Voidwalker Minion (8996) is a level 47 guardian of the Searing Blade Warlock (11324) and
// is unsummoned when the Warlock dies. The Warlock stands about 25 yards behind the tank's target, past the 12 yard pack reach, so the damage dealers
// used to turn on the Voidwalker (kill_priority rank 1) and never on the Warlock. With the Voidwalker in view, the Warlock is taken first from up to 35
// yards; without a Voidwalker nothing changes (a distant Warlock is left alone).
void ADamageDealerTakesTheWarlockWhoseVoidwalkerIsUp()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; }; // bot 11 is the mage, 12 the warrior, the rest priests
    options.TacticInterrupt = false;
    options.KillPriority = { { 11324, 0 }, { 8996, 1 }, { 11322, 2 }, { 11323, 3 } }; // the shipped dungeon file's order
    NearbyUnit enforcer = Hostile(7, 2.0f, 100);
    enforcer.guid = MakeUnitRef(8, 11323, 7);         // the tank's own target
    NearbyUnit voidwalker = Hostile(8, 5.0f, 100);
    voidwalker.guid = MakeUnitRef(8, 8996, 8);        // joined the melee at the tank
    NearbyUnit warlock = Hostile(9, 27.0f, 100);
    warlock.guid = MakeUnitRef(8, 11324, 9);          // behind, outside the 12 yard pack reach
    auto const run = [&](bool withVoidwalker, bool tacticFocus, std::uint64_t& onWarlock, std::uint64_t& onEnforcer, std::uint64_t& onVoidwalker)
    {
        options.TacticFocus = tacticFocus;
        BotPolicy policy(options);
        policy.SetRouteActive(true);
        WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
        view.bots[2].in_combat = 1;
        view.bots[2].threat_target_guid = enforcer.guid;
        AddNearby(view, 1, enforcer);
        if (withVoidwalker)
            AddNearby(view, 1, voidwalker);
        AddNearby(view, 1, warlock);
        onWarlock = onEnforcer = onVoidwalker = 0;
        for (std::uint64_t at = 0; at < 6000; at += 500)
            for (BotAction const& action : policy.Decide(view, at))
                if (action.bot_guid == 11)
                {
                    onWarlock += action.target_guid == warlock.guid;
                    onEnforcer += action.target_guid == enforcer.guid;
                    onVoidwalker += action.target_guid == voidwalker.guid;
                }
    };
    std::uint64_t onWarlock = 0, onEnforcer = 0, onVoidwalker = 0;
    run(true, true, onWarlock, onEnforcer, onVoidwalker);
    CHECK(onWarlock > 0);       // the Warlock first, 25 yards from the tank's target
    CHECK(onVoidwalker == 0);   // not the Voidwalker (rank 1) that stood nearer
    CHECK(onEnforcer == 0);
    run(true, false, onWarlock, onEnforcer, onVoidwalker); // not a tactic: it holds with focus off, as the golem's engineer does
    CHECK(onWarlock > 0);
    run(false, true, onWarlock, onEnforcer, onVoidwalker); // no Voidwalker up: the distant Warlock is outside the pack reach as before
    CHECK(onWarlock == 0);
    CHECK(onEnforcer > 0);
}

// Razorfen Downs, the second hall (2026-11-07 sim journal, bots 0.45.2): the tank fights a non-elite Splinterbone Centurion (7346) while a Boneflayer
// Ghoul (7347) and a Freezing Spirit (7353, damage x2.8, Frost Nova) are already in the fight. With 129-razorfen-downs.json's kill_priority the damage
// dealers turn on the Spirit, and on the Skeletal Summoner (7342) before it when one is there; with no list they stay on the tank's target, as before.
void RazorfenDownsDamageDealersTakeTheFreezingSpiritFirst()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; }; // bot 11 is the mage, 12 the warrior, the rest priests
    options.TacticInterrupt = false;
    std::map<std::uint32_t, std::uint32_t> const shipped = { { 7342, 0 }, { 7340, 1 }, { 7353, 2 }, { 7345, 3 }, { 7341, 4 }, { 7335, 5 }, { 7337, 6 } }; // the file's order
    NearbyUnit centurion = Hostile(7, 2.0f, 30);
    centurion.guid = MakeUnitRef(8, 7346, 7);   // the tank's target, the weakest
    NearbyUnit ghoul = Hostile(8, 5.0f, 60);
    ghoul.guid = MakeUnitRef(8, 7347, 8);
    NearbyUnit spirit = Hostile(9, 8.0f, 100);
    spirit.guid = MakeUnitRef(8, 7353, 9);
    NearbyUnit summoner = Hostile(10, 9.0f, 100);
    summoner.guid = MakeUnitRef(8, 7342, 10);
    auto const firstTarget = [&](std::map<std::uint32_t, std::uint32_t> const& priority, bool withSummoner)
    {
        options.KillPriority = priority;
        BotPolicy policy(options);
        policy.SetRouteActive(true);
        WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
        view.bots[2].in_combat = 1;
        view.bots[2].threat_target_guid = centurion.guid;
        view.bots[3].in_combat = 1;
        view.bots[3].threat_target_guid = ghoul.guid;
        view.bots[4].in_combat = 1;
        view.bots[4].threat_target_guid = spirit.guid;
        AddNearby(view, 1, centurion);
        AddNearby(view, 1, ghoul);
        AddNearby(view, 1, spirit);
        if (withSummoner)
            AddNearby(view, 1, summoner);
        for (std::uint64_t at = 0; at < 6000; at += 500)
            for (BotAction const& action : policy.Decide(view, at))
                if (action.bot_guid == 11 && action.target_guid != 0)
                    return action.target_guid;
        return std::uint64_t(0);
    };
    CHECK(firstTarget(shipped, false) == spirit.guid);     // the Spirit before the weaker Centurion and the Ghoul
    CHECK(firstTarget(shipped, true) == summoner.guid);    // the Summoner before the Spirit
    CHECK(firstTarget({}, false) == centurion.guid);       // no list: the tank's target, as before 0.45.2
}

// Cc: a mage (Polymorph, spell 118) holds one extra, unengaged Humanoid member of the tank's pack -- not the tank's own target,
// which is already engaged.
void CcHoldsAnUnengagedHumanoidPackMember()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit tankTarget = Hostile(7, 2.0f, 100);
    tankTarget.guid = MakeUnitRef(8, 598, 7);   // the tank's own target, already engaged: never a cc candidate
    NearbyUnit evoker = Hostile(8, 8.0f, 100);
    evoker.guid = MakeUnitRef(8, 1729, 8);      // a Defias Evoker of the same pack: Humanoid, Polymorph can hold it
    view.bots[2].in_combat = 1;
    view.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(view, 1, tankTarget);
    AddNearby(view, 1, evoker);
    bool onEvoker = false, onTankTarget = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11 && action.type == ActionType::kCastSpell && action.spell_id == 118)
            {
                onEvoker = onEvoker || action.target_guid == evoker.guid;
                onTankTarget = onTankTarget || action.target_guid == tankTarget.guid;
            }
    CHECK(onEvoker);
    CHECK(!onTankTarget);
}

// Cc: a creature type with no spell to hold it (Mechanical: the Remote-Controlled Golem, 2520) is never tried -- the policy does
// nothing instead of risking a cast the server would refuse (and recast every tick for ever).
void CcDoesNotHoldACreatureTypeItHasNoSpellFor()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit tankTarget = Hostile(7, 2.0f, 100);
    tankTarget.guid = MakeUnitRef(8, 622, 7);  // Goblin Engineer, Humanoid: the tank's own target, already engaged
    NearbyUnit golem = Hostile(8, 8.0f, 100);
    golem.guid = MakeUnitRef(8, 2520, 8);      // Remote-Controlled Golem: Mechanical, Polymorph cannot hold it
    view.bots[2].in_combat = 1;
    view.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(view, 1, tankTarget);
    AddNearby(view, 1, golem);
    bool anyCc = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11 && action.type == ActionType::kCastSpell && action.spell_id == 118)
                anyCc = true;
    CHECK(!anyCc);
}

// Cc: the dungeon's own kill_priority unit is never held -- the point of cc is one fewer attacker, not a delay on the one the
// party means to kill first.
void CcNeverHoldsTheKillPriorityTarget()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; };
    options.KillPriority = { { 1729, 0 } }; // the Defias Evoker is the one to kill first, not to hold
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit tankTarget = Hostile(7, 2.0f, 100);
    tankTarget.guid = MakeUnitRef(8, 598, 7);
    NearbyUnit evoker = Hostile(8, 8.0f, 100);
    evoker.guid = MakeUnitRef(8, 1729, 8); // Humanoid, and otherwise eligible, but it is the kill_priority unit
    view.bots[2].in_combat = 1;
    view.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(view, 1, tankTarget);
    AddNearby(view, 1, evoker);
    bool onEvoker = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11 && action.type == ActionType::kCastSpell && action.spell_id == 118 && action.target_guid == evoker.guid)
                onEvoker = true;
    CHECK(!onEvoker);
}

// Cc: with the tactic off (the default when DungeonTactics does not list cc) no hold is attempted, whatever the pack holds.
void CcTacticOffMeansNoHold()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; };
    options.TacticCc = false;
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit tankTarget = Hostile(7, 2.0f, 100);
    tankTarget.guid = MakeUnitRef(8, 598, 7);
    NearbyUnit evoker = Hostile(8, 8.0f, 100);
    evoker.guid = MakeUnitRef(8, 1729, 8);
    view.bots[2].in_combat = 1;
    view.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(view, 1, tankTarget);
    AddNearby(view, 1, evoker);
    bool anyCc = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11 && action.type == ActionType::kCastSpell && action.spell_id == 118)
                anyCc = true;
    CHECK(!anyCc);
}

void NoPartyActionsInCombat()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    view.bots[2].in_combat = 1;
    CHECK(!HasGroup(PartyPolicy(5).Decide(view, 0), 12, 1));
}

void IdleBotWalksToItsVendor()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 100.0f);
    auto const actions = VendorPolicy().Decide(view, 0);
    bool found = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.bot_guid == 10)
        {
            found = true;
            CHECK(action.dest_x == 100.0f && action.dest_y == 0.0f);
        }
    CHECK(found);
    CHECK(!HasAction(actions, 10, ActionType::kVendor)); // the visit waits until the bot is in reach
}

void WornGearSendsTheBotToTheRepairman()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 100.0f);
    view.bots[0].repair_x = -200.0f;
    view.bots[0].repair_valid = 1;
    view.bots[0].durability_pct = 20;
    BotPolicy policy = VendorPolicy();
    auto const actions = AfterMount(policy, view, 0);
    bool found = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.bot_guid == 10)
        {
            found = true;
            CHECK(action.dest_x == -200.0f); // the repairman, not the vendor at x = 100
        }
    CHECK(found);
}

void GoodGearGoesToTheNearestVendor()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 100.0f);
    view.bots[0].repair_x = -200.0f;
    view.bots[0].repair_valid = 1;
    view.bots[0].durability_pct = 90;
    auto const actions = VendorPolicy().Decide(view, 0);
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.bot_guid == 10)
            CHECK(action.dest_x == 100.0f);
}

void VendorVisitWhenInReach()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 100.0f);
    view.bots[0].pos_x = 98.0f; // two yards from the vendor
    auto const actions = VendorPolicy().Decide(view, 0);
    CHECK(HasAction(actions, 10, ActionType::kVendor));
    CHECK(!HasAction(actions, 10, ActionType::kMoveTo));
}

void TravelIsDroppedAfterTheTimeout()
{
    BotPolicy policy = VendorPolicy();
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 100.0f);
    CHECK(HasAction(policy.Decide(view, 0), 10, ActionType::kMoveTo));
    auto const stuck = policy.Decide(view, 200000); // still far away after the timeout
    for (BotAction const& action : stuck)                // the walk to the vendor is over (the bot may take a short walk of its own)
        CHECK(!(action.bot_guid == 10 && action.type == ActionType::kMoveTo && action.dest_x == 100.0f));
    CHECK(!HasAction(stuck, 10, ActionType::kVendor));
}

void WalkingBotDoesNotSit()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 100.0f);
    view.bots[0].health = 500; // hurt enough to sit, if it were idle
    auto const actions = VendorPolicy().Decide(view, 0);
    CHECK(HasAction(actions, 10, ActionType::kMoveTo));
    for (BotAction const& action : actions)
        CHECK(!(action.type == ActionType::kRest && action.spell_id == 1));
}

void NoVendorOnTheMapMeansNoWalk()
{
    // The map has no vendor: the visit is tried where the bot stands (the core refuses it if no NPC is in range).
    auto const actions = VendorPolicy().Decide(PartyView({ 10 }), 0);
    CHECK(!HasAction(actions, 10, ActionType::kMoveTo));
    CHECK(HasAction(actions, 10, ActionType::kVendor));
}

void PartyMembersDoNotWalkAlone()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    SetVendor(view, 100.0f);
    auto const actions = VendorPolicy(true).Decide(view, 0);
    CHECK(!HasAction(actions, 10, ActionType::kMoveTo));
    CHECK(HasAction(actions, 12, ActionType::kMoveTo)); // the leader walks; the members follow it
}

void IdleBotWandersToAHubAfterAWhile()
{
    BotPolicy policy = HubPolicy();
    WorldTickSnapshotView view = OnMap(1);
    CHECK(WalkTo(policy.Decide(view, 0), 10) < 0.0f);     // idle starts now
    CHECK(WalkTo(policy.Decide(view, 29000), 10) < 0.0f);
    CHECK(WalkTo(AfterMount(policy, view, 30000), 10) == 500.0f);
}

void ArrivalMovesOnToTheNextHubAfterAnInterval()
{
    BotPolicy policy = HubPolicy();
    WorldTickSnapshotView view = OnMap(1);
    policy.Decide(view, 0);
    CHECK(WalkTo(AfterMount(policy, view, 30000), 10) == 500.0f); // walking to hub 1
    view.bots[0].pos_x = 495.0f;                              // five yards from it: arrived
    CHECK(WalkTo(policy.Decide(view, 31000), 10) < 0.0f);     // waits at the hub
    CHECK(WalkTo(policy.Decide(view, 60000), 10) < 0.0f);     // not a full interval yet
    // The bot is still mounted from the first walk, so this one starts walking at once: AfterMount would read a tick too late.
    CHECK(WalkTo(policy.Decide(view, 61000), 10) == 900.0f);  // next walk, to hub 2
}

void ANoRouteAnswerEndsTheWalkInsteadOfBeingSentEveryTick()
{
    BotPolicy policy = HubPolicy();
    WorldTickSnapshotView view = OnMap(1);
    policy.Decide(view, 0);
    CHECK(WalkTo(AfterMount(policy, view, 30000), 10) == 500.0f);
    view.bots[0].last_action_type = std::uint8_t(ActionType::kMoveTo);
    view.bots[0].last_action_status = BotActionStatus::kNoRoute;
    view.bots[0].last_action_sequence = 7;
    CHECK(WalkTo(policy.Decide(view, 30100), 10) < 0.0f);      // the core found no route: the walk is over
    CHECK(WalkTo(policy.Decide(view, 30150), 10) < 0.0f);      // and it is not sent again on the next tick
    // 500 yards is a long walk: the answer may be a first leg only. The bot has not moved after the settle time, so the route is really missing.
    policy.Decide(view, 34000);
    CHECK(WalkTo(policy.Decide(view, 80000), 10) == 900.0f);   // after the short pause for a walk that failed: the next hub
}

// A long walk the core answered with "no route" is a partial path: the bot walked as far as the mesh reaches. While each leg brings it nearer the same
// move is sent again from where it stopped; a leg that went nowhere gives the walk up.
void ALongWalkWithNoRouteIsChainedWhileItMakesProgress()
{
    BotPolicy policy = HubPolicy();
    WorldTickSnapshotView view = OnMap(1);
    policy.Decide(view, 0);
    CHECK(WalkTo(AfterMount(policy, view, 30000), 10) == 500.0f);
    view.bots[0].last_action_type = std::uint8_t(ActionType::kMoveTo);
    view.bots[0].last_action_status = BotActionStatus::kNoRoute;
    view.bots[0].last_action_sequence = 7;
    CHECK(WalkTo(policy.Decide(view, 30100), 10) < 0.0f);       // nothing is sent again at once
    view.bots[0].moving = 1;
    CHECK(WalkTo(policy.Decide(view, 36000), 10) < 0.0f);       // still walking: no judgement
    view.bots[0].moving = 0;
    view.bots[0].pos_x = 300.0f;                                // the first leg ended 300 yards on, 200 short of the hub
    CHECK(WalkTo(policy.Decide(view, 40000), 10) == 500.0f);    // the same move goes out again from there
    view.bots[0].last_action_sequence = 8;                      // the core again: still no full route
    CHECK(WalkTo(policy.Decide(view, 40100), 10) < 0.0f);
    view.bots[0].pos_x = 420.0f;                                // the second leg
    CHECK(WalkTo(policy.Decide(view, 45000), 10) == 500.0f);
    view.bots[0].last_action_sequence = 9;
    CHECK(WalkTo(policy.Decide(view, 45100), 10) < 0.0f);
    CHECK(WalkTo(policy.Decide(view, 50000), 10) < 0.0f);       // this leg went nowhere (no progress): the route is missing, the walk is given up
    CHECK(WalkTo(policy.Decide(view, 51000), 10) < 0.0f);       // and the refused place is not sent for the pause
}

void AShortWalkWithNoRouteIsNotChained()
{
    BotPolicy policy = HubPolicy();
    WorldTickSnapshotView view = OnMap(1);
    view.bots[0].pos_x = 355.0f;                                // 145 yards from the hub at 500
    policy.Decide(view, 0);
    CHECK(WalkTo(AfterMount(policy, view, 30000), 10) == 500.0f);
    view.bots[0].last_action_type = std::uint8_t(ActionType::kMoveTo);
    view.bots[0].last_action_status = BotActionStatus::kNoRoute;
    view.bots[0].last_action_sequence = 7;
    policy.Decide(view, 30100);
    view.bots[0].pos_x = 440.0f;                                // it did walk, but a short refusal is a plain refusal: no second send
    CHECK(WalkTo(policy.Decide(view, 36000), 10) != 500.0f);    // not the same move again (the walk is over; the next hub may follow)
}

// A random explore point the core has no route to (the point is at the bot's height, and often on ground the navmesh does not cover) is retried
// nearer on the next tick, not after the idle pause.
void ARefusedExplorePointIsRetriedNearerAtOnce()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    view.bots[0].level = 20;
    BotPolicy policy; // no hubs, no grind spots: the bot explores on foot
    policy.Decide(view, 0);
    BotAction first{};
    bool started = false;
    for (BotAction const& action : policy.Decide(view, 11000))
        if (action.type == ActionType::kMoveTo && action.bot_guid == 10)
        {
            first = action;
            started = true;
        }
    CHECK(started);
    float const firstDistance = std::hypot(first.dest_x - view.bots[0].pos_x, first.dest_y - view.bots[0].pos_y);
    view.bots[0].last_action_type = std::uint8_t(ActionType::kMoveTo);
    view.bots[0].last_action_status = BotActionStatus::kNoRoute;
    view.bots[0].last_action_sequence = 3;
    BotAction retry{};
    started = false;
    for (BotAction const& action : policy.Decide(view, 11100))
        if (action.type == ActionType::kMoveTo && action.bot_guid == 10)
        {
            retry = action;
            started = true;
        }
    CHECK(started);                                                                                    // at once, not after the idle pause
    CHECK(std::hypot(retry.dest_x - view.bots[0].pos_x, retry.dest_y - view.bots[0].pos_y) < firstDistance); // and nearer
}

void ABlacksmithAsksForItsLineAndWalksToAnAnvilWhenTheRecipeNeedsOne()
{
    BotPolicy policy(CraftingWithAnAnvil());
    WorldTickSnapshotView view = View(1, 0, {});
    view.bots[0].profession_skill[0] = 164; // Blacksmithing
    view.bots[0].profession_value[0] = 50;
    CHECK((CraftsOf(policy.Decide(view, 1000)) == std::vector<std::uint32_t>{ 129 }));
    CraftAnswer(view, BotActionStatus::kRefused, 1);
    CHECK((CraftsOf(policy.Decide(view, 3000)) == std::vector<std::uint32_t>{ 185 }));
    CraftAnswer(view, BotActionStatus::kRefused, 2);
    CHECK((CraftsOf(policy.Decide(view, 5000)) == std::vector<std::uint32_t>{ 164 }));     // its own line, after the two secondary skills
    CraftAnswer(view, BotActionStatus::kNeedsFocus, 3);                                   // the recipes need an anvil within reach
    CHECK(CraftsOf(policy.Decide(view, 7000)).empty());                                    // no craft while walking
    CHECK(WalkTo(policy.Decide(view, 7500), kBot) == 300.0f);                              // the nearest anvil (the tick of the answer may mount first)
    view.bots[0].pos_x = 295.0f;                                                           // arrived within its radius
    CHECK((CraftsOf(policy.Decide(view, 20000)) == std::vector<std::uint32_t>{ 164 }));    // and it asks again at once
}

void ANeedsFocusAnswerWithNoRouteOrNoTableEndsInAWait()
{
    // The walk gets "no route": it is over, and the line waits half an hour.
    BotPolicy policy(CraftingWithAnAnvil());
    WorldTickSnapshotView view = View(1, 0, {});
    view.bots[0].profession_skill[0] = 164;
    view.bots[0].profession_value[0] = 50;
    policy.Decide(view, 1000);
    CraftAnswer(view, BotActionStatus::kRefused, 1);
    policy.Decide(view, 3000);
    CraftAnswer(view, BotActionStatus::kRefused, 2);
    policy.Decide(view, 5000);
    CraftAnswer(view, BotActionStatus::kNeedsFocus, 3);
    policy.Decide(view, 7000);
    CHECK(WalkTo(policy.Decide(view, 7500), kBot) == 300.0f);
    view.bots[0].last_action_type = std::uint8_t(ActionType::kMoveTo);
    view.bots[0].last_action_status = BotActionStatus::kNoRoute;
    view.bots[0].last_action_sequence = 4;
    CHECK(WalkTo(policy.Decide(view, 9000), kBot) < 0.0f);
    CHECK(CraftsOf(policy.Decide(view, 60000)).empty());                                   // the line waits (the secondary skills too, from their refusals)
    CHECK(CraftsOf(policy.Decide(view, 9000 + 1900000)).size() == 1);                      // and is asked for again after half an hour

    // No table loaded: the bot does not walk, the line waits.
    PolicyOptions none = AggressiveOptions();
    none.Crafting = true;
    BotPolicy blind(none);
    WorldTickSnapshotView plain = View(1, 0, {});
    plain.bots[0].profession_skill[0] = 164;
    plain.bots[0].profession_value[0] = 50;
    blind.Decide(plain, 1000);
    CraftAnswer(plain, BotActionStatus::kRefused, 1);
    blind.Decide(plain, 3000);
    CraftAnswer(plain, BotActionStatus::kRefused, 2);
    blind.Decide(plain, 5000);
    CraftAnswer(plain, BotActionStatus::kNeedsFocus, 3);
    blind.Decide(plain, 7000);
    CHECK(WalkTo(blind.Decide(plain, 7500), kBot) < 0.0f);
}

void AGatheringBotAsksForItsSkillAndBacksOffAfterARefusal()
{
    PolicyOptions options = AggressiveOptions();
    options.Gathering = true;
    BotPolicy policy(options);
    WorldTickSnapshotView view = View(1, 0, {});
    view.bots[0].profession_skill[1] = 393; // Skinning
    view.bots[0].profession_value[1] = 10;
    CHECK((GathersOf(policy.Decide(view, 1000)) == std::vector<std::uint32_t>{ 393 }));
    view.bots[0].last_action_type = std::uint8_t(ActionType::kGather);
    view.bots[0].last_action_status = BotActionStatus::kRefused;                            // nothing near
    view.bots[0].last_action_sequence = 1;
    CHECK(GathersOf(policy.Decide(view, 5000)).empty());
    CHECK(GathersOf(policy.Decide(view, 60000)).empty());
    CHECK((GathersOf(policy.Decide(view, 95000)) == std::vector<std::uint32_t>{ 393 }));    // asked again after the wait
    view.bots[0].last_action_status = BotActionStatus::kMoving;                             // the core is walking the bot to the corpse
    view.bots[0].last_action_sequence = 2;
    CHECK(GathersOf(policy.Decide(view, 96000)).empty());                                   // the answer is read
    CHECK((GathersOf(policy.Decide(view, 98000)) == std::vector<std::uint32_t>{ 393 }));    // and the next ask comes soon
    // No gathering profession, or the setting off: never asked.
    BotPolicy none(options);
    CHECK(GathersOf(none.Decide(View(1, 0, {}), 1000)).empty());
    BotPolicy off(AggressiveOptions());
    WorldTickSnapshotView skinner = View(1, 0, {});
    skinner.bots[0].profession_skill[1] = 393;
    skinner.bots[0].profession_value[1] = 10;
    CHECK(GathersOf(off.Decide(skinner, 1000)).empty());
}

void NoHubsOnTheBotsMapMeansNoWalk()
{
    BotPolicy policy = HubPolicy();
    CHECK(WalkTo(policy.Decide(OnMap(0), 0), 10) < 0.0f);
    // Map 0 has no hubs here: the bot takes a short walk on foot after a quiet spell instead (it never walks to the hubs of map 1).
    float const x = WalkTo(policy.Decide(OnMap(0), 30000), 10);
    CHECK(x > -120.0f && x < 120.0f);
}

void AnythingSeenStopsTheWalk()
{
    BotPolicy policy = HubPolicy();
    WorldTickSnapshotView view = OnMap(1);
    policy.Decide(view, 0);
    CHECK(WalkTo(AfterMount(policy, view, 30000), 10) == 500.0f);
    // A hostile creature comes into view: the bot fights it instead of walking on.
    NearbyUnit const wolf = Hostile(7, 2.0f, 100);
    AddNearby(view, 0, wolf);
    auto const actions = policy.Decide(view, 31000);
    CHECK(WalkTo(actions, 10) < 0.0f);
    CHECK(HasAction(actions, 10, ActionType::kAttackTarget, wolf.guid));
}

void HurtBotRestsInsteadOfWalking()
{
    WorldTickSnapshotView view = OnMap(1);
    view.bots[0].health = 500; // below the rest threshold
    BotPolicy policy = HubPolicy();
    policy.Decide(view, 0);
    CHECK(WalkTo(policy.Decide(view, 30000), 10) < 0.0f);
}

void PartyMembersDoNotWanderAlone()
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.map_id = 1;
    PolicyOptions options = AggressiveOptions();
    options.HubsByMap[1] = { Point{ 500.0f, 0.0f, 0.0f } };
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    BotPolicy policy(options);
    policy.Decide(view, 0);
    CHECK(WalkTo(policy.Decide(view, 30000), 10) < 0.0f); // a member: it follows its leader
}

} // namespace

void RunPolicyTestsPart2()
{
    IdleBotWandersToAHubAfterAWhile();
    ArrivalMovesOnToTheNextHubAfterAnInterval();
    ANoRouteAnswerEndsTheWalkInsteadOfBeingSentEveryTick();
    ALongWalkWithNoRouteIsChainedWhileItMakesProgress();
    AShortWalkWithNoRouteIsNotChained();
    ARefusedExplorePointIsRetriedNearerAtOnce();
    ABlacksmithAsksForItsLineAndWalksToAnAnvilWhenTheRecipeNeedsOne();
    ANeedsFocusAnswerWithNoRouteOrNoTableEndsInAWait();
    AGatheringBotAsksForItsSkillAndBacksOffAfterARefusal();
    NoHubsOnTheBotsMapMeansNoWalk();
    AnythingSeenStopsTheWalk();
    HurtBotRestsInsteadOfWalking();
    PartyMembersDoNotWanderAlone();
    IdleBotWalksToItsVendor();
    WornGearSendsTheBotToTheRepairman();
    GoodGearGoesToTheNearestVendor();
    VendorVisitWhenInReach();
    TravelIsDroppedAfterTheTimeout();
    WalkingBotDoesNotSit();
    PartyMembersDoNotWalkAlone();
    NoVendorOnTheMapMeansNoWalk();
    UnknownSideBotsAreLeftOutOfParties();
    ABotInAHumansPartyIsLeftAlone();
    OnlyTheFirstPartyIsFormed();
    InvitesWaitForTheRetry();
    InvitedMemberIsNotInvitedAgain();
    ATankLeadsThePartyWhateverItsGuid();
    MemberAcceptsItsLeadersInvite();
    MemberLeavesAForeignParty();
    MemberFollowsItsLeaderUntilClose();
    MemberDoesNotFollowALeaderOnAnotherMap();
    MemberFightsItsLeadersTarget();
    ADamageDealerFightsThePriorityCasterOfThePack();
    ADamageDealerFocusesTheEngineerWhenAGolemIsUp();
    ACasterOfThePackIsTargetedForAnInterrupt();
    ADamageDealerDoesNotWidenPastTwoAlreadyEngagedHostiles();
    TheVoidwalkerCompanionStaysReachablePastTheEngagementCapToo();
    ADamageDealerTakesTheWarlockWhoseVoidwalkerIsUp();
    RazorfenDownsDamageDealersTakeTheFreezingSpiritFirst();
    CcHoldsAnUnengagedHumanoidPackMember();
    CcDoesNotHoldACreatureTypeItHasNoSpellFor();
    CcNeverHoldsTheKillPriorityTarget();
    CcTacticOffMeansNoHold();
    NoPartyActionsInCombat();
}
