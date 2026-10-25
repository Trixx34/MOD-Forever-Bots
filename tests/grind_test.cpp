// Unit tests for grind spots: the file parser, the spot choice, and an idle bot walking to a spot. No core needed.
#include "bot_policy.h"
#include "grind_data.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotSnapshot;
using forever_module::WorldTickSnapshotView;

namespace {

// The pull and combat tests were written for bots that start Aggressive; the defensive default has its own test.
PolicyOptions AggressiveOptions()
{
    PolicyOptions options;
    options.AggressiveStart = true;
    return options;
}

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

// A walk longer than the mount threshold spends its first tick mounting up: kMount, then kStop instead of the walk. The walk to the
// spot follows on the next tick. BotMountsForALongWalk in policy_test covers the mount itself.
bool WalksToAfterMount(BotPolicy& policy, WorldTickSnapshotView const& view, std::uint64_t nowMs, float x)
{
    policy.Decide(view, nowMs);
    return WalksTo(policy.Decide(view, nowMs + 1), x);
}

void IdleBotWalksToAGrindSpotForItsLevel()
{
    std::vector<std::string> warnings;
    PolicyOptions options = AggressiveOptions();
    options.Grind = std::make_shared<GrindData>(Parse("spot 0 500 0 0 1 5 0\nspot 0 900 0 0 6 10 0\n", warnings));
    BotPolicy policy(options);
    WorldTickSnapshotView view = OneBot(7, 0);
    CHECK(!WalksTo(policy.Decide(view, 1000), 900.0f));      // idle for a moment only
    CHECK(WalksToAfterMount(policy, view, 1000 + 31000, 900.0f)); // a quiet spell: to the spot for level 7
}

// With randomness on, bots of one level do not all take the nearest spot after exactly the same quiet spell, and they do not all stand on the exact point.
void RandomnessSpreadsTheBotsOverSpotsAndTimes()
{
    std::vector<std::string> warnings;
    PolicyOptions options = AggressiveOptions();
    options.Grind = std::make_shared<GrindData>(Parse("spot 0 500 0 0 1 10 0\nspot 0 600 0 0 1 10 0\nspot 0 700 0 0 1 10 0\nspot 0 800 0 0 1 10 0\n", warnings));
    options.RandomPct = 25;
    options.RandomSeed = 42;
    BotPolicy policy(options);

    WorldTickSnapshotView view;
    view.tick_number = 1;
    view.max_nearby_units = 2;
    for (std::uint64_t guid = 1; guid <= 40; ++guid)
    {
        BotSnapshot bot;
        bot.guid = guid;
        bot.health = 1000;
        bot.max_health = 1000;
        bot.level = 5;
        view.bots.push_back(bot);
    }
    view.nearby.resize(2 * 40);

    // Every bot is idle from the first tick; note when each one first sets off (a mount, then a walk) and where to.
    policy.Decide(view, 1000);
    std::map<std::uint64_t, std::uint64_t> setOffAt;
    std::set<int> spots;
    bool anyExactPoint = false, allWithinSix = true;
    for (std::uint64_t now = 1000 + 5000; now <= 1000 + 80000; now += 500)
        for (BotAction const& action : policy.Decide(view, now))
        {
            if ((action.type == ActionType::kMount || action.type == ActionType::kMoveTo) && !setOffAt.count(action.bot_guid))
                setOffAt[action.bot_guid] = now;
            if (action.type == ActionType::kMoveTo)
            {
                float const nearest = std::floor((action.dest_x + 50.0f) / 100.0f) * 100.0f;   // the spot this point belongs to (500, 600, 700 or 800)
                spots.insert(int(nearest));
                if (action.dest_x == nearest && action.dest_y == 0.0f)
                    anyExactPoint = true;
                float const dx = action.dest_x - nearest, dy = action.dest_y;
                if (dx * dx + dy * dy > 6.01f * 6.01f)
                    allWithinSix = false;
            }
        }
    std::set<std::uint64_t> times;
    for (auto const& [bot, at] : setOffAt)
        times.insert(at);
    CHECK(setOffAt.size() == 40);        // they all set off within the 80 s
    CHECK(times.size() >= 8);            // at many different moments, not all at 31 s
    CHECK(spots.size() >= 3);            // to different spots, not all the nearest
    CHECK(!anyExactPoint && allWithinSix); // standing near the spot, not on its exact point
}

// A map the spot file does not cover: the bot used to stand at its start for the whole run (the new races' island). It now takes short walks
// on foot, and looks for spots again after a minute.
void NoSpotForTheMapMeansExploringOnFoot()
{
    std::vector<std::string> warnings;
    PolicyOptions options = AggressiveOptions();
    options.Grind = std::make_shared<GrindData>(Parse("spot 1 500 0 0 1 5 0\n", warnings));
    BotPolicy policy(options);
    WorldTickSnapshotView view = OneBot(3, 0);
    policy.Decide(view, 1000);
    bool walked = false;
    std::uint64_t now = 1000 + 31000;
    for (int i = 0; i < 40 && !walked; ++i, now += 3000)    // the lookup fails once, then the exploring starts after a short quiet
        for (BotAction const& action : policy.Decide(view, now))
            if (action.type == ActionType::kMoveTo)
            {
                walked = true;
                float const dx = action.dest_x - view.bots[0].pos_x, dy = action.dest_y - view.bots[0].pos_y;
                CHECK(dx * dx + dy * dy >= 49.0f * 49.0f && dx * dx + dy * dy <= 101.0f * 101.0f);   // 50 to 100 yards
            }
    CHECK(walked);
}

void HubsAreTheFallbackWithoutGrindData()
{
    PolicyOptions options = AggressiveOptions();
    options.HubsByMap[0] = { Point{ 700.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    WorldTickSnapshotView view = OneBot(3, 0);
    policy.Decide(view, 1000);
    CHECK(WalksToAfterMount(policy, view, 1000 + 31000, 700.0f));
}

void NextWalkMovesOnToAnotherSpot()
{
    std::vector<std::string> warnings;
    PolicyOptions options = AggressiveOptions();
    options.Grind = std::make_shared<GrindData>(Parse("spot 0 100 0 0 1 5 0\nspot 0 400 0 0 1 5 0\n", warnings));
    BotPolicy policy(options);
    WorldTickSnapshotView view = OneBot(3, 0);
    policy.Decide(view, 1000);
    CHECK(WalksTo(policy.Decide(view, 1000 + 31000), 100.0f));       // the nearest first
    view.bots[0].pos_x = 100.0f;                                     // arrived
    policy.Decide(view, 40000);
    CHECK(WalksToAfterMount(policy, view, 40000 + 31000, 400.0f)); // then the other one, not back to the same
}

// A start zone whose creatures the bot will not pull (an elite, a level far above it, a pack of four) used to count as "an enemy near": the bot
// was never idle, so it never walked to a grind spot and stood there for hours.
void UnpullableCreaturesDoNotKeepTheBotFromWalking()
{
    std::vector<std::string> warnings;
    PolicyOptions options = AggressiveOptions();
    options.Grind = std::make_shared<GrindData>(Parse("spot 0 900 0 0 1 5 0\n", warnings));
    BotPolicy policy(options);
    WorldTickSnapshotView view = OneBot(1, 0);
    view.max_nearby_units = 8;
    view.nearby.assign(8, forever_module::NearbyUnit{});
    view.bots[0].nearby_count = 5;
    for (int i = 0; i < 4; ++i)                                   // a pack of four, close together
    {
        forever_module::NearbyUnit& unit = view.nearby[i];
        unit.guid = forever_module::MakeUnitRef(8, 100, i + 1);
        unit.pos_x = 30.0f + float(i);
        unit.health = unit.max_health = 50;
        unit.is_hostile = true;
        unit.level = 1;
    }
    forever_module::NearbyUnit& elite = view.nearby[4];                           // and an elite on its own
    elite.guid = forever_module::MakeUnitRef(8, 200, 1);
    elite.pos_x = -40.0f;
    elite.health = elite.max_health = 500;
    elite.is_hostile = true;
    elite.level = 2;
    elite.elite = 1;
    policy.Decide(view, 1000);
    CHECK(WalksToAfterMount(policy, view, 1000 + 31000, 900.0f));
}

// Bots standing together (a start zone) used to follow the nearest of each other forever, in circles. Another bot is not a player to follow: the
// bot walks to a grind spot. A real player (not one of the controlled bots, and not an invite) is not followed either: it walks to the grind
// spot just the same (fix/random-bot-no-auto-follow).
void BotsDoNotFollowEachOther()
{
    std::vector<std::string> warnings;
    PolicyOptions options = AggressiveOptions();
    options.Grind = std::make_shared<GrindData>(Parse("spot 0 900 0 0 1 5 0\n", warnings));
    BotPolicy policy(options);
    WorldTickSnapshotView view = OneBot(1, 0);
    view.max_nearby_units = 2;
    view.nearby.assign(2, forever_module::NearbyUnit{});
    BotSnapshot second = view.bots[0];
    second.guid = 8;
    second.pos_x = 3.0f;
    view.bots.push_back(second);
    view.bots[0].nearby_count = 1;
    view.bots[1].nearby_count = 1;
    view.nearby.resize(4);
    view.nearby[0].guid = forever_module::MakeUnitRef(2, 0, 8);      // bot 7 sees bot 8
    view.nearby[0].pos_x = 3.0f;
    view.nearby[0].health = view.nearby[0].max_health = 1000;
    view.nearby[2].guid = forever_module::MakeUnitRef(2, 0, 7);      // bot 8 sees bot 7
    view.nearby[2].health = view.nearby[2].max_health = 1000;
    policy.Decide(view, 1000);
    bool followed = false;
    bool walked = false;
    for (std::uint64_t now = 2000; now <= 40000; now += 1000)
        for (BotAction const& action : policy.Decide(view, now))
        {
            followed = followed || action.type == ActionType::kFollow;
            walked = walked || (action.type == ActionType::kMoveTo && action.dest_x == 900.0f);
        }
    CHECK(!followed);
    CHECK(walked);

    // A real player (not controlled by the module, and not an invite) in view is not followed either.
    WorldTickSnapshotView withPlayer = OneBot(1, 0);
    withPlayer.max_nearby_units = 2;
    withPlayer.nearby.assign(2, forever_module::NearbyUnit{});
    withPlayer.bots[0].nearby_count = 1;
    withPlayer.nearby[0].guid = forever_module::MakeUnitRef(2, 0, 999);
    withPlayer.nearby[0].pos_x = 20.0f;
    withPlayer.nearby[0].health = withPlayer.nearby[0].max_health = 1000;
    BotPolicy other(options);
    bool followsPlayer = false;
    for (std::uint64_t now = 1000; now <= 5000; now += 1000)
        for (BotAction const& action : other.Decide(withPlayer, now))
            followsPlayer = followsPlayer || action.type == ActionType::kFollow;
    CHECK(!followsPlayer);
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
    RandomnessSpreadsTheBotsOverSpotsAndTimes();
    NoSpotForTheMapMeansExploringOnFoot();
    HubsAreTheFallbackWithoutGrindData();
    NextWalkMovesOnToAnotherSpot();
    UnpullableCreaturesDoNotKeepTheBotFromWalking();
    BotsDoNotFollowEachOther();
    if (failures == 0)
        std::printf("grind_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
