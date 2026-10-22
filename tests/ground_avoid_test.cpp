// Unit tests for the ground-effect avoidance logic (src/ground_avoid.h) and for data/avoid_spells.txt.
#include "ground_avoid.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ground_avoid;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

float Dist(Point2 p, float x, float y) { return std::hypot(p.X - x, p.Y - y); }

void ParsesTheTable()
{
    std::vector<std::string> errors;
    AvoidTable const t = ParseAvoidSpells("# a comment\n\n11829|5.0|persistent_area|36|1729|Flamestrike (Defias Evoker)\n8364|8|persistent_area|189|4294|Blizzard\nbad line\n0|5|x\n99|abc|x\n", errors);
    CHECK(t.Size() == 2);
    CHECK(t.Contains(11829) && t.Radius(11829) == 5.0f);
    CHECK(t.Contains(8364) && t.Radius(8364) == 8.0f);
    CHECK(!t.Contains(1) && t.Radius(1) == 0.0f);
    CHECK(errors.size() == 3);                                   // 'bad line', spell 0 and a radius that is no number
    AvoidTable twice;
    twice.Add(5, 3.0f);
    twice.Add(5, 8.0f);
    twice.Add(5, 2.0f);
    CHECK(twice.Radius(5) == 8.0f);                              // the largest radius of a spell on several maps
}

void ParsesTheShippedFile()
{
#ifdef FOREVER_AVOID_SPELLS_FILE
    std::FILE* f = std::fopen(FOREVER_AVOID_SPELLS_FILE, "rb");
    CHECK(f != nullptr);
    if (!f)
        return;
    std::string text;
    char buffer[4096];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0)
        text.append(buffer, n);
    std::fclose(f);
    std::vector<std::string> errors;
    AvoidTable const t = ParseAvoidSpells(text, errors);
    CHECK(errors.empty());
    CHECK(t.Size() >= 15);
    CHECK(t.Contains(11829));                                    // Flamestrike of the Defias Evoker: the casters of the Deadmines' Sneed room
    CHECK(t.Contains(8364));                                     // Blizzard
#endif
}

void NoEscapeWhenOutside()
{
    CHECK(!EscapePoint(0, 0, {}).has_value());
    CHECK(!EscapePoint(0, 0, { Circle{ 20, 0, 5 } }).has_value());          // 20 yards from a 5-yard effect
    CHECK(InsideAny(0, 0, { Circle{ 5.5f, 0, 5 } }));                        // 5.5 yards from the center of a 5-yard effect: inside the 1-yard margin
    CHECK(!InsideAny(7, 0, { Circle{ 0, 0, 5 } }));                         // 7 yards from its center: outside
}

void LeavesByTheShortestWay()
{
    // The bot stands 1 yard from the center of a 5-yard effect: the way out is 7 yards (5 + the 3-yard margin, less the 1 it is off center), away from the center.
    auto const out = EscapePoint(1, 0, { Circle{ 0, 0, 5 } });
    CHECK(out.has_value());
    if (out)
    {
        CHECK(Dist(*out, 0, 0) >= 5.0f + kExitMarginYards - 0.01f);
        CHECK(out->X > 1.0f);                                                // away from the center, not across it
        CHECK(Dist(*out, 1, 0) < 8.5f);
    }
    // Exactly on the center: some way out, the same length from every direction.
    auto const centre = EscapePoint(0, 0, { Circle{ 0, 0, 5 } });
    CHECK(centre.has_value());
    if (centre)
        CHECK(Dist(*centre, 0, 0) >= 8.0f - 0.01f && Dist(*centre, 0, 0) <= 9.1f);
}

void DoesNotWalkIntoAnotherEffect()
{
    // Two effects side by side: the way straight out of the first (to the east) is blocked by the second, so the bot goes round.
    std::vector<Circle> const circles = { Circle{ 0, 0, 5 }, Circle{ 9, 0, 5 } };
    auto const out = EscapePoint(1, 0, circles);
    CHECK(out.has_value());
    if (out)
        CHECK(!InsideAny(out->X, out->Y, circles) && Dist(*out, 0, 0) >= 8.0f - 0.01f && Dist(*out, 9, 0) >= 8.0f - 0.01f);
    // Boxed in by effects all around within 40 yards: no way out, no answer (the bot stays and takes it).
    std::vector<Circle> const huge = { Circle{ 0, 0, 60 } };
    CHECK(!EscapePoint(1, 0, huge).has_value());
}

} // namespace

int main()
{
    ParsesTheTable();
    ParsesTheShippedFile();
    NoEscapeWhenOutside();
    LeavesByTheShortestWay();
    DoesNotWalkIntoAnotherEffect();
    if (failures)
    {
        std::printf("ground_avoid_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("ground_avoid_test: all checks passed\n");
    return 0;
}
