// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the raid rule table (raid_rules.h) and the shipped data/raid_rules.txt.
#include "raid_rules.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace raid_rules;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

void TestRows()
{
    std::vector<std::string> errors;
    Table t = Parse("# comment\n"
                    "boss|249|10184|Onyxia\n"
                    "add|249|11262|0|Whelp\n"
                    "interrupt|309|23954|Great Heal\n"
                    "dispel|309|16098|curse|Curse of Blood\n"
                    "dispel|309|23952|magic|SWP\r\n"
                    "avoid|309|object|177764|6|Liquid Fire\n"
                    "avoid|249|spell|18392|8.5|Fireball\n"
                    "swap|309|14517|12097|3|Pierce Armor\n"
                    "noaoe|309|14507|Venoxis\n"
                    "frontal|249|10184|18500|Wing Buffet\n",
                    errors);
    CHECK(errors.empty());
    CHECK(t.MapCount() == 2);
    CHECK(t.IsBoss(249, 10184));
    CHECK(!t.IsBoss(309, 10184));
    CHECK(t.AddRank(249, 11262).has_value() && *t.AddRank(249, 11262) == 0);
    CHECK(!t.AddRank(249, 1).has_value());
    CHECK(!t.AddRank(1, 11262).has_value());
    CHECK(t.InterruptFirst(309, 23954));
    CHECK(!t.InterruptFirst(249, 23954));
    CHECK(t.DispelKindOf(309, 16098) == DispelKind::Curse);
    CHECK(t.DispelKindOf(309, 23952) == DispelKind::Magic);
    CHECK(!t.DispelKindOf(309, 1).has_value());
    CHECK(t.AvoidOf(309, true).size() == 1 && t.AvoidOf(309, true)[0].Id == 177764 && t.AvoidOf(309, true)[0].Radius == 6.0f);
    CHECK(t.AvoidOf(309, false).empty());
    CHECK(t.AvoidOf(249, false).size() == 1 && t.AvoidOf(249, false)[0].Radius == 8.5f);
    CHECK(t.SwapStacks(309, 12097).has_value() && *t.SwapStacks(309, 12097) == 3);
    CHECK(!t.SwapStacks(309, 5).has_value());
    CHECK(t.NoAoe(309, 14507));
    CHECK(!t.NoAoe(309, 14517));
    CHECK(t.IsFrontal(249, 10184, 18500));
    CHECK(!t.IsFrontal(249, 10184, 18435));
    CHECK(t.IsFrontalAny(249, 18500) && !t.IsFrontalAny(249, 18435) && !t.IsFrontalAny(1, 18500));
}

void TestWildcardNoAoe()
{
    std::vector<std::string> errors;
    Table t = Parse("noaoe|309|0|every fight\n", errors);
    CHECK(errors.empty());
    CHECK(t.NoAoe(309, 14517));
    CHECK(!t.NoAoe(249, 14517));
}

void TestLeash()
{
    std::vector<std::string> errors;
    Table t = Parse("leash|309|15|shelf\nleash|249|5|too small\nleash|36|x|bad\n", errors);
    CHECK(errors.size() == 2);
    CHECK(t.PullLeash(309) == 15 && t.PullLeash(249) == 0 && t.PullLeash(1) == 0);
}

void TestSpacingAndAuraRows()
{
    std::vector<std::string> errors;
    Table t = Parse("spread|309|14507|10|ranged|Holy Wrath\n"
                    "spread|309|0|8|all|any fight\n"
                    "avoid_enemy|309|14507|15|healers|Holy Nova\n"
                    "avoid_enemy|309|14507|15|ranged\n"
                    "purge|309|23895|Renew\n"
                    "mindcontrol|309|24327|Cause Insanity\n",
                    errors);
    CHECK(errors.empty());
    auto const boss = t.SpreadFor(309, 14507);
    CHECK(boss.size() == 2 && boss[0].Yards == 10.0f && boss[0].Who == kWhoRanged && boss[1].Entry == 0 && boss[1].Who == kWhoAll);
    CHECK(t.SpreadFor(309, 1).size() == 1 && t.SpreadFor(249, 14507).empty());
    auto const nova = t.AvoidEnemyFor(309, 14507);
    CHECK(nova.size() == 2 && nova[0].Yards == 15.0f && nova[0].Who == kWhoHealers && nova[1].Who == kWhoRanged);
    CHECK(t.AvoidEnemyFor(309, 1).empty() && t.HasAvoidEnemy(309) && !t.HasAvoidEnemy(249));
    CHECK(t.IsPurgeAura(309, 23895) && !t.IsPurgeAura(309, 1) && !t.IsPurgeAura(249, 23895));
    CHECK(t.IsMindControlAura(309, 24327) && !t.IsMindControlAura(309, 23895));
    // bad rows: yards out of range, unknown who, entry 0 for avoid_enemy, missing fields, aura 0
    std::vector<std::string> bad;
    Table b = Parse("spread|309|1|1|all|x\nspread|309|1|70|all|x\nspread|309|1|10|everyone|x\nspread|309|1|10\n"
                    "avoid_enemy|309|0|10|all|x\navoid_enemy|309|5|10|none|x\npurge|309|0|x\nmindcontrol|309|abc|x\n", bad);
    CHECK(bad.size() == 8);
    CHECK(b.SpreadFor(309, 1).empty() && !b.HasAvoidEnemy(309) && !b.IsPurgeAura(309, 0));
}

void TestAvoidEnemyAuraRows()
{
    std::vector<std::string> errors;
    Table t = Parse("avoid_enemy|309|11350|33|healers|always\n"
                    "avoid_enemy_aura|309|11350|33|ranged|24018|Axe Flurry\n",
                    errors);
    CHECK(errors.empty());
    auto const zones = t.AvoidEnemyFor(309, 11350);
    CHECK(zones.size() == 2 && zones[0].Aura == 0 && zones[1].Aura == 24018 && zones[1].Who == kWhoRanged && zones[1].Yards == 33.0f);
    CHECK(t.HasAvoidEnemy(309));
    // bad rows: aura missing or 0, entry 0, yards out of range, unknown who
    std::vector<std::string> bad;
    Table b = Parse("avoid_enemy_aura|309|11350|33|ranged\navoid_enemy_aura|309|11350|33|ranged|0|x\navoid_enemy_aura|309|0|33|ranged|24018|x\n"
                    "avoid_enemy_aura|309|11350|90|ranged|24018|x\navoid_enemy_aura|309|11350|33|nobody|24018|x\n", bad);
    CHECK(bad.size() == 5);
    CHECK(!b.HasAvoidEnemy(309));
}

// `tanks` (ZG levers): its own group bit, not part of `all`.
void TestTanksGroup()
{
    std::vector<std::string> errors;
    Table t = Parse("avoid_enemy|309|11350|33|tanks|x\navoid_enemy|309|11350|33|all|x\n", errors);
    CHECK(errors.empty());
    auto const zones = t.AvoidEnemyFor(309, 11350);
    CHECK(zones.size() == 2 && zones[0].Who == kWhoTanks && (zones[1].Who & kWhoTanks) == 0);
}

void TestPullPosition()
{
    std::vector<std::string> errors;
    Table t = Parse("pullpos|309|11350|-11810.5|-1317|62|8|shelf\n"
                    "pullpos|309|11350|1|2|3|9|second\n"           // the same entry twice: the first is kept
                    "pullpos|309|11371|-11810|-1317|62|1|too small\n"
                    "pullpos|309|11372|-11810|-1317|62|41|too big\n"
                    "pullpos|309|11372|x|-1317|62|8\n"
                    "pullpos|309|0|1|2|3|8\n"
                    "pullpos|309|11372|1|2|3\n",                    // too short
                    errors);
    CHECK(errors.size() == 6);
    std::optional<PullPosition> const p = t.PullPositionOf(309, 11350);
    CHECK(p.has_value() && p->X == -11810.5f && p->Y == -1317.0f && p->Z == 62.0f && p->Yards == 8.0f);
    CHECK(!t.PullPositionOf(309, 11371) && !t.PullPositionOf(309, 11372) && !t.PullPositionOf(249, 11350));
}

// pullpos_near: one camp of an entry; the nearest centre within its reach wins, a `pullpos` row of the entry is the fallback, the 2-argument query sees only `pullpos` rows.
void TestScopedPullPosition()
{
    std::vector<std::string> errors;
    Table t = Parse("pullpos_near|309|11350|-11872.5|-1287.7|82.3|8|-11830|-1321|12|stairs camp\n"
                    "pullpos_near|309|11350|-11939.5|-1519.7|42.8|8|-11965|-1552|12|south camp\n"
                    "pullpos_near|309|11350|1|2|3|8|-11968|-1556|12|overlapping, farther centre\n"
                    "pullpos_near|309|11830|1|2|3|8|4|5|1|reach too small\n"
                    "pullpos_near|309|11830|1|2|3|8|4|5|201|reach too big\n"
                    "pullpos_near|309|11830|1|2|3|41|4|5|10|yards too big\n"
                    "pullpos_near|309|11830|1|2|3|8|4|5\n",          // too short
                    errors);
    CHECK(errors.size() == 4);
    std::optional<PullPosition> const stairs = t.PullPositionOf(309, 11350, -11830.6f, -1317.0f);
    CHECK(stairs.has_value() && stairs->X == -11872.5f && stairs->Y == -1287.7f && stairs->Yards == 8.0f);
    std::optional<PullPosition> const south = t.PullPositionOf(309, 11350, -11966.0f, -1553.0f);
    CHECK(south.has_value() && south->X == -11939.5f);
    CHECK(!t.PullPositionOf(309, 11350, -11882.7f, -1316.1f));    // the shelf throwers: in no reach
    CHECK(!t.PullPositionOf(309, 11350));                          // no map-wide row
    CHECK(!t.PullPositionOf(309, 11830, 4.0f, 5.0f) && !t.PullPositionOf(249, 11350, -11830.6f, -1317.0f));
    Table both = Parse("pullpos|309|11371|-11810|-1317|62|8|map-wide\n"
                       "pullpos_near|309|11371|-11872.5|-1287.7|82.3|8|-11864|-1315|10|shelf camp\n", errors);
    CHECK(both.PullPositionOf(309, 11371, -11866.1f, -1313.0f)->X == -11872.5f);
    CHECK(both.PullPositionOf(309, 11371, -11738.0f, -1336.0f)->X == -11810.0f);
    CHECK(both.PullPositionOf(309, 11371)->X == -11810.0f);
}

void TestErrors()
{
    std::vector<std::string> errors;
    Table t = Parse("dispel|309|16098|fire|x\n"        // unknown kind
                    "avoid|309|ground|5|6|x\n"          // unknown avoid kind
                    "avoid|309|spell|5|0|x\n"           // zero radius
                    "add|309|1|0|a\n"
                    "add|309|1|2|b\n"                   // duplicate entry
                    "swap|309|1|2\n"                    // too short
                    "bogus|309|1\n"
                    "interrupt|abc|5\n"
                    "interrupt|309|99|ok\n",
                    errors);
    CHECK(errors.size() == 7);
    CHECK(t.InterruptFirst(309, 99));
    CHECK(*t.AddRank(309, 1) == 0);     // the first rank is kept
    CHECK(t.AvoidOf(309, false).empty());
    CHECK(!t.DispelKindOf(309, 16098).has_value());
}

void TestMerge()
{
    std::vector<std::string> errors;
    Table t = Parse("add|249|11262|0|Whelp\nadd|249|12129|1|Warder\n", errors);
    std::map<std::uint32_t, std::uint32_t> priority{ { 12129, 5 } };
    MergeKillPriority(t, 249, priority);
    CHECK(priority.size() == 2 && priority[11262] == 0 && priority[12129] == 5);   // the dungeon file's rank stays
    std::map<std::uint32_t, std::uint32_t> other;
    MergeKillPriority(t, 309, other);
    CHECK(other.empty());
}

// tank_spot and flank rows (RaidBossPositions): parsed, bad values refused, a second row for the same boss refused.
void TestBossPositionRows()
{
    std::vector<std::string> errors;
    Table t = Parse("tank_spot|249|10184|-0.9|-217.2|-86.0|5|Onyxia\n"
                    "flank|249|10184|120|120|30|Onyxia\n"
                    "tank_spot|249|10184|9|9|9|5|second\n"     // line 3: twice
                    "tank_spot|249|10185|1|2|3|20|far\n"      // line 4: yards above 15
                    "flank|249|10186|200|60|30|wide\n"        // line 5: front above 180
                    "flank|249|10187|170|170|30|no flank\n"   // line 6: together above 300
                    "flank|249|10188|120|120|-1|neg\n",       // line 7: rear yards below 0
                    errors);
    CHECK(errors.size() == 5);
    auto const spot = t.TankSpotOf(249, 10184);
    CHECK(spot && std::fabs(spot->X + 0.9f) < 0.01f && std::fabs(spot->Y + 217.2f) < 0.01f && std::fabs(spot->Yards - 5.0f) < 0.01f);
    auto const flank = t.FlankOf(249, 10184);
    CHECK(flank && flank->FrontDeg == 120.0f && flank->RearDeg == 120.0f && flank->RearYards == 30.0f);
    CHECK(!t.TankSpotOf(249, 10185) && !t.FlankOf(249, 10186) && !t.FlankOf(249, 10187) && !t.FlankOf(249, 10188));
    CHECK(!t.TankSpotOf(309, 10184) && !t.FlankOf(309, 10184));
    CHECK(t.HasTankSpot(249) && !t.HasTankSpot(309));
}

// The geometry: Onyxia at her spawn (-4.9, -217.2), the tank spot 4 yards north. She faces north (0) while held there, whatever her own orientation says.
void TestBossPositionGeometry()
{
    TankSpot const spot{ -0.9f, -217.2f, -86.0f, 5.0f };
    CHECK(std::fabs(PlannedFacing(spot, -4.9f, -217.2f, 2.0f)) < 0.01f);
    CHECK(std::fabs(PlannedFacing(spot, -40.0f, -217.2f, 2.0f) - 2.0f) < 0.01f);   // far from the spot: her own orientation
    CHECK(std::fabs(PlannedFacing(std::nullopt, -4.9f, -217.2f, 1.5f) - 1.5f) < 0.01f);
    CHECK(std::fabs(PlannedFacing(spot, -0.9f, -217.2f, 1.5f) - 1.5f) < 0.01f);    // on the spot itself: no direction, her orientation
    // Bearings relative to a north facing: ahead 0, +y (west) +90, behind 180, -y -90.
    CHECK(std::fabs(RelativeBearingDeg(0, 0, 0, 10, 0)) < 0.01f);
    CHECK(std::fabs(RelativeBearingDeg(0, 0, 0, 0, 10) - 90.0f) < 0.01f);
    CHECK(std::fabs(std::fabs(RelativeBearingDeg(0, 0, 0, -10, 0)) - 180.0f) < 0.01f);
    CHECK(std::fabs(RelativeBearingDeg(0, 0, 0, 0, -10) + 90.0f) < 0.01f);
    CHECK(std::fabs(RelativeBearingDeg(0, 0, 3.14159265f, 10, 0) - 180.0f) < 0.01f);   // facing south: north is behind
    FlankRule const rule{ 120.0f, 120.0f, 30.0f };
    CHECK(InBossCone(rule, 0.0f, 40.0f, 5.0f));       // in front, far: the breath reaches 45
    CHECK(InBossCone(rule, -62.0f, 10.0f, 5.0f));     // inside the front edge plus margin
    CHECK(!InBossCone(rule, 90.0f, 26.0f, 5.0f));     // a flank
    CHECK(InBossCone(rule, 170.0f, 20.0f, 5.0f));     // behind, within 30: tail sweep
    CHECK(!InBossCone(rule, 170.0f, 35.0f, 5.0f));    // behind, beyond 30
    // Flank bearings: the 70..110 sector (60 + 10, 180 - 60 - 10) in 5 slots, slot middles 74, 82, 90, 98, 106; the right side negative.
    CHECK(std::fabs(FlankBearingDeg(rule, 1, 0, 5, 10.0f) - 74.0f) < 0.01f);
    CHECK(std::fabs(FlankBearingDeg(rule, 1, 2, 5, 10.0f) - 90.0f) < 0.01f);
    CHECK(std::fabs(FlankBearingDeg(rule, -1, 4, 5, 10.0f) + 106.0f) < 0.01f);
    for (unsigned slot = 0; slot < 5; ++slot)
        for (int side : { 1, -1 })
            CHECK(!InBossCone(rule, FlankBearingDeg(rule, side, slot, 5, 10.0f), 5.5f, 5.0f));   // every flank point is out of both cones, melee distance too
    // A narrow flank (front 170, rear 120: the sector is 85..120) still gives a bearing between the cones.
    FlankRule const narrow{ 170.0f, 120.0f, 30.0f };
    float const deg = FlankBearingDeg(narrow, 1, 0, 5, 20.0f);
    CHECK(deg >= 85.0f && deg <= 120.0f);
}

void TestShippedFile()
{
    std::ifstream file(FOREVER_RAID_RULES_FILE);
    std::stringstream text;
    text << file.rdbuf();
    CHECK(static_cast<bool>(file));
    std::vector<std::string> errors;
    Table t = Parse(text.str(), errors);
    for (std::string const& e : errors)
        std::printf("shipped file: %s\n", e.c_str());
    CHECK(errors.empty());
    // Onyxia: whelps first
    CHECK(t.AddRank(249, 11262).has_value() && *t.AddRank(249, 11262) == 0);
    CHECK(t.IsFrontal(249, 10184, 18500) && t.IsFrontal(249, 10184, 18435));
    CHECK(t.TankSpotOf(249, 10184).has_value() && t.FlankOf(249, 10184).has_value());   // Onyxia's boss positions (RaidBossPositions)
    CHECK(t.IsLethalSpell(249, 17086) && !t.IsLethalSpell(249, 18500) && !t.IsLethalSpell(309, 17086));
    // Zul'Gurub rows from the task
    for (std::uint32_t spell : { 23954u, 24208u, 24300u, 23953u })
        CHECK(t.InterruptFirst(309, spell));
    CHECK(*t.AddRank(309, 11368) == 0 && *t.AddRank(309, 14965) == 0);
    CHECK(t.DispelKindOf(309, 16098) == DispelKind::Curse);
    CHECK(t.DispelKindOf(309, 23952) == DispelKind::Magic);
    // trap objects are spell rows (kind 3 ground effects carry the trap's spell)
    {
        auto const zones = t.AvoidOf(309, false);
        auto radiusOf = [&](std::uint32_t spell) { for (auto const& z : zones) if (z.Id == spell) return z.Radius; return 0.0f; };
        CHECK(radiusOf(19628) == 5.0f);     // Jeklik's bomb, not 6
        CHECK(radiusOf(23972) == 3.5f);     // Liquid Fire is GO 180125 / trap 23972
        CHECK(t.AvoidOf(309, true).empty());
        auto const mc = t.AvoidOf(409, false);
        bool lava = false, burst = false;
        for (auto const& z : mc) { lava = lava || (z.Id == 19428 && z.Radius == 2.5f); burst = burst || z.Id == 21158; }
        CHECK(lava && burst);
    }
    CHECK(t.SwapStacks(309, 12097).has_value());
    CHECK(t.IsBoss(309, 14517));
    // Venoxis (14507): Holy Nova radius 15 (SpellRadius), Renew 23895 (his own cast, a periodic heal), Hakkar's Cause Insanity 24327 (charm aura)
    CHECK(t.IsPurgeAura(309, 23895) && t.IsMindControlAura(309, 24327));
    CHECK(!t.AvoidEnemyFor(309, 14507).empty() && t.AvoidEnemyFor(309, 14507)[0].Yards == 15.0f);
    CHECK(!t.SpreadFor(309, 14507).empty());
    CHECK(!t.PullPositionOf(309, 11350));    // no map-wide row for the thrower (the entry has many camps); pullpos_near rows below
    // healers, ranged, and (ZG levers, acted on only with RaidAvoidEnemyMelee / RaidAvoidEnemyTank) melee and tanks
    auto const thrower = t.AvoidEnemyFor(309, 11350);
    CHECK(thrower.size() == 4);
    std::uint8_t groups = 0;
    for (EnemyZone const& zone : thrower)
        groups |= zone.Who;
    CHECK(groups == (kWhoAll | kWhoTanks));
    for (std::uint32_t entry : { 11371u, 11372u })
    {
        CHECK(!t.PullPositionOf(309, entry));    // no map-wide row since 2026-11-15: the shelf camp only (pullpos_near)
        std::optional<PullPosition> const pos = t.PullPositionOf(309, entry, -11866.1f, -1313.0f);
        CHECK(pos.has_value() && pos->X == -11872.5f && pos->Y == -1287.7f && pos->Yards == 8.0f);
        CHECK(!t.PullPositionOf(309, entry, -11737.7f, -1335.7f));    // the snakes at shipped route point 14: no pull position
    }
    // the stairs camp (pack 72) is pulled from the ledge; the shelf throwers (pack 61) have no pull position
    CHECK(t.PullPositionOf(309, 11350, -11830.6f, -1317.0f)->X == -11872.5f && t.PullPositionOf(309, 11830, -11829.2f, -1324.6f)->X == -11872.5f);
    CHECK(!t.PullPositionOf(309, 11350, -11882.7f, -1316.1f));
    CHECK(t.PullPositionOf(309, 11350, -11969.0f, -1558.0f)->X == -11939.5f);    // pack 122, proposed route point 14
}

} // namespace

int main()
{
    TestRows();
    TestWildcardNoAoe();
    TestLeash();
    TestSpacingAndAuraRows();
    TestAvoidEnemyAuraRows();
    TestTanksGroup();
    TestPullPosition();
    TestScopedPullPosition();
    TestErrors();
    TestMerge();
    TestBossPositionRows();
    TestBossPositionGeometry();
    TestShippedFile();
    if (failures == 0)
        std::printf("raid_rules_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
