// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the no-route triage (route_triage.h). No core needed.
#include "route_triage.h"

#include <cstdio>
#include <string>

using route_triage::Classify;
using route_triage::Name;
using route_triage::NoRouteCause;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

// A refusal with a move on record, outside combat, to a point, with the bot far from its destination.
void PointMoveFarAwayIsToPoint()
{
    CHECK(Classify(false, true, false, 50.0f) == NoRouteCause::kToPoint);
}

// A move to a unit is classified as such, whatever the distance.
void UnitMoveIsToUnit()
{
    CHECK(Classify(false, true, true, 50.0f) == NoRouteCause::kToUnit);
}

// Standing at the destination is an arrival, not a missing route, and it wins over combat.
void ArrivalWinsOverEverything()
{
    CHECK(Classify(false, true, false, 0.5f) == NoRouteCause::kArrived);
    CHECK(Classify(true, true, true, 5.9f) == NoRouteCause::kArrived);
    // Exactly the counting threshold is no longer an arrival.
    CHECK(Classify(false, true, false, route_triage::kArrivedYards) == NoRouteCause::kToPoint);
}

// In combat with a move on record, the move was a chase or a retreat.
void CombatIsCombat()
{
    CHECK(Classify(true, true, true, 30.0f) == NoRouteCause::kCombat);
    CHECK(Classify(true, true, false, 30.0f) == NoRouteCause::kCombat);
}

// No move on record (the refusal is for something the module did not track), outside an arrival.
void NoMoveOnRecord()
{
    CHECK(Classify(false, false, false, 0.0f) == NoRouteCause::kNoMove);
    CHECK(Classify(true, false, false, 0.0f) == NoRouteCause::kCombat);
}

void NamesAreTheJournalWords()
{
    CHECK(std::string(Name(NoRouteCause::kArrived)) == "arrived");
    CHECK(std::string(Name(NoRouteCause::kCombat)) == "combat");
    CHECK(std::string(Name(NoRouteCause::kToUnit)) == "to_unit");
    CHECK(std::string(Name(NoRouteCause::kToPoint)) == "to_point");
    CHECK(std::string(Name(NoRouteCause::kNoMove)) == "no_move");
}

} // namespace

int main()
{
    PointMoveFarAwayIsToPoint();
    UnitMoveIsToUnit();
    ArrivalWinsOverEverything();
    CombatIsCombat();
    NoMoveOnRecord();
    NamesAreTheJournalWords();

    if (failures == 0)
        std::printf("route_triage_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
