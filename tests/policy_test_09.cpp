// SPDX-License-Identifier: GPL-2.0-or-later
// Policy tests, part 9 (tank pickup of loose mobs, healer combat mana floor; the helpers are in policy_test_support.h).
#include "policy_test_support.h"

namespace {

using forever_module::PartyMember;

constexpr std::uint64_t kMobOnTank = MakeUnitRef(8, 1000, 21);
constexpr std::uint64_t kLooseOne = MakeUnitRef(8, 1001, 22);
constexpr std::uint64_t kLooseTwo = MakeUnitRef(8, 1002, 23);

NearbyUnit MobAt(std::uint64_t ref, float x)
{
    NearbyUnit unit = Hostile(1, x, 100);
    unit.guid = ref;
    return unit;
}

// A warrior (10, the tank by class in a party), a rogue (11) and a priest (12) in one group, all in combat. The tank is on `kMobOnTank`; each mob of `loose` is in the party view's loose list,
// on the rogue, and in the tank's view of the world at `looseX` yards.
WorldTickSnapshotView TankView(std::vector<std::uint64_t> const& loose, float looseX, std::function<std::uint32_t(std::uint64_t)> const& classOf)
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 }, classOf);
    view.max_nearby_units = 8;
    view.nearby.resize(view.bots.size() * view.max_nearby_units);
    for (std::size_t i = 0; i < view.bots.size(); ++i)
    {
        view.bots[i].in_combat = 1;
        view.bots[i].party_view_index = 0;
        view.bots[i].party_member_index = static_cast<std::uint32_t>(i);
    }
    view.bots[0].threat_target_guid = kMobOnTank;
    AddNearby(view, 0, MobAt(kMobOnTank, 4.0f));
    for (std::uint64_t ref : loose)
        AddNearby(view, 0, MobAt(ref, looseX));
    forever_module::PartyView party;
    party.group_id = 5;
    party.member_first = 0;
    party.member_count = 3;
    party.in_combat = 1;
    party.loose_count = static_cast<std::uint32_t>(loose.size());
    for (std::size_t i = 0; i < loose.size(); ++i)
    {
        party.loose_mobs[i].unit_ref = loose[i];
        party.loose_mobs[i].victim_ref = PlayerOf(11);
        party.loose_mobs[i].since_ms = 500;
    }
    view.party_views.push_back(party);
    for (std::uint64_t counter : { 10, 11, 12 })
    {
        PartyMember member;
        member.guid = PlayerOf(counter);
        member.online = 1;
        member.is_bot = 1;
        member.health = 1000;
        member.max_health = 1000;
        member.role = counter == 10 ? forever_module::kPartyRoleTank : counter == 12 ? forever_module::kPartyRoleHealer : forever_module::kPartyRoleDamage;
        member.victim_ref = counter == 10 ? kMobOnTank : 0;
        view.party_members.push_back(member);
    }
    return view;
}

PolicyOptions TankOptions(std::function<std::uint32_t(std::uint64_t)> const& classOf)
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.PartyViewPlanner = true;
    options.ClassOf = classOf;
    return options;
}

auto const kClasses = [](std::uint64_t guid) { return guid == 10 ? 1u : guid == 11 ? 4u : 5u; };   // warrior, rogue, priest

void TankTauntsALooseMob()
{
    BotPolicy policy(TankOptions(kClasses));
    std::vector<BotAction> const actions = policy.Decide(TankView({ kLooseOne }, 6.0f, kClasses), 1000);
    CHECK(CastsSpell(actions, 10, 355, kLooseOne));
    // The taunt is not sent again at once (retry window), and a second tick does not stack it with another cast.
    CHECK(!CastsSpell(policy.Decide(TankView({ kLooseOne }, 6.0f, kClasses), 1100), 10, 355, kLooseOne));
}

void TankPickupNeedsThePartyViewPlannerAndTheKey()
{
    PolicyOptions off = TankOptions(kClasses);
    off.PartyViewPlanner = false;   // the code default
    BotPolicy withoutView(off);
    CHECK(!CastsSpell(withoutView.Decide(TankView({ kLooseOne }, 6.0f, kClasses), 1000), 10, 355, kLooseOne));
    PolicyOptions noKey = TankOptions(kClasses);
    noKey.TankPickup = false;
    BotPolicy keyOff(noKey);
    CHECK(!CastsSpell(keyOff.Decide(TankView({ kLooseOne }, 6.0f, kClasses), 1000), 10, 355, kLooseOne));
}

void TankLeavesAMobOutOfReachAndAMobThatIsItsOwn()
{
    BotPolicy far(TankOptions(kClasses));
    CHECK(!CastsSpell(far.Decide(TankView({ kLooseOne }, 60.0f, kClasses), 1000), 10, 355, kLooseOne));
    // Nothing loose: no pickup cast on any mob but the rotation's own.
    BotPolicy none(TankOptions(kClasses));
    CHECK(!CastsSpell(none.Decide(TankView({}, 6.0f, kClasses), 1000), 10, 355, kLooseOne));
}

void TankUsesChallengingShoutForTwoLooseMobs()
{
    BotPolicy policy(TankOptions(kClasses));
    std::vector<BotAction> const actions = policy.Decide(TankView({ kLooseOne, kLooseTwo }, 6.0f, kClasses), 1000);
    CHECK(CastsSpell(actions, 10, 1161));
    CHECK(!CastsSpell(actions, 10, 355));   // one cast per global cooldown: the shout, not the taunt
    // One loose mob is a single taunt, not the shout.
    BotPolicy single(TankOptions(kClasses));
    std::vector<BotAction> const one = single.Decide(TankView({ kLooseOne }, 6.0f, kClasses), 1000);
    CHECK(!CastsSpell(one, 10, 1161));
}

void TankPickupNotBelowTheTaunt()
{
    // A cooldown reported by the core holds the spell back.
    BotPolicy policy(TankOptions(kClasses));
    WorldTickSnapshotView view = TankView({ kLooseOne }, 6.0f, kClasses);
    forever_module::SpellCooldown cooldown;
    cooldown.spell_id = 355;
    cooldown.remaining_ms = 6000;
    view.bots[0].spell_cooldowns[0] = cooldown;
    CHECK(!CastsSpell(policy.Decide(view, 1000), 10, 355, kLooseOne));
}

// TankLoosePickup: bot 13 is a second warrior in the group (the ZG and Onyxia rosters have several), standing at `secondX`; bot 10 stands at 0.
auto const kTwoTanks = [](std::uint64_t guid) { return guid == 10 || guid == 13 ? 1u : guid == 11 ? 4u : 5u; };

WorldTickSnapshotView TwoTankView(std::vector<std::uint64_t> const& loose, float looseX, float secondX)
{
    WorldTickSnapshotView view = TankView(loose, looseX, kTwoTanks);
    BotSnapshot second = view.bots[0];
    second.guid = 13;
    second.pos_x = secondX;
    second.nearby_count = 0;
    second.threat_target_guid = kMobOnTank;
    second.party_member_index = 3;
    view.bots.push_back(second);
    view.nearby.resize(view.bots.size() * view.max_nearby_units);
    AddNearby(view, 3, MobAt(kMobOnTank, secondX + 4.0f));
    for (std::uint64_t ref : loose)
        AddNearby(view, 3, MobAt(ref, looseX));
    view.party_views[0].member_count = 4;
    PartyMember member = view.party_members[0];
    member.guid = PlayerOf(13);
    member.victim_ref = kMobOnTank;
    view.party_members.push_back(member);
    return view;
}

PolicyOptions LooseOptions()
{
    PolicyOptions options = TankOptions(kTwoTanks);
    options.PartySize = 4;
    options.TankLoosePickup = true;
    return options;
}

void LooseElectsTheNearestTankOnly()
{
    std::vector<std::string> events;
    PolicyOptions options = LooseOptions();
    options.OnEvent = [&](std::string const& line) { events.push_back(line); };
    BotPolicy watched(options);
    watched.SetRouteActive(true);
    std::vector<BotAction> const actions = watched.Decide(TwoTankView({ kLooseOne }, 25.0f, 24.0f), 1000);
    CHECK(CastsSpell(actions, 13, 355, kLooseOne));
    CHECK(!CastsSpell(actions, 10, 355, kLooseOne));
    bool logged = false;
    for (std::string const& line : events)
        logged = logged || (line.rfind("tank_loose: tank=13 target=1001 victim=", 0) == 0 && line.find("reason=newest") != std::string::npos);
    CHECK(logged);
    // The pick is not repeated inside the cooldown, by either tank.
    std::vector<BotAction> const again = watched.Decide(TwoTankView({ kLooseOne }, 25.0f, 24.0f), 1500);
    CHECK(!CastsSpell(again, 13, 355, kLooseOne) && !CastsSpell(again, 10, 355, kLooseOne));
}

void LooseKeepsTheOldWayWithoutTheKeyOrTheRoute()
{
    PolicyOptions off = LooseOptions();
    off.TankLoosePickup = false;
    BotPolicy keyOff(off);
    keyOff.SetRouteActive(true);
    std::vector<BotAction> const both = keyOff.Decide(TwoTankView({ kLooseOne }, 25.0f, 24.0f), 1000);
    CHECK(CastsSpell(both, 10, 355, kLooseOne) && CastsSpell(both, 13, 355, kLooseOne));
    BotPolicy noRoute(LooseOptions());   // a party that is not on a route: the gate is off
    std::vector<BotAction> const plain = noRoute.Decide(TwoTankView({ kLooseOne }, 25.0f, 24.0f), 1000);
    CHECK(CastsSpell(plain, 10, 355, kLooseOne) && CastsSpell(plain, 13, 355, kLooseOne));
}

void LooseTakesTheMobOnTheHealerFirst()
{
    PolicyOptions options = LooseOptions();
    std::vector<std::string> events;
    options.OnEvent = [&](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = TwoTankView({ kLooseOne, kLooseTwo }, 6.0f, 60.0f);
    view.nearby[2 + 1].pos_x = 20.0f;                       // bot 10's slot 3 is the second loose mob: out of the area taunt's ten yards
    view.party_views[0].loose_mobs[1].victim_ref = PlayerOf(12);   // the priest
    CHECK(view.party_views[0].loose_mobs[0].unit_ref == kLooseOne);
    std::vector<BotAction> const actions = policy.Decide(view, 1000);
    CHECK(CastsSpell(actions, 10, 355, kLooseTwo));
    CHECK(!CastsSpell(actions, 10, 355, kLooseOne));
    bool healer = false;
    for (std::string const& line : events)
        healer = healer || line.find("reason=healer act=taunt") != std::string::npos;
    CHECK(healer);
}

bool MovesAhead(std::vector<BotAction> const& actions, std::uint64_t bot)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.bot_guid == bot && action.dest_x > 0.5f)
            return true;
    return false;
}

void LooseWalksTowardAMobOutOfReachAndNeverWithoutTheChaseYards()
{
    BotPolicy policy(LooseOptions());
    policy.SetRouteActive(true);
    std::vector<BotAction> const actions = policy.Decide(TwoTankView({ kLooseOne }, 36.0f, -60.0f), 1000);
    CHECK(MovesAhead(actions, 10) && !CastsSpell(actions, 10, 355, kLooseOne));
    PolicyOptions still = LooseOptions();
    still.TankLooseChaseYards = 0;
    BotPolicy stays(still);
    stays.SetRouteActive(true);
    CHECK(!MovesAhead(stays.Decide(TwoTankView({ kLooseOne }, 36.0f, -60.0f), 1000), 10));
    PolicyOptions far = LooseOptions();
    far.TankLooseRangeYards = 30;   // beyond the range setting the mob is left alone
    BotPolicy leaves(far);
    leaves.SetRouteActive(true);
    CHECK(!MovesAhead(leaves.Decide(TwoTankView({ kLooseOne }, 36.0f, -60.0f), 1000), 10));
}

void LooseNeedsLineOfSight()
{
    BotPolicy policy(LooseOptions());
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = TwoTankView({ kLooseOne }, 10.0f, 60.0f);
    view.nearby[1].state_flags = forever_module::kNearbyLosKnown;   // bot 10's second slot: the loose mob, line of sight known and blocked
    std::vector<BotAction> const blocked = policy.Decide(view, 1000);
    CHECK(!CastsSpell(blocked, 10, 355, kLooseOne));
    BotPolicy seen(LooseOptions());
    seen.SetRouteActive(true);
    view.nearby[1].state_flags = forever_module::kNearbyLosKnown | forever_module::kNearbyInLosOfBot;
    CHECK(CastsSpell(seen.Decide(view, 1000), 10, 355, kLooseOne));
}

// Healer combat mana floor. Bot 12 is a priest in the party, in combat on a hostile in reach; Smite (585) is its attack row.
WorldTickSnapshotView HealerView(std::uint32_t power)
{
    WorldTickSnapshotView view = TankView({}, 6.0f, kClasses);
    view.bots[2].power = power;
    view.bots[2].max_power = 5000;
    view.bots[2].threat_target_guid = kMobOnTank;
    AddNearby(view, 2, MobAt(kMobOnTank, 8.0f));
    return view;
}

bool HealerAttacks(PolicyOptions options, std::uint32_t power)
{
    BotPolicy policy(options);
    std::vector<BotAction> const actions = policy.Decide(HealerView(power), 900);   // a fresh policy: its first decision is the first cast
    return CastsSpell(actions, 12, 585) || CastsSpell(actions, 12, 589) || CastsSpell(actions, 12, 8092);   // Smite, Shadow Word: Pain, Mind Blast
}

void HealerKeepsAManaFloorInCombat()
{
    PolicyOptions options = TankOptions(kClasses);
    options.PartyViewPlanner = false;
    CHECK(options.HealerCombatManaPct == 25);       // the default
    CHECK(HealerAttacks(options, 4000));            // 80 % mana: attacks as before
    CHECK(!HealerAttacks(options, 1000));           // 20 % mana, under the floor: no attack row
    options.HealerCombatManaPct = 0;
    CHECK(HealerAttacks(options, 1000));            // floor off
    options.HealerCombatManaPct = 90;
    CHECK(!HealerAttacks(options, 4000));           // a higher floor holds the 80 % healer too
}

// PullTankFirst: a party of five on the route, bot 12 the warrior leader with a target 20 yards from the mage (bot 11), which is not in combat with it.
struct TankFirstSetup
{
    PolicyOptions options;
    WorldTickSnapshotView view;
    std::uint64_t mob = 0;
};

TankFirstSetup TankFirstParty(bool tankFirst, std::uint32_t tankInCombat, std::uint32_t tankLife = 0)
{
    TankFirstSetup setup;
    setup.options = AggressiveOptions();
    setup.options.PartySize = 5;
    setup.options.PartyCount = 1;
    setup.options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; };
    setup.options.PullTankFirst = tankFirst;
    setup.view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit target = Hostile(7, 20.0f, 100);
    target.guid = MakeUnitRef(8, 598, 7);
    setup.mob = target.guid;
    setup.view.bots[2].threat_target_guid = target.guid;
    setup.view.bots[2].in_combat = tankInCombat;
    setup.view.bots[2].life_state = tankLife;
    AddNearby(setup.view, 1, target);
    return setup;
}

// The first time (ms) bot 11 acts on the mob, or 0 if it never does inside 20 s.
std::uint64_t MageEngagesAt(TankFirstSetup const& setup)
{
    BotPolicy policy(setup.options);
    policy.SetRouteActive(true);
    for (std::uint64_t at = 0; at <= 20000; at += 500)
        for (BotAction const& action : policy.Decide(setup.view, at))
            if (action.bot_guid == 11 && action.target_guid == setup.mob)
                return at == 0 ? 1 : at;
    return 0;
}

void MemberHoldsWhileTheTankIsNotInCombat()
{
    TankFirstSetup setup = TankFirstParty(true, 0);
    std::uint64_t const at = MageEngagesAt(setup);
    CHECK(at >= 12000);   // not before the hold ran out (the next test): the member stands on the route meanwhile
}

void MemberEngagesOnceTheTankIsInCombat()
{
    TankFirstSetup setup = TankFirstParty(true, 1);
    std::uint64_t const at = MageEngagesAt(setup);
    CHECK(at != 0 && at < 6000);
}

void PullHoldExpiresAfterTheHoldTime()
{
    TankFirstSetup setup = TankFirstParty(true, 0);
    std::uint64_t const at = MageEngagesAt(setup);
    CHECK(at >= 12000 && at <= 14000);
    setup.options.PullTankHoldMs = 4000;
    std::uint64_t const shorter = MageEngagesAt(setup);
    CHECK(shorter >= 4000 && shorter < at);
}

void DeadTankDoesNotFreezeTheParty()
{
    TankFirstSetup setup = TankFirstParty(true, 0, 1);   // the tank is dead: no tank to wait for
    std::uint64_t const at = MageEngagesAt(setup);
    CHECK(at < 6000);
}

void PullTankFirstOffKeepsTheOldBehaviour()
{
    TankFirstSetup setup = TankFirstParty(false, 0);
    std::uint64_t const at = MageEngagesAt(setup);
    CHECK(at != 0 && at < 6000);
}

void PullHoldLeavesATargetWithinReachAlone()
{
    TankFirstSetup setup = TankFirstParty(true, 0);
    setup.view.nearby[4].pos_x = 8.0f;   // the mage's slot: the mob is 8 yards away, inside PullMemberRangeYards
    std::uint64_t const at = MageEngagesAt(setup);
    CHECK(at != 0 && at < 6000);
}

// A target with a `pullpos` row is not excused from the hold for standing within reach of a member: the tank pulls it from its position, the members wait.
void PullPositionHoldsEvenAMemberNearTheTarget()
{
    for (bool row : { false, true })
    {
        TankFirstSetup setup = TankFirstParty(true, 0);
        std::vector<std::string> errors;
        if (row)
            setup.options.RaidRules = std::make_shared<raid_rules::Table>(raid_rules::Parse("pullpos|36|598|0|-10|0|4|shelf\n", errors));
        for (BotSnapshot& bot : setup.view.bots)
            bot.map_id = 36;
        setup.view.nearby[4].pos_x = 8.0f;   // inside PullMemberRangeYards
        std::uint64_t const at = MageEngagesAt(setup);
        CHECK(row ? at >= 12000 : (at != 0 && at < 6000));
    }
}

void PullHoldNeverTouchesASoloBot()
{
    PolicyOptions options = AggressiveOptions();
    options.PullTankFirst = true;
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10 });
    NearbyUnit mob = Hostile(7, 5.0f, 100);
    AddNearby(view, 0, mob);
    bool acted = false;
    for (std::uint64_t at = 0; at < 4000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            acted = acted || (action.bot_guid == 10 && action.target_guid == mob.guid);
    CHECK(acted);
}

void PullHoldLogsOncePerTenSeconds()
{
    TankFirstSetup setup = TankFirstParty(true, 0);
    std::vector<std::string> events;
    setup.options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(setup.options);
    policy.SetRouteActive(true);
    for (std::uint64_t at = 1000; at < 10000; at += 500)   // not 0: the log stamp's 0 means never
        policy.Decide(setup.view, at);
    std::size_t holds = 0;
    for (std::string const& line : events)
        if (line.find("pull_hold: bot=11 reason=tank_first tank=12 target=598 held_ms=") == 0)
            ++holds;
    CHECK(holds == 1);
}

// The gaps of the tank-first pull: the member's own threat target while the tank is not fighting, and PullMinHpPct.
TankFirstSetup OwnTargetParty(bool tankFirst)
{
    TankFirstSetup setup = TankFirstParty(tankFirst, 0);
    setup.view.bots[2].threat_target_guid = 0;                 // the tank has no target ...
    setup.view.bots[1].threat_target_guid = setup.mob;         // ... but the mage already has its own
    return setup;
}

void MemberOwnTargetIsHeldWhileTheTankIsQuiet()
{
    CHECK(MageEngagesAt(OwnTargetParty(false)) != 0);          // off: as before
    CHECK(MageEngagesAt(OwnTargetParty(true)) == 0);           // on: the member waits for the tank, however long
}

void MemberOwnTargetWithinReachOrAttackingIsNotHeld()
{
    TankFirstSetup near = OwnTargetParty(true);
    near.view.nearby[4].pos_x = 8.0f;
    CHECK(MageEngagesAt(near) != 0);
    TankFirstSetup fighting = OwnTargetParty(true);
    fighting.view.nearby[4].victim_ref = PlayerOf(13);        // the mob is on a party member: the party defends
    CHECK(MageEngagesAt(fighting) != 0);
}

void MemberOwnTargetIsHeldInTheLogToo()
{
    TankFirstSetup setup = OwnTargetParty(true);
    std::vector<std::string> events;
    setup.options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(setup.options);
    policy.SetRouteActive(true);
    for (std::uint64_t at = 1000; at < 4000; at += 500)
        policy.Decide(setup.view, at);
    bool logged = false;
    for (std::string const& line : events)
        logged = logged || line.find("pull_hold: bot=11 reason=own_target") == 0;
    CHECK(logged);
}

TankFirstSetup HurtMemberParty(std::uint32_t pct, std::uint32_t mageInCombat = 0)
{
    TankFirstSetup setup = TankFirstParty(true, 1);            // the tank is fighting: the mage would assist at once
    setup.options.PullMinHpPct = 60;
    setup.view.bots[1].max_health = 1800;
    setup.view.bots[1].health = 1800 * pct / 100;
    setup.view.bots[1].in_combat = mageInCombat;
    return setup;
}

void HurtMemberDoesNotJoinTheTanksFight()
{
    CHECK(MageEngagesAt(HurtMemberParty(100)) != 0);           // healthy: assists
    CHECK(MageEngagesAt(HurtMemberParty(49)) == 0);            // revived at 49%: rests
    CHECK(MageEngagesAt(HurtMemberParty(61)) != 0);
    TankFirstSetup off = HurtMemberParty(49);
    off.options.PullMinHpPct = 0;
    CHECK(MageEngagesAt(off) != 0);                            // setting off
    CHECK(MageEngagesAt(HurtMemberParty(49, 1)) != 0);         // attacked (in combat): defends itself
}

void HurtMemberRestEndsAfterTheCap()
{
    TankFirstSetup setup = HurtMemberParty(49);
    setup.options.PullRestMaxMs = 20000;
    BotPolicy policy(setup.options);
    policy.SetRouteActive(true);
    std::uint64_t first = 0;
    for (std::uint64_t at = 1000; at <= 60000 && first == 0; at += 1000)
    {
        setup.view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(setup.view, at))
            if (action.bot_guid == 11 && action.target_guid == setup.mob)
                first = at;
    }
    CHECK(first >= 21000 && first <= 25000);
}

void HurtMemberLogsAndStandsStill()
{
    TankFirstSetup setup = HurtMemberParty(49);
    std::vector<std::string> events;
    setup.options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(setup.options);
    policy.SetRouteActive(true);
    bool moved = false;
    for (std::uint64_t at = 1000; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(setup.view, at))
            moved = moved || (action.bot_guid == 11 && (action.type == ActionType::kMoveTo || action.type == ActionType::kFollow));
    bool logged = false;
    for (std::string const& line : events)
        logged = logged || line.find("pull_hold: bot=11 reason=low_hp hp_pct=49") == 0;
    CHECK(logged);
    CHECK(!moved);
}

// The ready gate before a pull: a mana user short of mana rests (pull_hold reason=low_mana) while the tank is quiet, at most PullReadyMaxMs.
TankFirstSetup DryMageParty(std::uint32_t manaPct, std::uint32_t tankInCombat)
{
    TankFirstSetup setup = TankFirstParty(false, tankInCombat);   // tank-first off: only the ready gate holds the mage
    setup.options.PullReadyManaPct = 60;
    setup.options.PullReadyMaxMs = 8000;
    setup.view.bots[1].power_type = 0;
    setup.view.bots[1].max_power = 1000;
    setup.view.bots[1].power = 10 * manaPct;
    return setup;
}

void DryMemberRestsBeforeAPullAndNotLongerThanTheCap()
{
    CHECK(MageEngagesAt(DryMageParty(100, 0)) != 0 && MageEngagesAt(DryMageParty(100, 0)) < 6000);   // full mana: pulls as before
    std::uint64_t const at = MageEngagesAt(DryMageParty(30, 0));
    CHECK(at >= 8000 && at <= 11000);                                                                // dry: waits for the cap
    CHECK(MageEngagesAt(DryMageParty(61, 0)) < 6000);
    TankFirstSetup off = DryMageParty(30, 0);
    off.options.PullReadyManaPct = 0;
    CHECK(MageEngagesAt(off) < 6000);                                                                // setting off
    CHECK(MageEngagesAt(DryMageParty(30, 1)) < 6000);                                                // the tank is already fighting: the mage helps
}

void DryMemberLogsLowMana()
{
    TankFirstSetup setup = DryMageParty(30, 0);
    std::vector<std::string> events;
    setup.options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(setup.options);
    policy.SetRouteActive(true);
    for (std::uint64_t at = 1000; at < 6000; at += 500)
        policy.Decide(setup.view, at);
    bool logged = false;
    for (std::string const& line : events)
        logged = logged || line.find("pull_hold: bot=11 reason=low_mana hp_pct=100 mana_pct=30") == 0;
    CHECK(logged);
}

// The route leader does not walk on while a member is below PullMinHpPct or PullReadyManaPct; after PullReadyMaxMs it does.
void LeaderHoldsTheNextPullForAWeakMember(bool mana)
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f } };
    options.PullMinHpPct = 60;
    options.PullReadyManaPct = 60;
    options.PullReadyMaxMs = 30000;
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 50.0f;
    if (mana)
    {
        view.bots[3].power_type = 0;
        view.bots[3].max_power = 1000;
        view.bots[3].power = 200;
    }
    else
        view.bots[3].health = 400;
    std::uint32_t early = 0, late = 0;
    for (std::uint64_t now = 0; now <= 20000; now += 1000)
        early += LeaderWalk(policy.Decide(view, now)) == 100.0f;
    for (std::uint64_t now = 40000; now <= 60000; now += 1000)
        late += LeaderWalk(policy.Decide(view, now)) == 100.0f;
    CHECK(early == 0);                                                // held while the member is weak
    CHECK(late >= 1);                                                 // PullReadyMaxMs passed: it goes on
    bool logged = false;
    for (std::string const& line : events)
        logged = logged || line.find(std::string("pull_hold: bot=12 reason=") + (mana ? "low_mana" : "low_hp")) == 0;
    CHECK(logged);
    view.bots[3].health = 1000;
    view.bots[3].power = 1000;
    BotPolicy fit(options);
    fit.SetRouteActive(true);
    std::uint32_t walks = 0;
    for (std::uint64_t now = 0; now <= 5000; now += 1000)
        walks += LeaderWalk(fit.Decide(view, now)) == 100.0f;
    CHECK(walks >= 1);                                                // everybody fit: no hold
}

void LeaderHoldsForAHurtMember() { LeaderHoldsTheNextPullForAWeakMember(false); }
void LeaderHoldsForADryMember() { LeaderHoldsTheNextPullForAWeakMember(true); }

// The wipe hold from the director: members rest and the leader waits until it is free; a rewind sets the leader's route index back once.
void WipeHoldHoldsMembersAndLeaderUntilFreed()
{
    auto const make = [](WipeHold& hold, std::vector<std::string>& events)
    {
        PolicyOptions options = AggressiveOptions();
        options.PartySize = 5;
        options.ClassOf = WarriorLeads;
        options.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f } };
        options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
        auto policy = std::make_unique<BotPolicy>(options);
        policy->SetRouteActive(true);
        policy->SetWipeHoldFor([&hold](std::uint64_t) { return hold; });
        return policy;
    };
    WipeHold hold;
    hold.Hold = true;
    std::vector<std::string> events;
    auto policy = make(hold, events);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 50.0f;
    std::uint32_t walks = 0;
    for (std::uint64_t now = 0; now <= 10000; now += 1000)
        walks += LeaderWalk(policy->Decide(view, now)) == 100.0f;
    CHECK(walks == 0);
    bool logged = false;
    for (std::string const& line : events)
        logged = logged || line.find("pull_hold: bot=12 reason=wipe_hold") == 0;
    CHECK(logged);
    hold.LeaderFree = true;                                           // the tank goes first
    for (std::uint64_t now = 11000; now <= 16000; now += 1000)
        walks += LeaderWalk(policy->Decide(view, now)) == 100.0f;
    CHECK(walks >= 1);
}

void WipeBackoffRewindsTheLeadersRouteIndexOnce()
{
    WipeHold hold;
    std::vector<std::string> events;
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 300.0f, 0.0f, 0.0f }, Point{ 500.0f, 0.0f, 0.0f } };
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    policy.SetWipeHoldFor([&hold](std::uint64_t) { return hold; });
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 295.0f;                                           // at point 1: the leader's index is there
    float walk = -1.0f;
    for (std::uint64_t now = 0; now <= 3000; now += 1000)
        if (float const w = LeaderWalk(policy.Decide(view, now)); w >= 0.0f)
            walk = w;
    CHECK(walk == 500.0f);                                            // it walks to point 2 (point 1 is reached or passed)
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 250.0f;                                           // clear of every point
    hold.Epoch = 1;
    hold.RewindTo = 0;
    walk = -1.0f;
    for (std::uint64_t now = 5000; now <= 9000; now += 1000)
        if (float const w = LeaderWalk(policy.Decide(view, now)); w >= 0.0f)
            walk = w;
    CHECK(walk == 100.0f);                                            // set back to point 0
    bool logged = false;
    for (std::string const& line : events)
        logged = logged || line.find("wipe_recovery: bot=12 reason=rewind from=2 to=0") == 0;
    CHECK(logged);
}

} // namespace

void RunPolicyTestsPart9()
{
    TankTauntsALooseMob();
    TankPickupNeedsThePartyViewPlannerAndTheKey();
    TankLeavesAMobOutOfReachAndAMobThatIsItsOwn();
    TankUsesChallengingShoutForTwoLooseMobs();
    TankPickupNotBelowTheTaunt();
    LooseElectsTheNearestTankOnly();
    LooseKeepsTheOldWayWithoutTheKeyOrTheRoute();
    LooseTakesTheMobOnTheHealerFirst();
    LooseWalksTowardAMobOutOfReachAndNeverWithoutTheChaseYards();
    LooseNeedsLineOfSight();
    HealerKeepsAManaFloorInCombat();
    MemberHoldsWhileTheTankIsNotInCombat();
    MemberEngagesOnceTheTankIsInCombat();
    PullHoldExpiresAfterTheHoldTime();
    DeadTankDoesNotFreezeTheParty();
    PullTankFirstOffKeepsTheOldBehaviour();
    PullHoldLeavesATargetWithinReachAlone();
    PullPositionHoldsEvenAMemberNearTheTarget();
    PullHoldNeverTouchesASoloBot();
    PullHoldLogsOncePerTenSeconds();
    MemberOwnTargetIsHeldWhileTheTankIsQuiet();
    MemberOwnTargetWithinReachOrAttackingIsNotHeld();
    MemberOwnTargetIsHeldInTheLogToo();
    HurtMemberDoesNotJoinTheTanksFight();
    HurtMemberRestEndsAfterTheCap();
    HurtMemberLogsAndStandsStill();
    DryMemberRestsBeforeAPullAndNotLongerThanTheCap();
    DryMemberLogsLowMana();
    LeaderHoldsForAHurtMember();
    LeaderHoldsForADryMember();
    WipeHoldHoldsMembersAndLeaderUntilFreed();
    WipeBackoffRewindsTheLeadersRouteIndexOnce();
}
