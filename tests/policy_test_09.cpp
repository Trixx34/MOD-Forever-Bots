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

} // namespace

void RunPolicyTestsPart9()
{
    TankTauntsALooseMob();
    TankPickupNeedsThePartyViewPlannerAndTheKey();
    TankLeavesAMobOutOfReachAndAMobThatIsItsOwn();
    TankUsesChallengingShoutForTwoLooseMobs();
    TankPickupNotBelowTheTaunt();
    HealerKeepsAManaFloorInCombat();
    MemberHoldsWhileTheTankIsNotInCombat();
    MemberEngagesOnceTheTankIsInCombat();
    PullHoldExpiresAfterTheHoldTime();
    DeadTankDoesNotFreezeTheParty();
    PullTankFirstOffKeepsTheOldBehaviour();
    PullHoldLeavesATargetWithinReachAlone();
    PullHoldNeverTouchesASoloBot();
    PullHoldLogsOncePerTenSeconds();
}
