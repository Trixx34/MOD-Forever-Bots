// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for path jitter (src/path_jitter.h), with a fixed seed: off by default, the offset stays within the configured yards, it is stable for the same
// target, it spreads over directions, and it never applies in combat or in a dungeon, near an unwalkable point, or across a required waypoint.
#include "path_jitter.h"

#include <cmath>
#include <cstdio>

using namespace path_jitter;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

constexpr std::uint64_t kSeed = 20261103;

Settings On()
{
    Settings settings;
    settings.Enabled = true;
    return settings;
}

Facts Walking()
{
    Facts facts;
    facts.Walking = true;
    facts.From = { 0.0f, 0.0f };
    return facts;
}

float Dist(Pt a, Pt b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

void OffByDefaultLeavesTheTarget()
{
    Pt const t{ 100.0f, 50.0f };
    CHECK(!Settings{}.Enabled);
    Pt const r = Apply(Settings{}, Walking(), t, kSeed, 1);
    CHECK(r.X == t.X && r.Y == t.Y);
}

void OffsetStaysWithinTheYardsAndSpreads()
{
    Pt const t{ 100.0f, 50.0f };
    int moved = 0, east = 0, west = 0, north = 0, south = 0;
    for (std::uint64_t bot = 1; bot <= 400; ++bot)
    {
        Pt const r = Apply(On(), Walking(), t, kSeed, bot);
        CHECK(Dist(r, t) <= 1.0001f);
        moved += Dist(r, t) > 0.01f;
        east += r.X > t.X;
        west += r.X < t.X;
        north += r.Y > t.Y;
        south += r.Y < t.Y;
    }
    CHECK(moved > 380);
    CHECK(east > 120 && west > 120 && north > 120 && south > 120);
}

void TheYardsKeyScalesTheOffset()
{
    Settings settings = On();
    settings.Yards = 0.25f;
    for (std::uint64_t bot = 1; bot <= 100; ++bot)
        CHECK(Dist(Apply(settings, Walking(), { 10.0f, 10.0f }, kSeed, bot), { 10.0f, 10.0f }) <= 0.2501f);
}

void SameTargetSameOffsetAndDifferentSeedDiffers()
{
    Pt const t{ 100.0f, 50.0f };
    Pt const a = Apply(On(), Walking(), t, kSeed, 5), b = Apply(On(), Walking(), t, kSeed, 5), c = Apply(On(), Walking(), t, kSeed + 1, 5);
    CHECK(a.X == b.X && a.Y == b.Y);   // a re-issued move does not wobble from tick to tick
    CHECK(a.X != c.X || a.Y != c.Y);
}

void NeverInCombatInADungeonOrWhenNotAPlainWalk()
{
    Pt const t{ 100.0f, 50.0f };
    for (int which = 0; which < 5; ++which)
    {
        Facts facts = Walking();
        facts.InCombat = which == 0;
        facts.HasTarget = which == 1;
        facts.EnemyNear = which == 2;
        facts.InDungeon = which == 3;
        facts.Walking = which != 4;
        Pt const r = Apply(On(), facts, t, kSeed, 3);
        CHECK(r.X == t.X && r.Y == t.Y);
    }
}

void KeepsTwoYardsFromUnwalkablePoints()
{
    std::vector<Pt> const bad = { { 100.5f, 50.0f } };   // right beside the target: every jittered point is within 2 yards of it
    Facts facts = Walking();
    facts.Unwalkable = &bad;
    for (std::uint64_t bot = 1; bot <= 100; ++bot)
    {
        Pt const r = Apply(On(), facts, { 100.0f, 50.0f }, kSeed, bot);
        CHECK(r.X == 100.0f && r.Y == 50.0f);
    }
    std::vector<Pt> const far = { { 130.0f, 50.0f } };
    facts.Unwalkable = &far;
    int moved = 0;
    for (std::uint64_t bot = 1; bot <= 100; ++bot)
        moved += Dist(Apply(On(), facts, { 100.0f, 50.0f }, kSeed, bot), { 100.0f, 50.0f }) > 0.01f;
    CHECK(moved > 90);
}

void NeverSkipsARequiredWaypoint()
{
    // The bot walks from (0,0) to (100,0) and a required waypoint lies on the way at (99.5,0), inside the jitter circle of the target.
    std::vector<Pt> const required = { { 99.5f, 0.0f } };
    Facts facts = Walking();
    facts.Required = &required;
    for (std::uint64_t bot = 1; bot <= 200; ++bot)
    {
        Pt const r = Apply(On(), facts, { 100.0f, 0.0f }, kSeed, bot);
        // Either the target stays, or the new line still passes within 2 yards of the waypoint.
        float const dx = r.X - 0.0f, dy = r.Y - 0.0f, len2 = dx * dx + dy * dy;
        float t = ((99.5f) * dx) / len2;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        CHECK(std::hypot(99.5f - t * dx, 0.0f - t * dy) < 2.0f);
    }
}

}  // namespace

int main()
{
    OffByDefaultLeavesTheTarget();
    OffsetStaysWithinTheYardsAndSpreads();
    TheYardsKeyScalesTheOffset();
    SameTargetSameOffsetAndDifferentSeedDiffers();
    NeverInCombatInADungeonOrWhenNotAPlainWalk();
    KeepsTwoYardsFromUnwalkablePoints();
    NeverSkipsARequiredWaypoint();
    if (failures == 0)
        std::printf("path_jitter_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
