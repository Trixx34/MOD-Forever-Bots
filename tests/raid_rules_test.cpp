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
    CHECK(t.AvoidOf(309, true).size() == 1 && t.AvoidOf(309, true)[0].Id == 177764 && t.AvoidOf(309, true)[0].Radius == 6.0f);
    CHECK(t.SwapStacks(309, 12097).has_value());
    CHECK(t.IsBoss(309, 14517));
}

} // namespace

int main()
{
    TestRows();
    TestWildcardNoAoe();
    TestErrors();
    TestMerge();
    TestShippedFile();
    if (failures == 0)
        std::printf("raid_rules_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
