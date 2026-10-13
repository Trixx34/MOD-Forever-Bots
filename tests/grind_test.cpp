// Unit tests for grind spots: the file parser, the spot choice, and an idle bot walking to a spot. No core needed.
#include "bot_policy.h"
#include "grind_data.h"

#include <cstdio>
#include <sstream>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotSnapshot;
using forever_module::WorldTickSnapshotView;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

GrindData Parse(char const* text, std::vector<std::string>& warnings)
{
    std::istringstream in(text);
    return ParseGrindData(in, warnings);
}

void ParsesSpotsAndRejectsBadLines()
{
    std::vector<std::string> warnings;
    GrindData const data = Parse(
        "# comment\n"
        "spot 0 -9000.5 -100 50 1 5 1 12   # Elwynn\n"
        "spot 1 300 -4000 20 3 8 2\n"
        "spot 0 1 2 3 9 4 0\n"       // min above max
        "spot 0 1 2 3 1 5 3\n"       // side 3
        "spot 0 x 2 3 1 5 0\n"       // not a number
        "hello\n",
        warnings);
    CHECK(data.Spots.size() == 2);
    CHECK(warnings.size() == 4);
    CHECK(data.Spots[0].Map == 0 && data.Spots[0].X == -9000.5f && data.Spots[0].MinLevel == 1 && data.Spots[0].MaxLevel == 5 &&
          data.Spots[0].Side == 1 && data.Spots[0].Weight == 12);
    CHECK(data.Spots[1].Side == 2 && data.Spots[1].Weight == 1);
}

void ChoosesByLevelSideAndDistance()
{
    std::vector<std::string> warnings;
    GrindData const data = Parse(
        "spot 0 0 0 0 1 5 0\n"          // 0: low level, both
        "spot 0 100 0 0 1 5 0\n"        // 1: low level, both, farther
        "spot 0 10 0 0 6 10 0\n"        // 2: level 6 to 10
        "spot 0 5 0 0 1 5 2\n"          // 3: Horde only
        "spot 1 0 0 0 1 5 0\n",         // 4: another map
        warnings);
    auto spots = GrindSpotsFor(data, 0, 3, 1, 20.0f, 0.0f);
    CHECK(spots.size() == 2 && spots[0] == 0 && spots[1] == 1); // nearest first; the Horde spot and the other map are out
    spots = GrindSpotsFor(data, 0, 3, 2, 20.0f, 0.0f);
    CHECK(spots.size() == 3 && spots[0] == 3);                  // a Horde bot may use the Horde spot
    spots = GrindSpotsFor(data, 0, 8, 1, 0.0f, 0.0f);
    CHECK(spots.size() == 1 && spots[0] == 2);
    spots = GrindSpotsFor(data, 0, 40, 1, 0.0f, 0.0f);          // over-leveled: the best there is (top level 10)
    CHECK(spots.size() == 1 && spots[0] == 2);
    CHECK(GrindSpotsFor(data, 5, 3, 1, 0.0f, 0.0f).empty());    // no spots on that map
}

void SkipsTheSpotsJustVisited()
{
    std::vector<std::string> warnings;
    GrindData const data = Parse("spot 0 0 0 0 1 5 0\nspot 0 50 0 0 1 5 0\nspot 0 90 0 0 1 5 0\n", warnings);
    auto spots = GrindSpotsFor(data, 0, 3, 0, 0.0f, 0.0f, { 0 });
    CHECK(spots.size() == 2 && spots[0] == 1);
    spots = GrindSpotsFor(data, 0, 3, 0, 0.0f, 0.0f, { 0, 1, 2 }); // everything visited: all are allowed again
    CHECK(spots.size() == 3);
}

void RichSpotsCountAsCloser()
{
    std::vector<std::string> warnings;
    GrindData const data = Parse("spot 0 100 0 0 1 5 0 1\nspot 0 130 0 0 1 5 0 500\n", warnings);
    auto const spots = GrindSpotsFor(data, 0, 3, 0, 0.0f, 0.0f);
    CHECK(spots.size() == 2 && spots[0] == 1);
}

void SidesOfRaces()
{
    CHECK(SideOfRace(1) == 1 && SideOfRace(4) == 1);
    CHECK(SideOfRace(2) == 2 && SideOfRace(5) == 2 && SideOfRace(6) == 2);
    CHECK(SideOfRace(0) == 0);
}

WorldTickSnapshotView OneBot(std::uint32_t level, std::uint32_t map)
{
    WorldTickSnapshotView view;
    view.tick_number = 1;
    view.max_nearby_units = 2;
    BotSnapshot bot;
    bot.guid = 7;
    bot.health = 1000;
    bot.max_health = 1000;
    bot.level = level;
    bot.map_id = map;
    view.bots.push_back(bot);
    view.nearby.resize(2);
    return view;
}

bool WalksTo(std::vector<BotAction> const& actions, float x)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.dest_x == x)
            return true;
    return false;
}

void IdleBotWalksToAGrindSpotForItsLevel()
{
    std::vector<std::string> warnings;
    PolicyOptions options;
    options.Grind = std::make_shared<GrindData>(Parse("spot 0 500 0 0 1 5 0\nspot 0 900 0 0 6 10 0\n", warnings));
    BotPolicy policy(options);
    WorldTickSnapshotView view = OneBot(7, 0);
    CHECK(!WalksTo(policy.Decide(view, 1000), 900.0f));      // idle for a moment only
    CHECK(WalksTo(policy.Decide(view, 1000 + 31000), 900.0f)); // a quiet spell: to the spot for level 7
}

void NoSpotForTheMapMeansNoWalk()
{
    std::vector<std::string> warnings;
    PolicyOptions options;
    options.Grind = std::make_shared<GrindData>(Parse("spot 1 500 0 0 1 5 0\n", warnings));
    BotPolicy policy(options);
    WorldTickSnapshotView view = OneBot(3, 0);
    policy.Decide(view, 1000);
    for (BotAction const& action : policy.Decide(view, 1000 + 31000))
        CHECK(action.type != ActionType::kMoveTo);
}

void HubsAreTheFallbackWithoutGrindData()
{
    PolicyOptions options;
    options.HubsByMap[0] = { Point{ 700.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    WorldTickSnapshotView view = OneBot(3, 0);
    policy.Decide(view, 1000);
    CHECK(WalksTo(policy.Decide(view, 1000 + 31000), 700.0f));
}

void NextWalkMovesOnToAnotherSpot()
{
    std::vector<std::string> warnings;
    PolicyOptions options;
    options.Grind = std::make_shared<GrindData>(Parse("spot 0 100 0 0 1 5 0\nspot 0 400 0 0 1 5 0\n", warnings));
    BotPolicy policy(options);
    WorldTickSnapshotView view = OneBot(3, 0);
    policy.Decide(view, 1000);
    CHECK(WalksTo(policy.Decide(view, 1000 + 31000), 100.0f));       // the nearest first
    view.bots[0].pos_x = 100.0f;                                     // arrived
    policy.Decide(view, 40000);
    CHECK(WalksTo(policy.Decide(view, 40000 + 31000), 400.0f));      // then the other one, not back to the same
}

} // namespace

int main()
{
    ParsesSpotsAndRejectsBadLines();
    ChoosesByLevelSideAndDistance();
    SkipsTheSpotsJustVisited();
    RichSpotsCountAsCloser();
    SidesOfRaces();
    IdleBotWalksToAGrindSpotForItsLevel();
    NoSpotForTheMapMeansNoWalk();
    HubsAreTheFallbackWithoutGrindData();
    NextWalkMovesOnToAnotherSpot();
    if (failures == 0)
        std::printf("grind_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
