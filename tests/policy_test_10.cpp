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
    BotPolicy meleeKeyOff(RaidOptions(classes, "avoid_enemy|309|14507|15|melee|Holy Nova\n"));
    CHECK(MovesOf(meleeKeyOff.Decide(RaidView(f), 1000), 11).empty());          // melee rows act only with RaidAvoidEnemyMelee (default off)
    PolicyOptions on = RaidOptions(classes, "avoid_enemy|309|14507|15|melee|Holy Nova\n");
    on.RaidAvoidEnemyMelee = true;
    BotPolicy melee(on);
    std::vector<BotAction> const actions = melee.Decide(RaidView(f), 1000);
    CHECK(MovesOf(actions, 11).size() == 1);
    CHECK(MovesOf(actions, 12).empty());                                        // the tank is not melee
}

// RaidAvoidEnemyTank (ZG lever): a `tanks` row moves the tank (warrior 12, one yard from the unit) out of the circle, only with the key; `all` never includes the tank.
void AvoidEnemyTanksRowMovesTheTankOnlyWithTheKey()
{
    RaidFixture f{ Classes, { { 40, 0 }, { 40, 5 }, { 1, 0 }, { -40, 0 }, { 0, 40 } }, false, 0.0f };
    BotPolicy keyOff(RaidOptions(Classes, "avoid_enemy|309|14507|15|tanks|x\n"));
    CHECK(MovesOf(keyOff.Decide(RaidView(f), 1000), 12).empty());
    PolicyOptions on = RaidOptions(Classes, "avoid_enemy|309|14507|15|tanks|x\n");
    on.RaidAvoidEnemyTank = true;
    BotPolicy tank(on);
    auto const moves = MovesOf(tank.Decide(RaidView(f), 1000), 12);
    CHECK(moves.size() == 1);
    if (moves.size() == 1)
        CHECK(FromPoint(moves[0], 0, 0) >= 15.0f - 0.01f);
    PolicyOptions all = RaidOptions(Classes, "avoid_enemy|309|14507|15|all|x\n");
    all.RaidAvoidEnemyTank = true;
    BotPolicy allRow(all);
    CHECK(MovesOf(allRow.Decide(RaidView(f), 1000), 12).empty());
}

// RaidFocusAvoidEnemy (ZG lever): a ranged damage dealer turns from its target (the boss) to a fighting hostile of an avoid_enemy entry in reach; one that fights
// nobody, the key off, or a healer, does not. The healers' row keeps the mage's own circle out of the picture.
constexpr std::uint64_t kThrowerRef = MakeUnitRef(8, 11350, 60);

bool ActsOn(std::vector<BotAction> const& actions, std::uint64_t bot, std::uint64_t target)
{
    for (BotAction const& action : actions)
        if (action.bot_guid == bot && action.target_guid == target)
            return true;
    return false;
}

WorldTickSnapshotView ThrowerView(bool fighting, std::uint64_t mageTarget = kBossRef)
{
    RaidFixture f{ Classes, { { 40, 0 }, { 5, 0 }, { 1, 0 }, { -40, 0 }, { 0, 40 } }, false, 0.0f };
    WorldTickSnapshotView view = RaidView(f);
    for (std::size_t i = 0; i < view.bots.size(); ++i)
    {
        NearbyUnit thrower = Hostile(60, 25.0f, 100);
        thrower.guid = kThrowerRef;
        thrower.pos_y = 0.0f;
        thrower.victim_ref = fighting ? PlayerOf(14) : 0;
        AddNearby(view, i, thrower);
    }
    view.bots[1].threat_target_guid = mageTarget;
    return view;
}

void RangedFocusFireOnAnAvoidEnemyHostile()
{
    auto const turns = [](bool key, bool fighting, std::uint64_t bot, std::vector<std::string>* events)
    {
        PolicyOptions options = RaidOptions(Classes, "avoid_enemy|309|11350|33|healers|Axe Flurry\n");
        options.RaidFocusAvoidEnemy = key;
        if (events)
            options.OnEvent = [events](std::string const& line) { events->push_back(line); };
        BotPolicy policy(options);
        policy.SetRouteActive(true);
        return ActsOn(policy.Decide(ThrowerView(fighting), 1000), bot, kThrowerRef);
    };
    std::vector<std::string> events;
    CHECK(turns(true, true, 11, &events));
    CHECK(Logged(events, "focus_avoid_enemy: bot=11 entry=11350"));
    CHECK(!turns(false, true, 11, nullptr));                                    // key off: the mage stays on its target
    CHECK(!turns(true, false, 11, nullptr));                                    // the thrower fights nobody yet: not pulled by the focus
    CHECK(!turns(true, true, 14, nullptr));                                     // the priest is a healer
}

// RaidFocusAvoidEnemy: the mage that fights the thrower ignores that unit's circle (a ranged row), the other mage (13, also inside, on the boss) still steps out.
void RangedFocusIgnoresTheCircleOfItsOwnTarget()
{
    auto const moves = [](bool key, std::uint64_t bot)
    {
        PolicyOptions options = RaidOptions(Classes, "avoid_enemy|309|11350|33|ranged|Axe Flurry\n");
        options.RaidFocusAvoidEnemy = key;
        BotPolicy policy(options);
        policy.SetRouteActive(true);
        WorldTickSnapshotView view = ThrowerView(true, kThrowerRef);
        view.bots[3].pos_x = 10.0f;                                             // mage 13 fifteen yards from the thrower
        return !MovesOf(policy.Decide(view, 1000), bot).empty();
    };
    CHECK(!moves(true, 11));
    CHECK(moves(true, 13));
    CHECK(moves(false, 11));
}

// ---- avoid_enemy_aura ----
void AvoidEnemyAuraHoldsOnlyWhileTheAuraIsOnOrUnknown()
{
    RaidFixture f{ Classes, { { 40, 0 }, { 5, 0 }, { 1, 0 }, { -40, 0 }, { 0, 40 } }, false, 0.0f };     // mage 11 five yards from the unit; every bot targets it
    char const* rule = "avoid_enemy_aura|309|14507|15|ranged|24018|Axe Flurry\n";
    // the bots' target lists show the aura: the mage steps out
    WorldTickSnapshotView seen = RaidView(f);
    seen.bots[2].target_aura_count = 1;
    seen.bots[2].target_auras[0].spell_id = 24018;
    BotPolicy withAura(RaidOptions(Classes, rule));
    CHECK(MovesOf(withAura.Decide(seen, 1000), 11).size() == 1);
    // complete lists without the aura: no circle
    WorldTickSnapshotView absent = RaidView(f);
    absent.bots[2].target_aura_count = 1;
    absent.bots[2].target_auras[0].spell_id = 772;
    BotPolicy noAura(RaidOptions(Classes, rule));
    CHECK(MovesOf(noAura.Decide(absent, 1000), 11).empty());
    // one list without it, one with it: the aura wins
    WorldTickSnapshotView mixed = absent;
    mixed.bots[4].target_aura_count = 1;
    mixed.bots[4].target_auras[0].spell_id = 24018;
    BotPolicy both(RaidOptions(Classes, rule));
    CHECK(MovesOf(both.Decide(mixed, 1000), 11).size() == 1);
    // nobody targets the unit: unknown keeps the circle
    WorldTickSnapshotView unknown = RaidView(f);
    for (BotSnapshot& bot : unknown.bots)
        bot.threat_target_guid = 0;
    BotPolicy noWatcher(RaidOptions(Classes, rule));
    CHECK(MovesOf(noWatcher.Decide(unknown, 1000), 11).size() == 1);
    // only full lists (8 auras, none of them the row's): unknown, the circle stays
    WorldTickSnapshotView full = RaidView(f);
    for (BotSnapshot& bot : full.bots)
    {
        bot.target_aura_count = forever_module::kMaxTargetAuras;
        for (std::size_t a = 0; a < forever_module::kMaxTargetAuras; ++a)
            bot.target_auras[a].spell_id = 1000 + std::uint32_t(a);
    }
    BotPolicy fullLists(RaidOptions(Classes, rule));
    CHECK(MovesOf(fullLists.Decide(full, 1000), 11).size() == 1);
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

// ---- dispel rows in a 5-man dungeon (Ruins of Lordaeron, doc/rol-hot-route-plan.md) ----
// Cadaver Worms (16143, a disease) on the leader after the fight: the party's priest casts Cure Disease (528) on it, out of combat, on the dungeon map only.
void DungeonPartyCuresCadaverWormsOutOfCombat()
{
    char const* const rule = "dispel|2999|16143|disease|Cadaver Worms\n";
    RaidFixture f{ Classes, { { 4, 0 }, { 6, 0 }, { 0, 0 }, { 8, 0 }, { 10, 0 } }, false, 200.0f };
    auto const quiet = [&f](std::uint32_t map) {
        WorldTickSnapshotView view = RaidView(f, map);
        for (BotSnapshot& bot : view.bots)
        {
            bot.in_combat = 0;
            bot.threat_target_guid = 0;
            bot.level = 20;
        }
        view.bots[2].aura_count = 1;                      // the warrior leader (12) carries the disease
        view.bots[2].auras[0].spell_id = 16143;
        return view;
    };
    BotPolicy policy(RaidOptions(Classes, rule));
    CHECK(CastsSpell(policy.Decide(quiet(2999), 1000), 14, 528));
    // the row is for map 2999: elsewhere nobody casts it
    BotPolicy elsewhere(RaidOptions(Classes, rule));
    CHECK(!CastsSpell(elsewhere.Decide(quiet(309), 1000), 14, 528));
    // no row: no cast (the behaviour before the row)
    BotPolicy noRow(RaidOptions(Classes, "dispel|309|16098|curse|x\n"));
    CHECK(!CastsSpell(noRow.Decide(quiet(2999), 1000), 14, 528));
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

// Urgent repair (repair_policy.h). A party member never walks to a vendor: its leader takes the party to the repairman for the member's broken gear,
// waits there, the member walks the last yards and repairs, and then the leader makes its own visit.
inline PolicyOptions RepairPartyOptions()
{
    PolicyOptions options = AggressiveOptions();
    options.ClassOf = WarriorLeads;
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ServiceEveryMs = 0; // no routine visits: only the urgent rule moves them
    options.WanderWhenIdle = false;
    return options;
}

inline void SetRepairman(WorldTickSnapshotView& view, float x)
{
    for (BotSnapshot& bot : view.bots)
    {
        bot.repair_valid = 1;
        bot.repair_x = x;
        bot.repair_guid = 0xF130000000000077ull;
        bot.copper = 100000;
        bot.level = 10;
        bot.bag_free_slots = 10; // room in the bags: only the money decides the junk sale
    }
}

void LeaderTakesThePartyToTheRepairmanForAMember()
{
    BotPolicy policy(RepairPartyOptions());
    WorldTickSnapshotView view = PartyView({ 11, 12 }); // 12 the warrior leads, 11 follows
    SetRepairman(view, -200.0f);
    view.bots[0].durability_pct = 0; // the member's gear is broken, the leader's is fine
    std::vector<BotAction> const walk = AfterMount(policy, view, 0);
    bool leaderWalks = false;
    for (BotAction const& action : walk)
        leaderWalks = leaderWalks || (action.type == ActionType::kMoveTo && action.bot_guid == 12 && action.dest_x == -200.0f);
    CHECK(leaderWalks);
    CHECK(!HasAction(walk, 11, ActionType::kVendor)); // the member is 200 yards away: it follows
}

void MemberRepairsWhileTheLeaderWaits()
{
    BotPolicy policy(RepairPartyOptions());
    WorldTickSnapshotView view = PartyView({ 11, 12 });
    SetRepairman(view, -200.0f);
    view.bots[0].durability_pct = 0;
    view.bots[0].pos_x = -190.0f; // the member followed to 10 yards
    view.bots[1].pos_x = -198.0f; // the leader is in reach
    std::vector<BotAction> const first = policy.Decide(view, 0);
    bool memberWalks = false;
    for (BotAction const& action : first)
        memberWalks = memberWalks || (action.type == ActionType::kMoveTo && action.bot_guid == 11 && action.dest_x == -200.0f);
    CHECK(memberWalks);
    CHECK(!HasAction(first, 12, ActionType::kVendor)); // the leader waits for the member
    view.bots[0].pos_x = -199.0f;
    std::vector<BotAction> const second = policy.Decide(view, 1000);
    BotAction const* memberVisit = nullptr;
    for (BotAction const& action : second)
        if (action.type == ActionType::kVendor && action.bot_guid == 11)
            memberVisit = &action;
    CHECK(memberVisit && (memberVisit->spell_id & 2) != 0 && (memberVisit->spell_id & 1024) == 0); // repairs; has money, so no junk sale
    CHECK(HasAction(second, 12, ActionType::kVendor)); // the member has been: the leader makes its visit too
    // Repaired: no new trip.
    view.bots[0].durability_pct = 100;
    policy.Decide(view, 7000);
    for (std::uint64_t at = 8000; at < 60000; at += 5000)
    {
        std::vector<BotAction> const later = policy.Decide(view, at);
        CHECK(!HasAction(later, 11, ActionType::kVendor) && !HasAction(later, 12, ActionType::kVendor));
    }
}

// A lone bot with a broken piece and no money: one visit (selling junk), judged, then it waits; the leader-less loop the prod bots would have made is not there.
void BrokenBotWithoutMoneyDoesNotLoop()
{
    PolicyOptions options = RepairPartyOptions();
    options.PartySize = 0;
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10 });
    SetRepairman(view, -2.0f); // in reach
    view.bots[0].durability_pct = 0;
    view.bots[0].copper = 0;
    int visits = 0;
    for (std::uint64_t at = 0; at < 3600000; at += 2000)
        for (BotAction const& action : policy.Decide(view, at))
            if (action.type == ActionType::kVendor)
            {
                ++visits;
                CHECK((action.spell_id & 1024) != 0); // no money: the white junk is sold first
            }
    // 300 s, 600, 1200, 2400 after the first: five visits in the hour, not one every tick.
    CHECK(visits >= 3 && visits <= 5);
}

// 2026-11-14: an auction errand ended the moment the bot came in reach, and the bot walked on before the listing visit (its own 10 s timer) saw it
// there. It stands kAuctionDwellMs (25 s) in reach now, then the errand is done.
void AuctionErrandStandsAtTheAuctioneerBeforeItEnds()
{
    BotPolicy policy{ PolicyOptions{} };
    ErrandOrder order;
    order.Map = 0;
    order.Dest = Point{ 2.0f, 0.0f, 0.0f };
    order.Auction = true;
    WorldTickSnapshotView const view = View(1, 0, {});
    policy.Decide(view, 500);                                      // the bot is known to the policy
    policy.StartErrand(kBot, order);
    auto const movesOf = [](std::vector<BotAction> const& actions)
    {
        std::size_t moves = 0;
        for (BotAction const& action : actions)
            moves += action.bot_guid == kBot && action.type == ActionType::kMoveTo;
        return moves;
    };
    CHECK(movesOf(policy.Decide(view, 1000)) == 0);                // in reach (2 yards): it stands
    CHECK(policy.TakeErrandOutcome(kBot) == ErrandOutcome::kNone);
    CHECK(movesOf(policy.Decide(view, 20000)) == 0);
    CHECK(policy.TakeErrandOutcome(kBot) == ErrandOutcome::kNone); // 19 s in reach: still there for the listing visit
    policy.Decide(view, 26000);
    CHECK(policy.TakeErrandOutcome(kBot) == ErrandOutcome::kDone); // 25 s: done
}

// ---- RaidBossPositions: tank_spot and flank rows (Onyxia fix 1) ----
// Onyxia at the origin of map 249, facing north (+x); the tank spot 4 yards north of her. Bots 10..14 (12 the warrior tank, 14 a priest, the rest mages) at the given points.
constexpr std::uint64_t kOnyRef = MakeUnitRef(8, 10184, 60);
char const* const kOnyPositionRows = "tank_spot|249|10184|4|0|0|3|north\nflank|249|10184|120|120|30|cones\n";

struct OnyFixture
{
    std::vector<std::pair<float, float>> at;
    std::uint64_t victim = 0;     // the bot she attacks (0 = nobody known)
    float bossZ = 0.0f;
    std::uint32_t healthPct = 100;
};

WorldTickSnapshotView OnyView(OnyFixture const& f, std::uint32_t map = 249)
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 }, Classes);
    for (std::size_t i = 0; i < view.bots.size(); ++i)
    {
        BotSnapshot& bot = view.bots[i];
        bot.map_id = map;
        bot.pos_x = f.at[i].first;
        bot.pos_y = f.at[i].second;
        bot.in_combat = 1;
        bot.threat_target_guid = kOnyRef;
        NearbyUnit boss = Hostile(60, 0.0f, f.healthPct);
        boss.guid = kOnyRef;
        boss.pos_z = f.bossZ;
        boss.orientation = 0.0f;
        boss.victim_ref = f.victim != 0 ? PlayerRef(f.victim) : 0;
        AddNearby(view, i, boss);
    }
    return view;
}

PolicyOptions OnyOptions(bool key, char const* rules = kOnyPositionRows)
{
    PolicyOptions options = RaidOptions(Classes, rules);
    options.WanderWhenIdle = false;
    options.RaidBossPositions = key;
    return options;
}

// The kMoveTo destinations of every bot over 1..6 s (past the damage dealers' 3 s pull hold), from one run of a fresh policy (a repeated order is not sent again,
// so each run starts with its own policy).
std::map<std::uint64_t, std::vector<std::pair<float, float>>> OnyRun(PolicyOptions const& options, WorldTickSnapshotView view)
{
    BotPolicy policy(options);
    std::map<std::uint64_t, std::vector<std::pair<float, float>>> out;
    for (std::uint64_t at = 1000; at < 6000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
            if (action.type == ActionType::kMoveTo && action.target_guid == 0)
                out[action.bot_guid].emplace_back(action.dest_x, action.dest_y);
    }
    return out;
}

float BearingDeg(std::pair<float, float> const& p)
{
    return std::fabs(std::atan2(p.second, p.first) * 180.0f / 3.14159265f);
}

// The default layout: mage 11 in front of her (20 yards north), mage 13 behind her (20 south, inside the tail sweep), the tank 10 yards south of her and her victim,
// mage 10 and priest 14 already on her flanks in their bands.
OnyFixture OnyDefault()
{
    return OnyFixture{ { { 0.0f, 26.0f }, { 20.0f, 0.0f }, { -10.0f, 0.0f }, { -20.0f, 0.0f }, { 0.0f, -23.0f } }, 12 };
}

bool AtSpot(std::pair<float, float> const& m, float x)
{
    return std::fabs(m.first - x) < 0.01f && std::fabs(m.second) < 0.01f;
}

int FlankMoves(std::vector<std::pair<float, float>> const& moves)
{
    int n = 0;
    for (auto const& m : moves)
        n += BearingDeg(m) >= 60.0f && BearingDeg(m) <= 120.0f;
    return n;
}

void BossPositionsKeyOnPutsTheRaidOnHerFlanks()
{
    auto moves = OnyRun(OnyOptions(true), OnyView(OnyDefault()));
    for (std::uint64_t bot : { 11u, 13u })
    {
        CHECK(!moves[bot].empty());
        for (auto const& m : moves[bot])
        {
            CHECK(BearingDeg(m) >= 70.0f - 0.5f && BearingDeg(m) <= 110.0f + 0.5f);              // out of both cones
            CHECK(std::fabs(std::hypot(m.first, m.second) - 26.0f) < 0.5f);                    // the ranged band's middle
        }
    }
    CHECK(moves[10].empty());   // already on a flank, in its band
    CHECK(moves[14].empty());
    // The tank she attacks walks to the tank spot (4, 0), north of her.
    CHECK(!moves[12].empty());
    for (auto const& m : moves[12])
        CHECK(AtSpot(m, 4.0f));
}

void BossPositionsKeyOffChangesNothing()
{
    auto moves = OnyRun(OnyOptions(false), OnyView(OnyDefault()));
    for (auto const& m : moves[11])
        CHECK(BearingDeg(m) < 5.0f);              // only the old band kite, straight back along its bearing (north)
    for (auto const& m : moves[13])
        CHECK(BearingDeg(m) > 175.0f);            // and south
    for (auto const& m : moves[12])
        CHECK(!AtSpot(m, 4.0f));                  // no walk to the tank spot
    CHECK(FlankMoves(moves[11]) == 0 && FlankMoves(moves[13]) == 0);
}

// Left alone: no rows for the boss (the key on), another map, a boss in the air (Onyxia's phase 2, by height and by health), the boss's victim when it is not a tank
// (a mage in front of her with her aggro does not drag her round), and a tank she does not attack.
void BossPositionsLeaveAloneCases()
{
    CHECK(FlankMoves(OnyRun(OnyOptions(true, "frontal|249|10184|18435|Flame Breath\n"), OnyView(OnyDefault()))[11]) == 0);
    CHECK(FlankMoves(OnyRun(OnyOptions(true), OnyView(OnyDefault(), 309))[11]) == 0);
    {
        OnyFixture f = OnyDefault();
        f.bossZ = 20.0f;
        auto moves = OnyRun(OnyOptions(true), OnyView(f));
        CHECK(FlankMoves(moves[11]) == 0);
        for (auto const& m : moves[12])
            CHECK(!AtSpot(m, 4.0f));
    }
    {
        OnyFixture f = OnyDefault();
        f.healthPct = 50;
        CHECK(FlankMoves(OnyRun(OnyOptions(true), OnyView(f))[11]) == 0);
    }
    {
        OnyFixture f = OnyDefault();
        f.victim = 11;
        auto moves = OnyRun(OnyOptions(true), OnyView(f));
        CHECK(FlankMoves(moves[11]) == 0);
        for (auto const& m : moves[12])
            CHECK(!AtSpot(m, 4.0f));              // the tank is not her victim: no tank spot
        CHECK(FlankMoves(moves[13]) > 0);         // the others still go to her flanks
    }
}

// The tank at the spot does not walk out to her: with her 4 yards off it attacks from where it stands, and with the spot 20 yards from her it waits there for her.
void BossPositionsTankHoldsAtTheSpot()
{
    OnyFixture f = OnyDefault();
    f.at[2] = { 4.0f, 0.0f };
    CHECK(OnyRun(OnyOptions(true), OnyView(f))[12].empty());
    f.at[2] = { 20.0f, 0.0f };
    CHECK(OnyRun(OnyOptions(true, "tank_spot|249|10184|20|0|0|3|far\nflank|249|10184|120|120|30|cones\n"), OnyView(f))[12].empty());
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
    AvoidEnemyTanksRowMovesTheTankOnlyWithTheKey();
    RangedFocusFireOnAnAvoidEnemyHostile();
    RangedFocusIgnoresTheCircleOfItsOwnTarget();
    AvoidEnemyAuraHoldsOnlyWhileTheAuraIsOnOrUnknown();
    PurgeCastsDispelMagicOnTheBuffedBoss();
    DungeonPartyCuresCadaverWormsOutOfCombat();
    PurgeShamanUsesPurgeAndNoOtherClassPurges();
    MindControlMageHoldsTheControlledMember();
    MindControlBackupWaitsAndNoRogueCasts();
    LeaderTakesThePartyToTheRepairmanForAMember();
    MemberRepairsWhileTheLeaderWaits();
    BrokenBotWithoutMoneyDoesNotLoop();
    AuctionErrandStandsAtTheAuctioneerBeforeItEnds();
    BossPositionsKeyOnPutsTheRaidOnHerFlanks();
    BossPositionsKeyOffChangesNothing();
    BossPositionsLeaveAloneCases();
    BossPositionsTankHoldsAtTheSpot();
}
