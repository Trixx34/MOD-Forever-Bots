// Unit tests for the flight decision (src/taxi_policy.h). No core or SDK needed.
#include "taxi_policy.h"

#include <cstdio>

using namespace taxi;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

Settings On()
{
    Settings settings;
    settings.Enabled = true;
    return settings;
}

// A bot at the origin with a flight master 100 yards east.
Facts Bot()
{
    Facts facts;
    facts.MasterValid = true;
    facts.MasterX = 100.0f;
    return facts;
}

void ALongWalkWithAMasterOnTheWayGoesToTheMaster()
{
    CHECK(Decide(On(), Bot(), 3000.0f, 0.0f, 0, 0, 0) == Move::WalkToMaster);
    Facts there = Bot();
    there.X = 98.0f;                                                        // 2 yards from the master
    CHECK(Decide(On(), there, 3000.0f, 0.0f, 0, 0, 0) == Move::Fly);
}

void ShortWalksAreWalked()
{
    CHECK(Decide(On(), Bot(), 1499.0f, 0.0f, 0, 0, 0) == Move::Walk);
    CHECK(Decide(On(), Bot(), 1500.0f, 0.0f, 0, 0, 0) == Move::WalkToMaster);
    Settings far = On();
    far.MinWalkYards = 3000.0f;
    CHECK(Decide(far, Bot(), 2500.0f, 0.0f, 0, 0, 0) == Move::Walk);
}

void AMasterThatIsFarBehindOrWrongIsNotWalkedTo()
{
    Facts behind = Bot();
    behind.MasterX = -400.0f;                                               // behind the bot, the destination is east
    CHECK(Decide(On(), behind, 3000.0f, 0.0f, 0, 0, 0) == Move::Walk);
    Facts far = Bot();
    far.MasterX = 650.0f;                                                   // on the way but past MasterMaxYards
    CHECK(Decide(On(), far, 5000.0f, 0.0f, 0, 0, 0) == Move::Walk);
    Facts sideways = Bot();
    sideways.MasterX = 0.0f;
    sideways.MasterY = 550.0f;                                              // beside the bot: farther from a destination 1600 yards east than the bot is
    CHECK(Decide(On(), sideways, 1600.0f, 0.0f, 0, 0, 0) == Move::Walk);
    Facts ahead = Bot();
    ahead.MasterX = 500.0f;
    ahead.MasterY = 300.0f;                                                 // 583 yards off, but ahead of the bot
    CHECK(Decide(On(), ahead, 3000.0f, 0.0f, 0, 0, 0) == Move::WalkToMaster);
    CHECK(Decide(On(), ahead, 1100.0f, 0.0f, 0, 0, 0) == Move::Walk);       // a walk of 1100 is short, whatever is on the way
    Facts half = Bot();
    half.MasterX = 700.0f;                                                  // 700 yards: past MasterMaxYards
    CHECK(Decide(On(), half, 1400.0f + 600.0f, 0.0f, 0, 0, 0) == Move::Walk);
    Facts notAMaster = Bot();
    notAMaster.MasterValid = false;
    CHECK(Decide(On(), notAMaster, 3000.0f, 0.0f, 0, 0, 0) == Move::Walk);
}

void NothingHappensWhenOffOrBusyOrBlocked()
{
    CHECK(Decide(Settings{}, Bot(), 3000.0f, 0.0f, 0, 0, 0) == Move::Walk);           // off by default
    Facts fighting = Bot();
    fighting.InCombat = true;
    CHECK(Decide(On(), fighting, 3000.0f, 0.0f, 0, 0, 0) == Move::Walk);
    Facts dead = Bot();
    dead.Alive = false;
    CHECK(Decide(On(), dead, 3000.0f, 0.0f, 0, 0, 0) == Move::Walk);
    CHECK(Decide(On(), Bot(), 3000.0f, 0.0f, 1000, 600000, 0) == Move::Walk);        // a refused flight: walk until the block ends
    CHECK(Decide(On(), Bot(), 3000.0f, 0.0f, 600000, 600000, 0) == Move::WalkToMaster);
}

void AFlightInProgressOrJustOrderedWaits()
{
    Facts flying = Bot();
    flying.InFlight = true;
    CHECK(Decide(On(), flying, 3000.0f, 0.0f, 0, 0, 0) == Move::Wait);
    CHECK(Decide(On(), Bot(), 3000.0f, 0.0f, 1000, 0, 5000) == Move::Wait);          // the order was sent a moment ago
    CHECK(Decide(On(), Bot(), 3000.0f, 0.0f, 5000, 0, 5000) == Move::WalkToMaster);
    flying.MasterValid = false;
    CHECK(Decide(On(), flying, 3000.0f, 0.0f, 0, 0, 0) == Move::Wait);               // in the air, whatever else
}

} // namespace

int main()
{
    ALongWalkWithAMasterOnTheWayGoesToTheMaster();
    ShortWalksAreWalked();
    AMasterThatIsFarBehindOrWrongIsNotWalkedTo();
    NothingHappensWhenOffOrBusyOrBlocked();
    AFlightInProgressOrJustOrderedWaits();
    if (failures == 0)
        std::printf("taxi_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
