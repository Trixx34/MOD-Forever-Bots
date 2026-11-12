// SPDX-License-Identifier: GPL-2.0-or-later
// Policy tests for the Phase 0 fixes of the 2026-11-10 bot AI report (docs/bot-ai-plan-status.md, bugs #1, #3, #6, #8, #12, #13, #17).
// Bugs #2, #4 and #9 are in rotation_test.cpp and rotation_data_test.cpp. Hand-built snapshots, no core needed.
#include "policy_test_support.h"

namespace {

// Bug #1: below 30 % health the rotation still runs, so a rogue at 25 % uses Evasion (a self_hp_below 40 row).
void ALowHealthBotStillCastsItsDefensiveRow()
{
    // The shipped rogue rows carry the Evasion row (self_hp_below 40); a made-up block of its own and the builder keeps the test small.
    auto const rogue = []()
    {
        std::vector<std::string> warnings;
        std::istringstream in("class 4\nrow 5277 self_hp_below 40 self\nrow 1752 always target range 0 5\n");
        PolicyOptions options = AggressiveOptions();
        options.ClassId = 4;
        options.WanderWhenIdle = false;
        options.Rotations = std::make_shared<rotation::RotationData const>(rotation::ParseRotationData(in, warnings));
        return BotPolicy(options);
    };
    NearbyUnit const mob = Hostile(2, 3.0f, 100);
    CHECK(HasCast(rogue().Decide(Hurt(250, { mob }, mob.guid), 1000), 5277));
    // At 90 % the row does not apply: the builder is cast.
    auto const healthy = rogue().Decide(Hurt(900, { mob }, mob.guid), 1000);
    CHECK(!HasCast(healthy, 5277) && HasCast(healthy, 1752));
}

// Bug #3: a paladin whose roster spec is prot (the tank role) does not heal the party; a holy paladin does.
void ATankPaladinDoesNotHeal()
{
    auto const classOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 10 ? 2u : 5u; };
    auto const run = [&](char const* spec)
    {
        PolicyOptions options = AggressiveOptions();
        options.PartySize = 5;
        options.PartyCount = 1;
        options.ClassOf = classOf;
        options.SpecOf = [spec](std::uint64_t guid) { return guid == 10 ? std::string(spec) : std::string(); };
        BotPolicy policy(options);
        policy.SetRouteActive(true);
        WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 }, classOf);
        view.bots[3].health = 300;   // bot 13 is hurt
        return policy.Decide(view, 0);
    };
    CHECK(CastsSpell(run("holy"), 10, 635, PlayerOf(13)));
    CHECK(!CastsSpell(run("prot"), 10, 635, PlayerOf(13)));
}

// Bug #6: two rogues of one party in front of one casting target: one Kick, not two.
void OnlyOneKickLandsOnACastingTarget()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t) { return 4u; };
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit caster = Hostile(7, 3.0f, 100);
    caster.casting = 1;
    for (std::size_t i = 0; i < 5; ++i)
        AddNearby(view, i, caster);
    int kicks = 0;
    for (BotAction const& action : policy.Decide(view, 0))
        kicks += action.type == ActionType::kCastSpell && action.spell_id == 1766;
    CHECK(kicks == 1);
}

// Bug #8: the pet is sent at a new target when the bot's target changes mid-fight, and not again for the same one.
void ThePetFollowsATargetChange()
{
    BotPolicy policy = ClassPolicy(3);
    NearbyUnit const first = Hostile(2, 20.0f, 100);
    NearbyUnit const second = Hostile(3, 22.0f, 100);
    WorldTickSnapshotView one = HunterView(1, 20, { first }, first.guid);
    one.bots[0].pet_state = 1;
    CHECK(HasAction(policy.Decide(one, 1000), kBot, ActionType::kPetCommand, first.guid));
    CHECK(!HasAction(policy.Decide(one, 3000), kBot, ActionType::kPetCommand));          // same target: not again
    WorldTickSnapshotView two = HunterView(2, 20, { second }, second.guid);
    two.bots[0].pet_state = 1;
    CHECK(HasAction(policy.Decide(two, 5000), kBot, ActionType::kPetCommand, second.guid)); // target changed: sent again
}

// Bug #12: a heal refused for line of sight walks the healer toward the member, not toward the enemy it fights.
void ALineOfSightRefusedHealWalksTowardTheMember()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    auto const classOf = [](std::uint64_t guid) { return guid == 10 ? 5u : 1u; };   // bot 10 is the only healer
    options.ClassOf = classOf;
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    auto const build = [&]()
    {
        WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 }, classOf);
        for (BotSnapshot& bot : view.bots)
            bot.in_combat = 1;
        view.bots[3].health = 300;          // bot 13, hurt, 30 yards north of the healer
        view.bots[3].pos_y = 30.0f;
        NearbyUnit mob = Hostile(7, 12.0f, 100);   // the enemy 12 yards east of the healer
        AddNearby(view, 0, mob);
        view.bots[0].threat_target_guid = mob.guid;
        return view;
    };
    std::vector<BotAction> first = policy.Decide(build(), 0);
    CHECK(CastsSpell(first, 10, 2050, PlayerOf(13)));
    WorldTickSnapshotView next = build();
    next.tick_number = 2;
    Result(next, 2050, BotActionStatus::kNoLineOfSight, 1);
    bool towardMember = false, towardEnemy = false;
    for (BotAction const& action : policy.Decide(next, 1500))
        if (action.bot_guid == 10 && action.type == ActionType::kMoveTo)
        {
            towardMember = towardMember || action.dest_y > 0.0f;
            towardEnemy = towardEnemy || (action.dest_x > 0.0f && action.dest_y <= 0.0f);
        }
    CHECK(towardMember);
    CHECK(!towardEnemy);
}

// Bug #13: crowd control holds an elite pack member that is not in the dungeon's boss list.
void CcHoldsAnEliteHumanoid()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 11 ? 8u : 5u; };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit tankTarget = Hostile(7, 2.0f, 100);
    tankTarget.guid = MakeUnitRef(8, 598, 7);
    NearbyUnit evoker = Hostile(8, 8.0f, 100);
    evoker.guid = MakeUnitRef(8, 1729, 8);   // a Defias Evoker: Humanoid
    evoker.elite = 1;
    view.bots[2].in_combat = 1;
    view.bots[2].threat_target_guid = tankTarget.guid;
    AddNearby(view, 1, tankTarget);
    AddNearby(view, 1, evoker);
    bool held = false;
    for (std::uint64_t at = 0; at < 6000; at += 500)
        for (BotAction const& action : policy.Decide(view, at))
            held = held || (action.bot_guid == 11 && action.type == ActionType::kCastSpell && action.spell_id == 118 && action.target_guid == evoker.guid);
    CHECK(held);
}

// Bug #17: a route-active healer with no module party (no leader entry in the tick's map) heals itself and does not read past the map's end.
void ARouteHealerWithoutAPartyHealsItself()
{
    BotPolicy policy = ClassPolicy(5);
    policy.SetRouteActive(true);
    NearbyUnit const mob = Hostile(2, 12.0f, 100);
    WorldTickSnapshotView view = Hurt(300, { mob }, mob.guid);
    view.bots[0].power = 5000;
    view.bots[0].max_power = 5000;
    view.bots[0].in_combat = 1;
    auto const actions = policy.Decide(view, 1000);
    CHECK(HasCast(actions, 2050) || HasCast(actions, 2054) || HasCast(actions, 17));   // a heal or a shield, on itself
}

// Plan item 4 (interrupt completeness): the warrior's Shield Bash (72) and Pummel (6552) come from the shipped rotation file, each only in its stance.
BotPolicy WarriorWithTheShippedRows(char const* spec)
{
    std::vector<std::string> warnings;
    PolicyOptions options = AggressiveOptions();
    options.ClassId = 1;
    options.WanderWhenIdle = false;
    options.Rotations = std::make_shared<rotation::RotationData const>(rotation::LoadRotationData(std::string(FOREVER_BOTS_SOURCE_DIR) + "/data/rotations.txt", warnings));
    options.SpecOf = [spec](std::uint64_t) { return std::string(spec); };
    return BotPolicy(options);
}

WorldTickSnapshotView CasterView(std::uint32_t stanceAura, std::uint32_t remainingMs)
{
    NearbyUnit caster = Hostile(2, 3.0f, 100);
    caster.casting = 1;
    caster.cast_remaining_ms = remainingMs;
    WorldTickSnapshotView view = View(1, caster.guid, { caster });
    view.bots[0].level = 60;
    view.bots[0].power_type = 1;
    view.bots[0].power = 50;
    view.bots[0].max_power = 100;
    if (stanceAura != 0)
    {
        view.bots[0].auras[0] = { stanceAura, 0xFFFFFFFFu, 1 };
        view.bots[0].aura_count = 1;
    }
    return view;
}

void AWarriorInterruptsOnlyInTheStanceThatAllowsIt()
{
    CHECK(HasCast(WarriorWithTheShippedRows("prot").Decide(CasterView(71, 1500), 1000), 72));      // Defensive Stance: Shield Bash
    CHECK(!HasCast(WarriorWithTheShippedRows("prot").Decide(CasterView(0, 1500), 1000), 72));      // no stance aura: no kick
    CHECK(HasCast(WarriorWithTheShippedRows("arms").Decide(CasterView(2458, 1500), 1000), 6552));  // Berserker Stance: Pummel
    CHECK(!HasCast(WarriorWithTheShippedRows("arms").Decide(CasterView(2457, 1500), 1000), 6552)); // Battle Stance: none
    CHECK(!HasCast(WarriorWithTheShippedRows("arms").Decide(CasterView(2457, 1500), 1000), 72));
}

// The kick waits out nothing: a cast with under kInterruptMinRemainingMs left is left alone, and 0 (not reported) never blocks it.
void AnInterruptIsSkippedWhenTheCastIsAboutToEnd()
{
    CHECK(!HasCast(WarriorWithTheShippedRows("prot").Decide(CasterView(71, 200), 1000), 72));
    CHECK(HasCast(WarriorWithTheShippedRows("prot").Decide(CasterView(71, 300), 1000), 72));
    CHECK(HasCast(WarriorWithTheShippedRows("prot").Decide(CasterView(71, 0), 1000), 72));
    // The rogue's built-in Kick follows the same gate.
    NearbyUnit caster = Hostile(2, 3.0f, 100);
    caster.casting = 1;
    caster.cast_remaining_ms = 150;
    CHECK(!HasCast(ClassPolicy(4).Decide(View(1, 0, { caster }), 1000), 1766));
    caster.cast_remaining_ms = 900;
    CHECK(HasCast(ClassPolicy(4).Decide(View(1, 0, { caster }), 1000), 1766));
}

} // namespace

int main()
{
    ALowHealthBotStillCastsItsDefensiveRow();
    ATankPaladinDoesNotHeal();
    OnlyOneKickLandsOnACastingTarget();
    ThePetFollowsATargetChange();
    ALineOfSightRefusedHealWalksTowardTheMember();
    CcHoldsAnEliteHumanoid();
    ARouteHealerWithoutAPartyHealsItself();
    AWarriorInterruptsOnlyInTheStanceThatAllowsIt();
    AnInterruptIsSkippedWhenTheCastIsAboutToEnd();
    if (failures == 0)
        std::printf("phase0_fixes_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
