// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the raid rule table (raid_rules.h) and the shipped data/raid_rules.txt.
#include "raid_rules.h"

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
    CHECK(!t.PullPositionOf(309, 11350));    // the lone thrower shares the entry: no pullpos row, Axe Flurry is avoided (avoid_enemy)
    CHECK(t.AvoidEnemyFor(309, 11350).size() == 2);
    for (std::uint32_t entry : { 11371u, 11372u })
    {
        std::optional<PullPosition> const pos = t.PullPositionOf(309, entry);
        CHECK(pos.has_value() && pos->X == -11872.5f && pos->Y == -1287.7f && pos->Yards == 8.0f);
    }
}

} // namespace

int main()
{
    TestRows();
    TestWildcardNoAoe();
    TestLeash();
    TestSpacingAndAuraRows();
    TestPullPosition();
    TestErrors();
    TestMerge();
    TestShippedFile();
    if (failures == 0)
        std::printf("raid_rules_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
