// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the capital inspection emote (src/city_inspect.h): off by default, needs a capital, an idle bot, range and a better item; limits per player and per map.
#include "city_inspect.h"

#include <cstdio>

using namespace city_inspect;

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: %s\\n", __FILE__, __LINE__, #cond); } } while (0)

Settings On(std::uint32_t chance = 100)
{
    Settings s;
    s.Enabled = true;
    s.ChancePct = chance;
    return s;
}

Facts Good()
{
    Facts f;
    f.Idle = true;
    f.InCapital = true;
    f.BotItemLevel = 100;
    f.PlayerItemLevel = 200;
    f.Distance = 10.0f;
    return f;
}

void OffAndConditions()
{
    Emote e{};
    Inspector a;
    CHECK(!a.Try(Settings{}, Good(), 1, 1, 9, 0, 1000, e));
    Facts f = Good(); f.Idle = false;           CHECK(!a.Try(On(), f, 1, 1, 9, 0, 1000, e));
    f = Good(); f.InCapital = false;            CHECK(!a.Try(On(), f, 1, 1, 9, 0, 1000, e));
    f = Good(); f.Distance = 25.0f;             CHECK(!a.Try(On(), f, 1, 1, 9, 0, 1000, e));
    f = Good(); f.PlayerItemLevel = 100;        CHECK(!a.Try(On(), f, 1, 1, 9, 0, 1000, e));
    f = Good(); f.PlayerItemLevel = 0;          CHECK(!a.Try(On(), f, 1, 1, 9, 0, 1000, e));
    CHECK(!a.Try(On(0), Good(), 1, 1, 9, 0, 1000, e));
}

void EmoteAndPlayerLimit()
{
    Emote e{};
    Inspector a;
    CHECK(a.Try(On(), Good(), 1, 1, 9, 0, 1000, e));
    CHECK(static_cast<std::uint32_t>(e) <= 2);
    CHECK(!a.Try(On(), Good(), 1, 2, 9, 0, 2000, e));            // another bot, same player, within 5 minutes
    CHECK(a.Try(On(), Good(), 1, 2, 9, 0, 1000 + 5ull * 60000 + 1, e));
}

void ZoneLimit()
{
    Emote e{};
    Inspector a;
    CHECK(a.Try(On(), Good(), 1, 1, 10, 7, 1000, e));
    CHECK(a.Try(On(), Good(), 1, 2, 11, 7, 1100, e));
    CHECK(!a.Try(On(), Good(), 1, 3, 12, 7, 1200, e));           // ZoneMax 2 a minute on map 7
    CHECK(a.Try(On(), Good(), 1, 3, 12, 8, 1200, e));            // another map
    CHECK(a.Try(On(), Good(), 1, 3, 13, 7, 62000, e));           // a minute later
}
}  // namespace

int main()
{
    OffAndConditions();
    EmoteAndPlayerLimit();
    ZoneLimit();
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("city_inspect_test: ok\n");
    return 0;
}
