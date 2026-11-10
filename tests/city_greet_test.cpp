// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the city greeting (src/city_greet.h): off by default, needs a capital, an idle bot and range; limits per player and per map.
#include "city_greet.h"

#include <cstdio>

using namespace city_greet;

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

Settings On(std::uint32_t chance = 100) { Settings s; s.Enabled = true; s.ChancePct = chance; return s; }
Facts Good() { Facts f; f.Idle = true; f.InCapital = true; f.Distance = 8.0f; return f; }

void Conditions()
{
    std::uint32_t d = 0;
    Greeter g;
    CHECK(!g.Try(Settings{}, Good(), 1, 1, 9, 0, 1000, d));
    Facts f = Good(); f.Idle = false;       CHECK(!g.Try(On(), f, 1, 1, 9, 0, 1000, d));
    f = Good(); f.InCapital = false;        CHECK(!g.Try(On(), f, 1, 1, 9, 0, 1000, d));
    f = Good(); f.Distance = 20.0f;         CHECK(!g.Try(On(), f, 1, 1, 9, 0, 1000, d));
    CHECK(!g.Try(On(0), Good(), 1, 1, 9, 0, 1000, d));
    CHECK(g.Try(On(), Good(), 1, 1, 9, 0, 1000, d));
}

void Limits()
{
    std::uint32_t d = 0;
    Greeter g;
    CHECK(g.Try(On(), Good(), 1, 1, 9, 0, 1000, d));
    CHECK(!g.Try(On(), Good(), 1, 2, 9, 0, 2000, d));                   // same player within 10 minutes
    CHECK(g.Try(On(), Good(), 1, 2, 9, 0, 1000 + 10 * 60 * 1000, d));   // after it
    Settings s = On(); s.ZoneMax = 1;
    Greeter h;
    CHECK(h.Try(s, Good(), 1, 1, 10, 5, 1000, d));
    CHECK(!h.Try(s, Good(), 1, 2, 11, 5, 1500, d));                     // the map's rate is used up
    CHECK(h.Try(s, Good(), 1, 3, 12, 6, 1500, d));                      // another map
}
}  // namespace

int main()
{
    Conditions();
    Limits();
    if (failures == 0) std::printf("city_greet_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
