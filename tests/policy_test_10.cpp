// SPDX-License-Identifier: GPL-2.0-or-later
// Policy tests, part 10: the raid rule rows spread, avoid_enemy, purge and mindcontrol (raid_rules.h; the helpers are in policy_test_support.h).
#include "policy_test_support.h"
#include "bot_policy_internal.h"

#include <cmath>

namespace {

constexpr std::uint32_t kVenoxis = 14507;
constexpr std::uint64_t kBossRef = MakeUnitRef(8, kVenoxis, 50);

// Bots 10..14 in one party (12 the warrior leader and tank), all in combat on map 309 with the boss in view; positions as given (bot order 10..14).
struct RaidFixture
{
    std::function<std::uint32_t(std::uint64_t)> classes;
    std::vector<std::pair<float, float>> at;
    bool bossCasting = false;
    float bossX = 0.0f;
};

WorldTickSnapshotView RaidView(RaidFixture const& f, std::uint32_t map = 309)
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 }, f.classes);
    for (std::size_t i = 0; i < view.bots.size(); ++i)
    {
        BotSnapshot& bot = view.bots[i];
        bot.map_id = map;
        bot.pos_x = f.at[i].first;
        bot.pos_y = f.at[i].second;
        bot.in_combat = 1;
        bot.threat_target_guid = kBossRef;
        NearbyUnit boss = Hostile(50, f.bossX, 100);
        boss.guid = kBossRef;
        boss.pos_y = 0.0f;
        boss.casting = f.bossCasting ? 1 : 0;
        AddNearby(view, i, boss);
    }
    return view;
}

PolicyOptions RaidOptions(std::function<std::uint32_t(std::uint64_t)> classes, char const* rules)
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.ClassOf = classes;
    std::vector<std::string> errors;
    options.RaidRules = std::make_shared<raid_rules::Table>(raid_rules::Parse(rules, errors));
    CHECK(errors.empty());
    return options;
}

// 12 warrior, 10 and 11 and 13 mages, 14 priest.
std::uint32_t Classes(std::uint64_t guid)
{
    switch (guid)
    {
        case 12: return 1;
        case 14: return 5;
        default: return 8;
    }
}

std::vector<BotAction> MovesOf(std::vector<BotAction> const& actions, std::uint64_t bot)
{
    std::vector<BotAction> out;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.bot_guid == bot)
            out.push_back(action);
    return out;
}

float FromPoint(BotAction const& move, float x, float y)
{
    return std::hypot(move.dest_x - x, move.dest_y - y);
}

// ---- spread ----
char const* const kSpreadRule = "spread|309|14507|10|ranged|Holy Wrath\n";

RaidFixture Clumped()
{
    return RaidFixture{ Classes, { { 0, 0 }, { 3, 0 }, { 0, 18 }, { 40, 0 }, { -40, 0 } }, false, 0.0f };   // mages 10 and 11 three yards apart; the boss at (0, 0) is just a unit in view
}

void SpreadMovesTheHigherGuidAwayFromItsNeighbour()
{
    BotPolicy policy(RaidOptions(Classes, kSpreadRule));
    std::vector<BotAction> const actions = policy.Decide(RaidView(Clumped()), 1000);
    auto const moves = MovesOf(actions, 11);
    CHECK(moves.size() == 1);
    if (moves.size() == 1)
        CHECK(FromPoint(moves[0], 0, 0) >= 10.0f - 0.01f);
    CHECK(MovesOf(actions, 10).empty());                      // the lower guid stays
    CHECK(MovesOf(actions, 12).empty());                      // the tank never moves for a spread row
    // the end point is kept while the bot walks, and no second nudge comes inside the cooldown
    WorldTickSnapshotView view = RaidView(Clumped());
    view.bots[1].pos_x = 6.0f;
    CHECK(MovesOf(policy.Decide(view, 1500), 11).empty());
}

void SpreadNeedsARowTheSwitchAndAnEngagedBoss()
{
    PolicyOptions off = RaidOptions(Classes, kSpreadRule);
    off.RaidSpread = false;
    BotPolicy switchedOff(off);
    CHECK(MovesOf(switchedOff.Decide(RaidView(Clumped()), 1000), 11).empty());
    BotPolicy noRow(RaidOptions(Classes, "avoid|309|spell|19628|5|bomb\n"));
    CHECK(MovesOf(noRow.Decide(RaidView(Clumped()), 1000), 11).empty());
    BotPolicy otherMap(RaidOptions(Classes, kSpreadRule));
    CHECK(MovesOf(otherMap.Decide(RaidView(Clumped(), 249), 1000), 11).empty());
    BotPolicy otherBoss(RaidOptions(Classes, "spread|309|14510|10|ranged|someone else\n"));
    CHECK(MovesOf(otherBoss.Decide(RaidView(Clumped()), 1000), 11).empty());
    BotPolicy healersOnly(RaidOptions(Classes, "spread|309|14507|10|healers|x\n"));
    CHECK(MovesOf(healersOnly.Decide(RaidView(Clumped()), 1000), 11).empty());    // the mages are not healers
    BotPolicy calm(RaidOptions(Classes, kSpreadRule));
    WorldTickSnapshotView quiet = RaidView(Clumped());
    for (BotSnapshot& bot : quiet.bots)
        bot.threat_target_guid = 0;                                                // nobody engaged the boss
    CHECK(MovesOf(calm.Decide(quiet, 1000), 11).empty());
}

void SpreadDoesNotInterruptACast()
{
    BotPolicy policy(RaidOptions(Classes, kSpreadRule));
    WorldTickSnapshotView view = RaidView(Clumped());
    view.bots[1].casting_spell = 133;
    view.bots[1].cast_remaining_ms = 1500;
    CHECK(MovesOf(policy.Decide(view, 1000), 11).empty());
}

// ---- avoid_enemy ----
void AvoidEnemyMovesARangedMemberOutOfTheCircle()
{
    RaidFixture f{ Classes, { { 40, 0 }, { 5, 0 }, { 1, 0 }, { -40, 0 }, { 0, 40 } }, false, 0.0f };     // mage 11 five yards from the boss, the warrior one yard
    BotPolicy policy(RaidOptions(Classes, "avoid_enemy|309|14507|15|ranged|Holy Nova\n"));
    std::vector<BotAction> const actions = policy.Decide(RaidView(f), 1000);
    auto const moves = MovesOf(actions, 11);
    CHECK(moves.size() == 1);
    if (moves.size() == 1)
        CHECK(FromPoint(moves[0], 0, 0) >= 15.0f - 0.01f);
    CHECK(MovesOf(actions, 12).empty());                      // the tank stays
    CHECK(MovesOf(actions, 10).empty());                      // already outside
    // kept: no new move on the next tick, and a bot that has arrived is left alone
    WorldTickSnapshotView view = RaidView(f);
    view.bots[1].pos_x = moves.empty() ? 5.0f : moves[0].dest_x;
    view.bots[1].pos_y = moves.empty() ? 0.0f : moves[0].dest_y;
    CHECK(MovesOf(policy.Decide(view, 1100), 11).empty());
}

void AvoidEnemyHonoursGroupSwitchAndCasting()
{
    RaidFixture f{ Classes, { { 40, 0 }, { 5, 0 }, { 1, 0 }, { -40, 0 }, { 0, 40 } }, false, 0.0f };
    BotPolicy healersOnly(RaidOptions(Classes, "avoid_enemy|309|14507|15|healers|Holy Nova\n"));
    CHECK(MovesOf(healersOnly.Decide(RaidView(f), 1000), 11).empty());           // a mage is ranged, not a healer
    PolicyOptions off = RaidOptions(Classes, "avoid_enemy|309|14507|15|all|Holy Nova\n");
    off.RaidAvoidEnemy = false;
    BotPolicy switchedOff(off);
    CHECK(MovesOf(switchedOff.Decide(RaidView(f), 1000), 11).empty());
    BotPolicy other(RaidOptions(Classes, "avoid_enemy|309|14510|15|all|someone else\n"));
    CHECK(MovesOf(other.Decide(RaidView(f), 1000), 11).empty());
    // mid-cast: holds, unless the unit is casting
    WorldTickSnapshotView casting = RaidView(f);
    casting.bots[1].casting_spell = 133;
    casting.bots[1].cast_remaining_ms = 1500;
    BotPolicy holds(RaidOptions(Classes, "avoid_enemy|309|14507|15|ranged|Holy Nova\n"));
    CHECK(MovesOf(holds.Decide(casting, 1000), 11).empty());
    RaidFixture g = f;
    g.bossCasting = true;
    WorldTickSnapshotView bossCasting = RaidView(g);
    bossCasting.bots[1].casting_spell = 133;
    bossCasting.bots[1].cast_remaining_ms = 1500;
    BotPolicy moves(RaidOptions(Classes, "avoid_enemy|309|14507|15|ranged|Holy Nova\n"));
    CHECK(MovesOf(moves.Decide(bossCasting, 1000), 11).size() == 1);
}

void AvoidEnemyMeleeStaysWhenOnlyRangedAreListed()
{
    auto const classes = [](std::uint64_t guid) -> std::uint32_t { return guid == 12 ? 1u : guid == 11 ? 4u : 8u; };   // 11 is a rogue
    RaidFixture f{ classes, { { 40, 0 }, { 5, 0 }, { 1, 0 }, { -40, 0 }, { 0, 40 } }, false, 0.0f };
    BotPolicy policy(RaidOptions(classes, "avoid_enemy|309|14507|15|ranged|Holy Nova\n"));
    CHECK(MovesOf(policy.Decide(RaidView(f), 1000), 11).empty());
    BotPolicy melee(RaidOptions(classes, "avoid_enemy|309|14507|15|melee|Holy Nova\n"));
    CHECK(MovesOf(melee.Decide(RaidView(f), 1000), 11).size() == 1);
}

// ---- purge ----
void PurgeCastsDispelMagicOnTheBuffedBoss()
{
    RaidFixture f{ Classes, { { 40, 0 }, { 20, 0 }, { 1, 0 }, { -40, 0 }, { 10, 0 } }, false, 0.0f };    // the priest (14) ten yards from the boss
    WorldTickSnapshotView view = RaidView(f);
    view.bots[1].target_aura_count = 1;                       // a mage sees Renew on its target (layout 24 target_auras)
    view.bots[1].target_auras[0].spell_id = 23895;
    view.bots[1].target_auras[0].remaining_ms = 15000;
    BotPolicy policy(RaidOptions(Classes, "purge|309|23895|Venoxis Renew\n"));
    std::vector<BotAction> const actions = policy.Decide(view, 1000);
    CHECK(CastsSpell(actions, 14, 527, kBossRef));
    // the same job is not sent again inside the retry window
    CHECK(!CastsSpell(policy.Decide(view, 1500), 14, 527, kBossRef));
    // no listed aura: no cast
    WorldTickSnapshotView other = RaidView(f);
    other.bots[1].target_aura_count = 1;
    other.bots[1].target_auras[0].spell_id = 99999;
    BotPolicy none(RaidOptions(Classes, "purge|309|23895|Venoxis Renew\n"));
    CHECK(!CastsSpell(none.Decide(other, 1000), 14, 527));
    PolicyOptions off = RaidOptions(Classes, "purge|309|23895|Venoxis Renew\n");
    off.RaidPurge = false;
    BotPolicy switchedOff(off);
    CHECK(!CastsSpell(switchedOff.Decide(view, 1000), 14, 527));
    BotPolicy noRow(RaidOptions(Classes, "dispel|309|16098|curse|x\n"));
    CHECK(!CastsSpell(noRow.Decide(view, 1000), 14, 527));
    BotPolicy elsewhere(RaidOptions(Classes, "purge|309|23895|Venoxis Renew\n"));
    WorldTickSnapshotView otherMap = view;
    for (BotSnapshot& bot : otherMap.bots)
        bot.map_id = 249;
    CHECK(!CastsSpell(elsewhere.Decide(otherMap, 1000), 14, 527));
}

void PurgeShamanUsesPurgeAndNoOtherClassPurges()
{
    auto const classes = [](std::uint64_t guid) -> std::uint32_t { return guid == 12 ? 1u : guid == 14 ? 7u : 8u; };   // 14 a shaman
    RaidFixture f{ classes, { { 40, 0 }, { 20, 0 }, { 1, 0 }, { -40, 0 }, { 10, 0 } }, false, 0.0f };
    WorldTickSnapshotView view = RaidView(f);
    view.bots[1].target_aura_count = 1;
    view.bots[1].target_auras[0].spell_id = 23895;
    BotPolicy policy(RaidOptions(classes, "purge|309|23895|Venoxis Renew\n"));
    std::vector<BotAction> const actions = policy.Decide(view, 1000);
    CHECK(CastsSpell(actions, 14, 370, kBossRef));
    for (std::uint64_t mage : { 10, 11, 13 })
        CHECK(!CastsSpell(actions, mage, 370) && !CastsSpell(actions, mage, 527));
}

// ---- mind control ----
void MindControlMageHoldsTheControlledMember()
{
    RaidFixture f{ Classes, { { 0, 0 }, { 10, 0 }, { 1, 0 }, { 80, 0 }, { 5, 0 } }, false, 0.0f };      // bot 10 (a mage, controlled), mage 11 ten yards from it
    WorldTickSnapshotView view = RaidView(f);
    view.bots[0].aura_count = 1;
    view.bots[0].auras[0].spell_id = 24327;
    view.bots[0].auras[0].remaining_ms = 10000;
    BotPolicy policy(RaidOptions(Classes, "mindcontrol|309|24327|Cause Insanity\n"));
    std::vector<BotAction> const actions = policy.Decide(view, 1000);
    CHECK(CastsSpell(actions, 11, 118, PlayerOf(10)));            // the nearest mage polymorphs it
    CHECK(!CastsSpell(actions, 10, 118));                         // not the controlled one itself
    CHECK(!CastsSpell(actions, 13, 118));                         // out of reach, and only the backup after a delay
    CHECK(!CastsSpell(policy.Decide(view, 1500), 11, 118, PlayerOf(10)));   // inside the retry window
    CHECK(CastsSpell(policy.Decide(view, 8000), 11, 118, PlayerOf(10)));    // and again once it has run out
    PolicyOptions off = RaidOptions(Classes, "mindcontrol|309|24327|Cause Insanity\n");
    off.RaidMindControl = false;
    BotPolicy switchedOff(off);
    CHECK(!CastsSpell(switchedOff.Decide(view, 1000), 11, 118));
    BotPolicy noRow(RaidOptions(Classes, "purge|309|23895|x\n"));
    CHECK(!CastsSpell(noRow.Decide(view, 1000), 11, 118));
}

void MindControlBackupWaitsAndNoRogueCasts()
{
    auto const classes = [](std::uint64_t guid) -> std::uint32_t { return guid == 12 ? 1u : guid == 13 ? 4u : 8u; };   // 13 a rogue
    RaidFixture f{ classes, { { 0, 0 }, { 10, 0 }, { 1, 0 }, { 6, 0 }, { 20, 0 } }, false, 0.0f };
    WorldTickSnapshotView view = RaidView(f);
    view.bots[0].aura_count = 1;
    view.bots[0].auras[0].spell_id = 24327;
    BotPolicy policy(RaidOptions(classes, "mindcontrol|309|24327|Cause Insanity\n"));
    std::vector<BotAction> const first = policy.Decide(view, 1000);
    CHECK(CastsSpell(first, 11, 118, PlayerOf(10)));              // rank 0: the nearer mage among the mages (11 at 10 yards, 14 at 20)
    CHECK(!CastsSpell(first, 14, 118));                           // the backup waits for the delay
    CHECK(!CastsSpell(first, 13, 6770));                          // a rogue's Sap needs a target out of combat: not used
}

} // namespace

void RunPolicyTestsPart10()
{
    SpreadMovesTheHigherGuidAwayFromItsNeighbour();
    SpreadNeedsARowTheSwitchAndAnEngagedBoss();
    SpreadDoesNotInterruptACast();
    AvoidEnemyMovesARangedMemberOutOfTheCircle();
    AvoidEnemyHonoursGroupSwitchAndCasting();
    AvoidEnemyMeleeStaysWhenOnlyRangedAreListed();
    PurgeCastsDispelMagicOnTheBuffedBoss();
    PurgeShamanUsesPurgeAndNoOtherClassPurges();
    MindControlMageHoldsTheControlledMember();
    MindControlBackupWaitsAndNoRogueCasts();
}
