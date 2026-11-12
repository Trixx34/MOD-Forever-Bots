// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the module's main helpers (src/main_util.cpp) and the shared text helpers (src/text_split.h). No core needed.
#include "main_internal.h"
#include "text_split.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

using namespace main_detail;

static void Names()
{
    CHECK(std::string(ActionTypeName(forever_module::ActionType::kCastSpell)) == "cast");
    CHECK(std::string(ActionTypeName(forever_module::ActionType::kSellToVendor)) == "sell");
    CHECK(std::string(ActionTypeName(forever_module::ActionType::kReclaimCorpse)) == "reclaim");
    CHECK(JoinWords({ "a", "b", "c" }, 0) == "a b c" && JoinWords({ "a", "b", "c" }, 1) == "b c" && JoinWords({ "a" }, 1).empty() && JoinWords({}, 0).empty());
    CHECK(LowerCopy("MiXeD 12") == "mixed 12");
    CHECK(ActionsPerTick(10, 4) == "2.5" && ActionsPerTick(5, 0) == "0.0" && ActionsPerTick(0, 7) == "0.0");
    CHECK(BesideTheModule("") == "" && BesideTheModule("/abs/x.txt") == "/abs/x.txt" && BesideTheModule("no_such_file_here.txt") == "no_such_file_here.txt");
}

static void Routes()
{
    Dungeon d;
    d.Id = "dm";
    d.Name = "Deadmines";
    d.MinLevel = 15;
    d.MaxLevel = 25;
    d.Entry = { 36.0f, -14.0f, -385.0f, 62.0f };
    d.Route = { { 1.0f, 2.0f, 3.0f, 0, "" }, { 4.0f, 5.0f, 6.0f, 639, "Edwin" } };
    DungeonBoss boss;
    boss.Entry = 639;
    boss.Strategy.AvoidSpells = { 111, 222 };
    boss.Strategy.Avoid.push_back(AvoidZone{ 7.0f, 8.0f, 9.0f, 5.0f, "" });
    d.Bosses.push_back(boss);
    Dungeon second;
    second.Id = "wc";
    second.Entry = { 43.0f, 1.0f, 1.0f, 1.0f };
    auto const routes = DungeonRoutes({ d, second });
    CHECK(routes.size() == 2 && routes.count(36) == 1 && routes.count(43) == 1);
    auto const found = routes.find(36);
    if (found != routes.end())
    {
        PolicyOptions::DungeonRoute const& r = found->second;
        CHECK(r.Id == "dm" && r.Name == "Deadmines" && r.MinLevel == 15 && r.MaxLevel == 25 && r.HasEntry);
        CHECK(r.Route.size() == 2 && r.BossAt.size() == 1 && r.BossAt.count(1) == 1 && r.BossAt.at(1) == 639);
        CHECK(r.Entry.X == -14.0f && r.Entry.Z == 62.0f);
        CHECK(r.AvoidSpells.size() == 2 && r.AvoidZones.size() == 1 && r.AvoidZones[0].Radius == 5.0f && r.AvoidZones[0].X == 7.0f);
    }
}

static void Text()
{
    CHECK(text_split::TrimBlanks(" \t x y \r") == "x y" && text_split::TrimBlanks("  ").empty() && text_split::TrimBlanks("\nx") == "\nx");
    std::uint32_t n = 7;
    CHECK(text_split::ParseUint32("123", n) && n == 123);
    CHECK(text_split::ParseUint32("4294967295", n) && n == 4294967295u);
    n = 5;
    CHECK(!text_split::ParseUint32("4294967296", n) && !text_split::ParseUint32("", n) && !text_split::ParseUint32("-1", n) && !text_split::ParseUint32(" 1", n) &&
          !text_split::ParseUint32("12345678901", n) && n == 5);
    CHECK(text_split::Split("a,,b", ',').size() == 3);
}

int main()
{
    Names();
    Routes();
    Text();
    if (failures == 0)
        std::printf("main_util: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
