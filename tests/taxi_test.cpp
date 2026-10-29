// Unit tests for the flight decision (src/taxi_policy.h). No core or SDK needed.
#include "taxi_policy.h"
#include "errand_trainers.h"

#include <cstdio>
#include <string>
#include <vector>

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

void TheFlightIsTrackedFromOrderToLanding()
{
    Flight f{ 1000, 0, 100.0f, 200.0f, 77 };
    CHECK(Track(f, false, 2000, 8000) == Progress::None);                            // ordered, not in the air yet
    CHECK(Track(f, true, 3000, 8000) == Progress::None && f.TookOffMs == 3000);
    CHECK(Track(f, true, 60000, 8000) == Progress::None);                            // a long flight is not a timeout
    CHECK(Track(f, false, 90000, 8000) == Progress::Landed);
    Flight none;
    CHECK(Track(none, false, 90000, 8000) == Progress::None);
}

void AFlightThatNeverStartsOrNeverEndsTimesOut()
{
    Flight f{ 1000, 0, 0.0f, 0.0f, 1 };
    CHECK(Track(f, false, 8999, 8000) == Progress::None);
    CHECK(Track(f, false, 9000, 8000) == Progress::NeverStarted);
    Flight g{ 1000, 0, 0.0f, 0.0f, 1 };
    CHECK(Track(g, true, 2000, 8000, 10000) == Progress::None);
    CHECK(Track(g, true, 12000, 8000, 10000) == Progress::TooLong);
}

void TheLinesKeepTheOldKeysAndAppendTheNew()
{
    Flight f{ 1000, 3000, 1234.9f, -50.2f, 42 };
    CHECK(OrderLine(5, 30, 1, 2500, f) == "taxi: bot=5 level=30 map=1 walk=2500 to_x=1234 to_y=-50 master=42 city=unknown");
    CHECK(OrderLine(5, 30, 1, 2500, f).rfind("taxi: bot=5 level=30 map=1 walk=2500", 0) == 0);
    f.Place = "Orgrimmar";
    CHECK(OrderLine(5, 30, 1, 2500, f) == "taxi: bot=5 level=30 map=1 walk=2500 to_x=1234 to_y=-50 master=42 city=Orgrimmar");
    CHECK(LandedLine(5, 1, 10.0f, 20.0f, f, 91000) == "taxi_landed: bot=5 map=1 x=10 y=20 to_x=1234 to_y=-50 took_s=90 city=Orgrimmar");
    CHECK(TimeoutLine(5, 1, Progress::NeverStarted, f, 9000) == "taxi_timeout: bot=5 map=1 reason=not_started waited_s=8 city=Orgrimmar");
    CHECK(TimeoutLine(5, 1, Progress::TooLong, f, 1801000) == "taxi_timeout: bot=5 map=1 reason=too_long waited_s=1800 city=Orgrimmar");
    CHECK(RefusedLine(5, 1, "refused") == "taxi_refused: bot=5 map=1 status=refused");
    CHECK(Seconds(5000, 1000) == 0);
}

void ADestinationIsNamedAfterTheNearestCityOnItsMap()
{
    std::vector<errand::Trainer> trainers;
    auto add = [&](std::uint32_t city, std::uint32_t map, float x, float y) {
        errand::Trainer t;
        t.City = city;
        t.Map = map;
        t.X = x;
        t.Y = y;
        trainers.push_back(t);
    };
    add(errand::kOrgrimmar, 1, 1600.0f, -4400.0f);
    add(errand::kOrgrimmar, 1, 1800.0f, -4400.0f);                          // Orgrimmar centre: (1700, -4400)
    add(errand::kStormwind, 0, -8800.0f, 600.0f);
    std::vector<Place> const places = errand::CityPlaces(trainers);
    CHECK(places.size() == 2);
    CHECK(PlaceName(places, 1, 1700.0f, -4400.0f) == "Orgrimmar");
    CHECK(PlaceName(places, 1, 2400.0f, -4400.0f) == "Orgrimmar");           // 700 yards out: still the city
    CHECK(PlaceName(places, 1, 2600.0f, -4400.0f) == "unknown");             // 900 yards out
    CHECK(PlaceName(places, 0, 1700.0f, -4400.0f) == "unknown");             // right place, wrong map
    CHECK(PlaceName(places, 0, -8700.0f, 650.0f) == "Stormwind");
    CHECK(PlaceName({}, 1, 0.0f, 0.0f) == "unknown");
    std::vector<Place> two = { Place{ "A", 1, 0.0f, 0.0f }, Place{ "B", 1, 500.0f, 0.0f } };
    CHECK(PlaceName(two, 1, 300.0f, 0.0f) == "B");                          // the nearer of two
}

} // namespace

int main()
{
    ALongWalkWithAMasterOnTheWayGoesToTheMaster();
    ShortWalksAreWalked();
    AMasterThatIsFarBehindOrWrongIsNotWalkedTo();
    NothingHappensWhenOffOrBusyOrBlocked();
    AFlightInProgressOrJustOrderedWaits();
    TheFlightIsTrackedFromOrderToLanding();
    AFlightThatNeverStartsOrNeverEndsTimesOut();
    TheLinesKeepTheOldKeysAndAppendTheNew();
    ADestinationIsNamedAfterTheNearestCityOnItsMap();
    if (failures == 0)
        std::printf("taxi_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
