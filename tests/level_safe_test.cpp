// SPDX-License-Identifier: GPL-2.0-or-later
// Tests of the level-safe places and the extras of 2026-11-18 (branch feature/level-safe-zone-move-20261118, doc/level-safe.md): the place levels from the grind
// spots, the graveyards seen, the deaths_top line, the below-map check, the auction 3D reach, and the policy's quest walk check, level-fitting graveyard and
// escape. Hand-built snapshots; the map is a line: a low zone (levels 1-6) around x = 0..300, a high one (levels 23-27) around x = 2000.
#include "policy_test_support.h"
#include "bot_settings.h"
#include "auction_policy.h"
#include "below_map.h"
#include "level_safe.h"

#include <cstdio>
#include <sstream>

namespace {

std::shared_ptr<GrindData> Spots()
{
    std::vector<std::string> warnings;
    std::istringstream spots("spot 0 0 0 0 1 6 0 10\n"
                             "spot 0 100 0 0 2 6 0 10\n"
                             "spot 0 300 0 0 1 6 0 10\n"
                             "spot 0 400 0 0 3 6 0 10\n"
                             "spot 0 2000 0 0 24 26 0 10\n"
                             "spot 0 2100 0 0 23 27 0 10\n"
                             "spot 0 2050 120 0 50 52 0 9\n");
    return std::make_shared<GrindData>(ParseGrindData(spots, warnings));
}

level_safe::LevelMap Levels()
{
    level_safe::LevelMap levels;
    levels.Build(*Spots(), 250.0f);
    return levels;
}

void PlaceLevelsComeFromTheNearSpots()
{
    level_safe::LevelMap const levels = Levels();
    CHECK(levels.PlaceMin(0, 0.0f, 0.0f) == 1);
    CHECK(levels.PlaceMin(0, 2000.0f, 50.0f) == 23);    // the lowest of the near spots, not the level 50 one beside them
    CHECK(levels.PlaceMin(0, 1000.0f, 0.0f) == 0);      // no spot within 250 yards: unknown
    CHECK(levels.PlaceMin(1, 0.0f, 0.0f) == 0);         // another map
    CHECK(levels.PlaceMin(0, 1780.0f, 0.0f) == 0);      // one spot only within 250 yards (a town's guards): unknown
    CHECK(levels.TooHigh(0, 2000.0f, 0.0f, 6, 5));       // 23 > 6 + 5
    CHECK(!levels.TooHigh(0, 2000.0f, 0.0f, 20, 5));     // 23 <= 25
    CHECK(!levels.TooHigh(0, 2000.0f, 0.0f, 0, 5));      // level unknown: never judged
    CHECK(!levels.TooHigh(0, 1000.0f, 0.0f, 1, 5));      // place unknown: never judged
    CHECK(levels.Fits(0, 300.0f, 0.0f, 6, 5) && !levels.Fits(0, 2000.0f, 0.0f, 6, 5) && !levels.Fits(0, 1000.0f, 0.0f, 6, 5));
}

void AWalkIsCheckedAlongItsLineAndAtItsEnd()
{
    level_safe::LevelMap const levels = Levels();
    CHECK(!levels.FirstTooHigh(0, 0.0f, 0.0f, 300.0f, 0.0f, 6, 5, 100.0f, 300.0f));         // low to low
    auto const toHigh = levels.FirstTooHigh(0, 0.0f, 0.0f, 2050.0f, 0.0f, 6, 5, 100.0f, 300.0f);
    CHECK(toHigh && toHigh->PlaceMin == 23 && toHigh->X == 1900.0f);                         // the first sample with two high spots within 250 yards
    CHECK(levels.FirstTooHigh(0, 0.0f, 0.0f, 4000.0f, 0.0f, 6, 5, 100.0f, 300.0f));          // the end is unknown, the way passes the high zone
    CHECK(!levels.FirstTooHigh(0, 0.0f, 0.0f, 4000.0f, 0.0f, 20, 5, 100.0f, 300.0f));        // a level 20 bot may pass it
    CHECK(!levels.FirstTooHigh(0, 2000.0f, 0.0f, 0.0f, 0.0f, 6, 5, 100.0f, 300.0f));         // out of a high place: the first 300 yards are not judged
    CHECK(levels.FirstTooHigh(0, 1800.0f, 300.0f, 2000.0f, 0.0f, 6, 5, 100.0f, 300.0f));      // a short walk: its end is judged all the same
}

void GraveyardsAreRememberedBySideAndChosenByLevel()
{
    level_safe::LevelMap const levels = Levels();
    level_safe::Graveyards yards;
    yards.Note(0, 50.0f, 0.0f, 0.0f, 1);
    yards.Note(0, 60.0f, 10.0f, 0.0f, 2);      // the same graveyard, seen by a Horde ghost
    yards.Note(0, 2050.0f, 0.0f, 0.0f, 1);     // Raven Hill
    CHECK(yards.Size() == 2);
    auto const* low = yards.Fitting(levels, 0, 2000.0f, 0.0f, 6, 5, 1);
    CHECK(low && low->X == 50.0f && low->Sides == 3);
    auto const* high = yards.Fitting(levels, 0, 2000.0f, 0.0f, 25, 5, 1);
    CHECK(high && high->X == 2050.0f);                                   // a level 25 bot may use the near one
    auto const* horde = yards.Fitting(levels, 0, 2000.0f, 0.0f, 25, 5, 2);
    CHECK(horde && horde->X == 50.0f);                                   // the Horde never appeared at Raven Hill
    CHECK(yards.Fitting(levels, 1, 0.0f, 0.0f, 6, 5, 1) == nullptr);      // no graveyard seen on that map
    level_safe::Graveyards highOnly;
    highOnly.Note(0, 2050.0f, 0.0f, 0.0f, 1);
    CHECK(highOnly.Fitting(levels, 0, 2000.0f, 0.0f, 6, 5, 1) == nullptr);
}

void TheDeathsTopLineCountsAndStartsAgain()
{
    level_safe::DeathTally tally;
    for (int i = 0; i < 5; ++i)
        tally.Note(1);
    for (int i = 0; i < 3; ++i)
        tally.Note(2);
    tally.Note(3);
    CHECK(tally.Total() == 9);
    std::string const line = tally.TakeLine(5, 3600000);
    CHECK(line.rfind("deaths_top: window_min=60 deaths=9 bots_online=5 bots_died=3 median=1.0 median_died=3.0 top10=9 top10_pct=100.0 top=1:5,2:3,3:1", 0) == 0);
    CHECK(tally.Total() == 0);
    CHECK(tally.TakeLine(4, 60000).find("deaths=0 bots_online=4 bots_died=0 median=0.0") != std::string::npos);
    tally.Note(7);
    tally.Note(7);
    tally.Note(8);
    CHECK(tally.TakeLine(4, 60000).find("median=0.5 median_died=1.5") != std::string::npos);   // 0, 0, 1, 2
}

void BelowTheMapIsSeenByHeightOrByAStorm()
{
    below_map::Options options;
    options.Enabled = true;
    options.BelowChecks = 1;   // the time alone here; the count of checks in a row (2026-11-19) is tested in arm_c_fixes_test.cpp
    below_map::Watch watch;
    below_map::Facts facts;
    facts.HasGround = true;
    facts.GroundZ = 505.0f;
    facts.Places = 2;   // 2026-11-20: z_below needs MinPlaces (2) known places near
    facts.Z = 19.0f;   // the Ironforge auction house, 2026-11-18
    CHECK(below_map::Check(options, watch, facts, 1000) == below_map::Verdict::kWaiting);
    CHECK(below_map::Check(options, watch, facts, 20999) == below_map::Verdict::kWaiting);
    facts.InCombat = true;
    CHECK(below_map::Check(options, watch, facts, 21000) == below_map::Verdict::kBlocked);
    facts.InCombat = false;
    CHECK(below_map::Check(options, watch, facts, 21000) == below_map::Verdict::kSendBelow);
    CHECK(below_map::Check(options, watch, facts, 22000) == below_map::Verdict::kWaiting);    // a new spell starts
    CHECK(below_map::Check(options, watch, facts, 50000) == below_map::Verdict::kCooldown);
    facts.Z = 500.0f;
    CHECK(below_map::Check(options, watch, facts, 60000) == below_map::Verdict::kFine);       // on the floor
    // The storm: eight short walks refused within five minutes. Arrivals (under 6 yards) and long walks do not count. 2026-11-20: eight different
    // destinations, each 50 yards above the bot (BelowMapStormDistinct, BelowMapStormRiseYards).
    below_map::Watch storm;
    below_map::Facts open;
    below_map::NoteNoRoute(options, storm, 3.0f, 500.0f, 0.0f, 69.0f, 19.0f, 1000);
    below_map::NoteNoRoute(options, storm, 400.0f, 520.0f, 0.0f, 69.0f, 19.0f, 1000);
    for (int i = 0; i < 7; ++i)
        below_map::NoteNoRoute(options, storm, 40.0f, float(i) * 20.0f, 0.0f, 69.0f, 19.0f, 1000 + std::uint64_t(i) * 1000);
    CHECK(below_map::Check(options, storm, open, 9000) == below_map::Verdict::kFine);
    below_map::NoteNoRoute(options, storm, 40.0f, 200.0f, 0.0f, 69.0f, 19.0f, 9000);
    CHECK(below_map::Check(options, storm, open, 9500) == below_map::Verdict::kSendStorm);
    CHECK(storm.NoRoutes.empty());
    for (int i = 0; i < 8; ++i)
        below_map::NoteNoRoute(options, storm, 40.0f, float(i) * 20.0f, 0.0f, 69.0f, 19.0f, 10000);
    CHECK(below_map::Check(options, storm, open, 400000) == below_map::Verdict::kFine);     // the window ran out
    below_map::Options off;
    below_map::Watch none;
    below_map::NoteNoRoute(off, none, 40.0f, 0.0f, 0.0f, 69.0f, 19.0f, 1000);
    CHECK(none.NoRoutes.empty() && below_map::Check(off, none, facts, 1000) == below_map::Verdict::kFine);
}

void TheAuctionReachIsMeasuredIn3d()
{
    CHECK(auction::InReach(true, 0.0f, 0.0f, 3.0f, 0.0f));
    CHECK(auction::InReach3D(true, 0.0f, 0.0f, 505.0f, 3.0f, 0.0f, 505.0f));
    CHECK(!auction::InReach3D(true, 0.0f, 0.0f, 19.0f, 3.0f, 0.0f, 505.0f));   // under the floor: in 2D reach, not in 3D
    CHECK(!auction::InReach3D(false, 0.0f, 0.0f, 505.0f, 0.0f, 0.0f, 505.0f));
    CHECK(auction::ReachFields(0.0f, 0.0f, 19.0f, 3.0f, 4.0f, 505.0f) == " dist2d=5.0 dist3d=486.0 bot_z=19.0 auctioneer_z=505.0");
}

PolicyOptions LevelOptions(std::vector<std::string>* events)
{
    PolicyOptions options = AggressiveOptions();
    options.Grind = Spots();
    options.WanderWhenIdle = false;
    options.OnEvent = [events](std::string const& text) { events->push_back(text); };
    return options;
}

WorldTickSnapshotView Alive(float x, std::uint32_t level)
{
    WorldTickSnapshotView view = View(1, 0, {});
    view.bots[0].level = level;
    view.bots[0].pos_x = x;
    return view;
}

void ALowBotInAHighPlaceIsMovedOut()
{
    std::vector<std::string> events;
    PolicyOptions options = LevelOptions(&events);
    options.LevelSafe.Escape = true;
    BotPolicy policy(options);
    std::optional<BotAction> const away = TeleportOf(policy.Decide(Alive(2000.0f, 6), 1000), kBot);
    CHECK(away.has_value() && away->dest_x == 400.0f && away->spell_id == 0);   // the nearest spot for level 6
    CHECK(Logged(events, "level_unsafe_escape: bot=42 level=6 map=0 from=2000,0 place_min=23 spot=3 to=400,0"));
    CHECK(!TeleportOf(policy.Decide(Alive(2000.0f, 6), 2000), kBot));                // the cooldown
    // A level 20 bot belongs there; a fighting one cannot be teleported (the core refuses it) and is only logged.
    std::vector<std::string> other;
    BotPolicy second(LevelOptions(&other));
    PolicyOptions on = LevelOptions(&other);
    on.LevelSafe.Escape = true;
    BotPolicy levelled(on);
    CHECK(!TeleportOf(levelled.Decide(Alive(2000.0f, 20), 1000), kBot));
    WorldTickSnapshotView fighting = Alive(2000.0f, 6);
    fighting.bots[0].in_combat = 1;
    CHECK(!TeleportOf(levelled.Decide(fighting, 7000), kBot));                       // (a fitting place is looked at again after 5 s)
    CHECK(LoggedWith(other, "result=in_combat"));
    CHECK(!TeleportOf(second.Decide(Alive(2000.0f, 6), 1000), kBot));                // the key off: as before
}

// Bot 13 of the 2026-11-18 sim: a level 6 bot dies in the high zone. Its ghost appears at the high graveyard; the corpse lies there too. It is taken to the low
// graveyard seen earlier and asks the spirit healer there, instead of reclaiming its corpse beside what killed it.
void AGhostIsRevivedAtAGraveyardThatFitsItsLevel()
{
    for (bool on : { true, false })
    {
        std::vector<std::string> events;
        PolicyOptions options = LevelOptions(&events);
        options.LevelSafe.Graveyard = on;
        options.SideOf = [](std::uint64_t) -> std::uint8_t { return 1; };
        BotPolicy policy(options);
        policy.Decide(Alive(0.0f, 6), 1000);
        WorldTickSnapshotView dead = Alive(0.0f, 6);
        Corpse(dead.bots[0], 10.0f);
        policy.Decide(dead, 2000);
        WorldTickSnapshotView ghost = Alive(0.0f, 6);
        Ghost(ghost.bots[0], 50.0f, 0, 10.0f);   // the low graveyard
        policy.Decide(ghost, 3000);
        CHECK(policy.GraveyardsKnown() == (on ? 1u : 0u));
        policy.Decide(Alive(50.0f, 6), 4000);
        policy.Decide(Alive(2000.0f, 6), 400000);
        WorldTickSnapshotView high = Alive(2000.0f, 6);
        Corpse(high.bots[0], 2000.0f);
        policy.Decide(high, 401000);
        WorldTickSnapshotView raven = Alive(2000.0f, 6);
        Ghost(raven.bots[0], 2050.0f, 0, 2000.0f);   // Raven Hill: the graveyard and the corpse both too high
        std::vector<BotAction> const turn = policy.Decide(raven, 402000);
        std::optional<BotAction> const teleport = TeleportOf(turn, kBot);
        if (on)
        {
            CHECK(teleport.has_value() && teleport->dest_x == 50.0f);
            CHECK(LoggedWith(events, "level_safe_yard: bot=42 level=6 map=0 graveyard=2050,0 graveyard_min=23 corpse_min=23 reason=corpse_high to=50,0"));
            CHECK(Logged(events, "ghost_fallback: bot=42 reason=level_unsafe"));
            CHECK(LoggedWith(events, "graveyard=level_safe to=50,0"));
            WorldTickSnapshotView there = Alive(0.0f, 6);
            Ghost(there.bots[0], 50.0f, 0, 2000.0f);
            bool asked = false;
            for (BotAction const& action : policy.Decide(there, 403000))
                asked = asked || (action.type == ActionType::kResurrect && action.spell_id == 2);
            CHECK(asked);
        }
        else
        {
            CHECK(!(teleport.has_value() && teleport->dest_x == 50.0f));
            CHECK(!Logged(events, "level_safe_yard:"));
        }
    }
}

// No fitting graveyard known yet: today's behaviour, and a line says so.
void WithNoFittingGraveyardTheGhostDoesAsBefore()
{
    std::vector<std::string> events;
    PolicyOptions options = LevelOptions(&events);
    options.LevelSafe.Graveyard = true;
    BotPolicy policy(options);
    policy.Decide(Alive(2000.0f, 6), 1000);
    WorldTickSnapshotView high = Alive(2000.0f, 6);
    Corpse(high.bots[0], 2000.0f);
    policy.Decide(high, 2000);
    WorldTickSnapshotView raven = Alive(2000.0f, 6);
    Ghost(raven.bots[0], 2050.0f, 0, 2000.0f);
    policy.Decide(raven, 3000);
    CHECK(LoggedWith(events, "result=none_known known=1"));
    CHECK(!Logged(events, "ghost_fallback: bot=42 reason=level_unsafe"));
}

constexpr char kQuests[] = "start test\n"
                           "anchor 0 100 0 0\n"
                           "quest 1 giver 2000 0 0 ender 2000 0 0\n"
                           "quest 2 giver 300 0 0 ender 300 0 0\n"
                           "quest 3 giver 0 0 0 ender 0 0 0\n"
                           "hunt 3 0 5 99\n"
                           "area 3 2000 0 0\n"
                           "area 3 300 0 0\n";

std::shared_ptr<QuestData const> QuestList()
{
    std::istringstream in(kQuests);
    std::vector<std::string> warnings;
    return std::make_shared<QuestData>(ParseQuestData(in, warnings));
}

// The quest walk to a giver in the high zone is dropped; the next quest, in the low zone, is taken.
void AQuestWalkIntoAHighPlaceIsDropped()
{
    for (bool on : { true, false })
    {
        std::vector<std::string> events;
        PolicyOptions options = LevelOptions(&events);
        options.Quests = QuestList();
        options.LevelSafe.Walks = on;
        BotPolicy policy(options);
        bool walkedHigh = false;
        for (std::uint64_t now = 1000; now < 8000; now += 1000)
            for (BotAction const& action : policy.Decide(Alive(0.0f, 6), now))
                walkedHigh = walkedHigh || (action.type == ActionType::kMoveTo && action.dest_x > 1000.0f);
        CHECK(walkedHigh == !on);
        CHECK(LoggedWith(events, "quest_walk_blocked: bot=42 map=0 quest=1 reason=level_unsafe kind=giver level=6 margin=5 place_min=23 at=1900,0 dest=2000,0") == on);
        CHECK(!LoggedWith(events, "quest=2 reason=level_unsafe"));
    }
}

// The planner's side: a hunt moves on to its next area, then the quest is blocked.
void TheBlockSkipsAreasFirst()
{
    QuestPlanner planner(QuestList());
    QuestMemory memory;
    planner.NoteLevelUnsafe(memory, 3, QuestStepKind::Hunt, 1000);
    CHECK(memory.AreaIndex[3] == 1 && memory.BlockedUntilMs.count(3) == 0);
    planner.NoteLevelUnsafe(memory, 3, QuestStepKind::Hunt, 2000);
    CHECK(memory.BlockedUntilMs[3] == 2000 + kLevelUnsafeBlockMs && memory.LevelUnsafeAreas.count(3) == 0);
    planner.NoteLevelUnsafe(memory, 1, QuestStepKind::WalkTo, 3000);
    CHECK(memory.BlockedUntilMs[1] == 3000 + kLevelUnsafeBlockMs);
}

void TheDeathsTopLineIsDueOncePerPeriod()
{
    std::vector<std::string> events;
    PolicyOptions options = LevelOptions(&events);
    options.DeathsTopEveryMs = 60000;
    BotPolicy policy(options);
    CHECK(!policy.TakeDeathsTopLine(1000));   // starts the clock
    policy.Decide(Alive(0.0f, 6), 1000);
    WorldTickSnapshotView dead = Alive(0.0f, 6);
    Corpse(dead.bots[0], 0.0f);
    policy.Decide(dead, 2000);
    CHECK(!policy.TakeDeathsTopLine(30000));
    std::optional<std::string> const line = policy.TakeDeathsTopLine(61000);
    CHECK(line && line->rfind("deaths_top: window_min=1 deaths=1 bots_online=1 bots_died=1 median=1.0", 0) == 0 && line->find("top=42:1") != std::string::npos);
    PolicyOptions offOptions = LevelOptions(&events);
    BotPolicy off(offOptions);
    CHECK(!off.TakeDeathsTopLine(1000) && !off.TakeDeathsTopLine(10000000));
}

void BelowTheMapThePolicyTeleportsUp()
{
    std::vector<std::string> events;
    std::vector<std::string> warnings;
    // 2026-11-20: the map must be known (10 places or more) and two places must lie near the bot (BelowMapMinPlaces).
    std::istringstream spots("spot 0 0 0 505 1 6 0 10\nspot 0 10 0 505 1 6 0 10\nspot 0 1000 0 0 1 6 0 10\nspot 0 1100 0 0 1 6 0 10\n"
                             "spot 0 1200 0 0 1 6 0 10\nspot 0 1300 0 0 1 6 0 10\nspot 0 1400 0 0 1 6 0 10\nspot 0 1500 0 0 1 6 0 10\n"
                             "spot 0 1600 0 0 1 6 0 10\nspot 0 1700 0 0 1 6 0 10\n");
    PolicyOptions options = LevelOptions(&events);
    options.Grind = std::make_shared<GrindData>(ParseGrindData(spots, warnings));
    options.BelowMap.Enabled = true;
    BotPolicy policy(options);
    WorldTickSnapshotView under = Alive(20.0f, 6);
    under.bots[0].pos_z = 19.0f;
    CHECK(!TeleportOf(policy.Decide(under, 1000), kBot));
    // Looked at every 2 s (CheckEveryMs); the drop holds 20 s and ten looks (BelowMapChecks): up at 21 s, the eleventh look.
    std::optional<BotAction> up;
    for (std::uint64_t now = 2000; now <= 22000 && !up; now += 1000)
    {
        up = TeleportOf(policy.Decide(under, now), kBot);
        CHECK(!up || now == 21000);
    }
    CHECK(up.has_value() && up->dest_z == 505.0f && up->dest_x == 0.0f);
    CHECK(Logged(events, "below_map_recover: bot=42 reason=z_below map=0 pos=20,0,19 ground_z=505 z=19 drop=486 places=2 floors=1 low_z=505 high_z=505 checks=11 "
                         "moving=0 to=0,0,505"));
    options.BelowMap.Enabled = false;
    BotPolicy off(options);
    CHECK(!TeleportOf(off.Decide(under, 1000), kBot) && !TeleportOf(off.Decide(under, 22000), kBot));
}

void TheSettingsKeysHaveTheirDefaultsAndBounds()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(defaults.LevelSafeWalks && defaults.LevelSafeGraveyard && defaults.LevelSafeEscape && defaults.LevelSafeMarginLevels == 5 && defaults.LevelSafeEscapeMarginLevels == 10 &&
          defaults.LevelSafeRadiusYards == 250 && defaults.LevelSafeEscapeCooldownSec == 600);
    CHECK(!defaults.BelowMapRecover && defaults.BelowMapDropYards == 60 && defaults.BelowMapStormCount == 8 && defaults.BelowMapCooldownSec == 600);
    CHECK(defaults.LogRefusedReason && defaults.DeathsTopEveryMin == 60 && !defaults.Auction.Walk3D && defaults.Auction.LogReach && warnings.empty());
    BotSettings const set = ParseBotSettings({ { "LevelSafeWalks", "0" }, { "LevelSafeGraveyard", "0" }, { "LevelSafeEscape", "0" }, { "LevelSafeMarginLevels", "8" }, { "LevelSafeEscapeMarginLevels", "12" },
                                               { "LevelSafeRadiusYards", "300" }, { "LevelSafeEscapeCooldownSec", "120" }, { "BelowMapRecover", "1" },
                                               { "BelowMapDropYards", "80" }, { "BelowMapStormCount", "0" }, { "BelowMapCooldownSec", "900" },
                                               { "LogRefusedReason", "0" }, { "DeathsTopEveryMin", "0" }, { "AuctionWalk3D", "1" }, { "AuctionLogReach", "0" } },
                                             warnings);
    CHECK(!set.LevelSafeWalks && !set.LevelSafeGraveyard && !set.LevelSafeEscape && set.LevelSafeMarginLevels == 8 && set.LevelSafeEscapeMarginLevels == 12 && set.LevelSafeRadiusYards == 300 &&
          set.LevelSafeEscapeCooldownSec == 120);
    CHECK(set.BelowMapRecover && set.BelowMapDropYards == 80 && set.BelowMapStormCount == 0 && set.BelowMapCooldownSec == 900);
    CHECK(!set.LogRefusedReason && set.DeathsTopEveryMin == 0 && set.Auction.Walk3D && !set.Auction.LogReach && warnings.empty());
    BotSettings const bad = ParseBotSettings({ { "LevelSafeMarginLevels", "0" }, { "LevelSafeRadiusYards", "5000" }, { "BelowMapDropYards", "5" },
                                               { "DeathsTopEveryMin", "2000" } },
                                             warnings);
    CHECK(bad.LevelSafeMarginLevels == 5 && bad.LevelSafeRadiusYards == 250 && bad.BelowMapDropYards == 60 && bad.DeathsTopEveryMin == 60 && warnings.size() == 4);
}

// ---- DeathLoopBreaker (2026-11-19, bot_policy_death_loop.cpp) ----

// Why the swap missed Raven Hill in arm C: the lowest minimum within 250 yards is 18 (a level 18-22 spot 240 yards off), and 18 is not above 8 + 10.
void TheRavenHillPlaceMinimumWasWithinTheWideMargin()
{
    std::vector<std::string> warnings;
    std::istringstream spots("spot 0 -10572.9 292.2 30.8 23 26 0 6\n"
                             "spot 0 -10510.6 286.2 31.4 23 27 0 6\n"
                             "spot 0 -10573.9 408.2 32.8 19 26 0 6\n"
                             "spot 0 -10629.7 533.1 31.8 18 22 0 6\n");
    level_safe::LevelMap levels;
    levels.Build(ParseGrindData(spots, warnings), 250.0f);
    CHECK(levels.PlaceMin(0, -10606.0f, 294.0f) == 18);
    CHECK(!levels.TooHigh(0, -10606.0f, 294.0f, 8, 10) && !levels.TooHigh(0, -10606.0f, 294.0f, 10, 10));   // no level_safe_yard for bots 235 and 66
    CHECK(levels.TooHigh(0, -10606.0f, 294.0f, 7, 10));
}

NearbyUnit Creature(float x, std::uint32_t level, std::uint32_t entry = 202)
{
    NearbyUnit unit;
    unit.guid = MakeUnitRef(8, entry, 7);
    unit.pos_x = x;
    unit.health = 100;
    unit.max_health = 100;
    unit.is_hostile = true;
    unit.level = level;
    return unit;
}

WorldTickSnapshotView At(float x, std::uint32_t level, std::vector<NearbyUnit> nearby = {}, std::uint64_t threat = 0)
{
    WorldTickSnapshotView view = View(1, threat, nearby);
    view.bots[0].level = level;
    view.bots[0].pos_x = x;
    return view;
}

// Alive at x, then dead there to `killer` (the threat target).
void DieAt(BotPolicy& policy, float x, std::uint32_t level, NearbyUnit const& killer, std::uint64_t at)
{
    policy.Decide(At(x, level), at);
    WorldTickSnapshotView dead = At(x, level, { killer }, killer.guid);
    Corpse(dead.bots[0], x);
    policy.Decide(dead, at + 1000);
}

std::vector<BotAction> GhostAt(BotPolicy& policy, float x, float corpseX, std::uint32_t level, std::uint64_t at, std::vector<NearbyUnit> nearby = {})
{
    WorldTickSnapshotView ghost = At(x, level, nearby);
    Ghost(ghost.bots[0], x, 0, corpseX);
    return policy.Decide(ghost, at);
}

bool AsksSpiritHealer(std::vector<BotAction> const& actions)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kResurrect && action.spell_id == 2)
            return true;
    return false;
}

std::size_t Count(std::vector<std::string> const& events, char const* text)
{
    std::size_t n = 0;
    for (std::string const& event : events)
        n += event.find(text) != std::string::npos ? 1 : 0;
    return n;
}

PolicyOptions BreakerOptions(std::vector<std::string>* events, bool on)
{
    PolicyOptions options = LevelOptions(events);
    options.DeathLoopBreaker = on;
    options.SideOf = [](std::uint64_t) -> std::uint8_t { return 1; };
    return options;
}

// A level 15 bot (Raven Hill, 23, is not too high for it by the wide margin: the arm C case) knows the low graveyard at x = 50, then dies three times at x = 2000 to
// entry 202, its ghost appearing at the graveyard x = 2050 each time. The third death breaks the loop as a ghost: the corpse is given up, the graveyard is unsafe,
// the ghost goes to x = 50 and the spirit healer revives it there; no death_loop_escape on the revive.
void AThirdDeathToOneKillerMovesTheGhostAway()
{
    for (bool on : { true, false })
    {
        std::vector<std::string> events;
        BotPolicy policy(BreakerOptions(&events, on));
        DieAt(policy, 0.0f, 15, Creature(5.0f, 3, 999), 1000);
        GhostAt(policy, 50.0f, 0.0f, 15, 3000);
        policy.Decide(At(50.0f, 15), 4000);
        std::vector<BotAction> third;
        for (std::uint64_t at : { 100000u, 160000u, 220000u })
        {
            DieAt(policy, 2000.0f, 15, Creature(2005.0f, 24), at);
            third = GhostAt(policy, 2050.0f, 2000.0f, 15, at + 2000);
            if (at != 220000u)
                policy.Decide(At(2050.0f, 15), at + 3000);
        }
        std::optional<BotAction> const teleport = TeleportOf(third, kBot);
        if (!on)
        {
            CHECK(!Logged(events, "death_loop_break:") && policy.UnsafeYardsKnown() == 0);
            CHECK(!(teleport && teleport->dest_x == 50.0f));
            continue;
        }
        CHECK(Logged(events, "death_loop_break: bot=42 level=15 map=0 pos=2000,0 entry=202 deaths=3 stage=detect"));
        CHECK(Count(events, "stage=detect") == 1);   // two deaths are not a loop
        CHECK(Logged(events, "death_loop_break: bot=42 level=15 map=0 stage=yard_unsafe graveyard=2050,0 entry=202 unsafe_s=7200"));
        CHECK(Logged(events, "death_loop_break: bot=42 level=15 map=0 stage=yard from=2050,0 to=50,0"));
        CHECK(Logged(events, "ghost_fallback: bot=42 reason=death_loop"));
        CHECK(teleport && teleport->dest_x == 50.0f && teleport->spell_id == 0);
        CHECK(policy.UnsafeYardsKnown() == 1);
        // At the new graveyard: the spirit healer, no corpse run back to x = 2000.
        std::vector<BotAction> const there = GhostAt(policy, 50.0f, 2000.0f, 15, 224000);
        CHECK(AsksSpiritHealer(there));
        for (BotAction const& action : there)
            CHECK(!(action.type == ActionType::kMoveTo && action.dest_x > 1000.0f));
        std::size_t const escapes = Count(events, "death_loop_escape:");
        CHECK(!TeleportOf(policy.Decide(At(50.0f, 15), 225000), kBot));
        CHECK(Logged(events, "death_loop_break: bot=42 level=15 map=0 stage=revived_away pos=50,0 loop=2000,0"));
        CHECK(Count(events, "death_loop_escape:") == escapes);
    }
}

// A bot the party plan holds (PartySize) but that sits in no group in the game is on its own: its loop is broken too (arm F: 82 to 318 such loops went unseen
// on single bots). One that is in a group is left to the party's rules.
void APlannedPartyWithoutAGroupDoesNotStopTheBreaker()
{
    for (bool grouped : { false, true })
    {
        std::vector<std::string> events;
        PolicyOptions options = BreakerOptions(&events, true);
        options.PartySize = 5;
        BotPolicy policy(options);
        for (std::uint64_t at : { 100000u, 160000u, 220000u })
        {
            policy.Decide(At(2000.0f, 15), at);
            WorldTickSnapshotView dead = At(2000.0f, 15, { Creature(2005.0f, 24) }, Creature(2005.0f, 24).guid);
            Corpse(dead.bots[0], 2000.0f);
            if (grouped)
                dead.bots[0].group_leader_guid = kBot;
            policy.Decide(dead, at + 1000);
        }
        CHECK((Count(events, "stage=detect") != 0) == !grouped);
    }
}

// The unsafe graveyard feeds the level-safe choice: a later death there (another killer, no loop) is sent to x = 50 by LevelSafeGraveyard, which alone would not
// judge Raven Hill too high for level 15.
void AnUnsafeGraveyardIsSkippedByTheLevelSafeChoice()
{
    for (bool on : { true, false })
    {
        std::vector<std::string> events;
        PolicyOptions options = BreakerOptions(&events, on);
        options.LevelSafe.Graveyard = true;
        BotPolicy policy(options);
        DieAt(policy, 0.0f, 15, Creature(5.0f, 3, 999), 1000);
        GhostAt(policy, 50.0f, 0.0f, 15, 3000);
        policy.Decide(At(50.0f, 15), 4000);
        for (std::uint64_t at : { 100000u, 160000u, 220000u })
        {
            DieAt(policy, 2000.0f, 15, Creature(2005.0f, 24), at);
            GhostAt(policy, 2050.0f, 2000.0f, 15, at + 2000);
            GhostAt(policy, 50.0f, 2000.0f, 15, at + 2500);
            policy.Decide(At(at == 220000u ? 50.0f : 2050.0f, 15), at + 3000);
        }
        DieAt(policy, 2000.0f, 15, Creature(2005.0f, 24, 888), 300000);
        std::optional<BotAction> const teleport = TeleportOf(GhostAt(policy, 2050.0f, 2000.0f, 15, 302000), kBot);
        CHECK(LoggedWith(events, "level_safe_yard: bot=42 level=15 map=0 graveyard=2050,0 graveyard_min=23 corpse_min=23 reason=graveyard_unsafe to=50,0") == on);
        CHECK((teleport && teleport->dest_x == 50.0f) == on);
        CHECK(!Logged(events, "level_safe_yard: bot=42 level=15 map=0 graveyard=2050,0 graveyard_min=23 corpse_min=23 reason=graveyard_high"));
    }
}

// A graveyard with a live hostile 3+ levels above the bot beside the ghost is not revived at: it is marked, and the next graveyard is taken.
void AGraveyardHeldByAHighHostileIsPassedOver()
{
    std::vector<std::string> events;
    BotPolicy policy(BreakerOptions(&events, true));
    DieAt(policy, 0.0f, 15, Creature(5.0f, 3, 999), 1000);
    GhostAt(policy, 50.0f, 0.0f, 15, 3000);
    policy.Decide(At(50.0f, 15), 4000);
    DieAt(policy, 300.0f, 15, Creature(305.0f, 3, 998), 10000);
    GhostAt(policy, 300.0f, 300.0f, 15, 12000);   // a second low graveyard, nearer to the loop
    policy.Decide(At(300.0f, 15), 13000);
    for (std::uint64_t at : { 100000u, 160000u, 220000u })
    {
        DieAt(policy, 2000.0f, 15, Creature(2005.0f, 24), at);
        GhostAt(policy, 2050.0f, 2000.0f, 15, at + 2000);
        if (at != 220000u)
            policy.Decide(At(2050.0f, 15), at + 3000);
    }
    CHECK(Logged(events, "death_loop_break: bot=42 level=15 map=0 stage=yard from=2050,0 to=300,0"));
    std::vector<BotAction> const held = GhostAt(policy, 300.0f, 2000.0f, 15, 240000, { Creature(320.0f, 18, 555) });
    CHECK(!AsksSpiritHealer(held));
    CHECK(Logged(events, "death_loop_break: bot=42 level=15 map=0 stage=yard_held graveyard=300,0 entry=555 hostile_level=18"));
    std::optional<BotAction> const next = TeleportOf(GhostAt(policy, 300.0f, 2000.0f, 15, 241000, { Creature(320.0f, 18, 555) }), kBot);
    CHECK(Logged(events, "death_loop_break: bot=42 level=15 map=0 stage=yard from=2050,0 to=50,0"));
    CHECK(next && next->dest_x == 50.0f);
    CHECK(policy.UnsafeYardsKnown() == 2);
    // A hostile only 2 levels above is no reason to wait.
    std::vector<std::string> other;
    BotPolicy calm(BreakerOptions(&other, true));
    DieAt(calm, 0.0f, 15, Creature(5.0f, 3, 999), 1000);
    GhostAt(calm, 50.0f, 0.0f, 15, 3000);
    calm.Decide(At(50.0f, 15), 4000);
    for (std::uint64_t at : { 100000u, 160000u, 220000u })
    {
        DieAt(calm, 2000.0f, 15, Creature(2005.0f, 24), at);
        GhostAt(calm, 2050.0f, 2000.0f, 15, at + 2000);
        if (at != 220000u)
            calm.Decide(At(2050.0f, 15), at + 3000);
    }
    CHECK(AsksSpiritHealer(GhostAt(calm, 50.0f, 2000.0f, 15, 240000, { Creature(60.0f, 17, 555) })));
}

// No other graveyard known: the ghost waits (no corpse run, no healer) up to 5 minutes, then revives, and the revived bot is moved to a grind spot away from the loop
// once it is out of combat.
void WithNoGraveyardTheGhostWaitsThenTheBotIsMoved()
{
    std::vector<std::string> events;
    BotPolicy policy(BreakerOptions(&events, true));
    for (std::uint64_t at : { 100000u, 160000u, 220000u })
    {
        DieAt(policy, 2000.0f, 15, Creature(2005.0f, 24), at);
        GhostAt(policy, 2050.0f, 2000.0f, 15, at + 2000);
        if (at != 220000u)
            policy.Decide(At(2050.0f, 15), at + 3000);
    }
    CHECK(Logged(events, "death_loop_break: bot=42 level=15 map=0 stage=wait result=no_safe_yard known=1 wait_s=300"));
    for (std::uint64_t at = 223000; at < 222000 + 300000; at += 30000)
    {
        std::vector<BotAction> const waiting = GhostAt(policy, 2050.0f, 2000.0f, 15, at);
        CHECK(!AsksSpiritHealer(waiting) && !TeleportOf(waiting, kBot));
        for (BotAction const& action : waiting)
            CHECK(action.type != ActionType::kMoveTo && action.type != ActionType::kReclaimCorpse);
    }
    CHECK(AsksSpiritHealer(GhostAt(policy, 2050.0f, 2000.0f, 15, 523000)));
    CHECK(Logged(events, "death_loop_break: bot=42 level=15 map=0 stage=wait_over"));
    std::size_t const escapes = Count(events, "death_loop_escape:");
    WorldTickSnapshotView fighting = At(2050.0f, 15, { Creature(2055.0f, 24) });
    fighting.bots[0].in_combat = 1;
    CHECK(!TeleportOf(policy.Decide(fighting, 524000), kBot));   // the core refuses a teleport in combat: not sent
    CHECK(Logged(events, "death_loop_break: bot=42 level=15 map=0 stage=in_combat pos=2050,0"));
    std::optional<BotAction> const away = TeleportOf(policy.Decide(At(2050.0f, 15), 525000), kBot);
    // (spot 3 at x = 400 was the second death's death_loop_escape and is skipped as just visited)
    CHECK(away && away->dest_x == 300.0f && away->spell_id == 0);
    CHECK(Logged(events, "death_loop_break: bot=42 level=15 map=0 stage=escape from=2050,0 spot=2 to=300,0"));
    CHECK(Count(events, "death_loop_escape:") == escapes);
}

// The Skyborne rule: a loop on map 2991 never sends the ghost or the bot to another map, even with graveyards and grind spots known elsewhere.
void ASkyborneLoopStaysOnItsMap()
{
    std::vector<std::string> events;
    BotPolicy policy(BreakerOptions(&events, true));
    DieAt(policy, 0.0f, 15, Creature(5.0f, 3, 999), 1000);
    GhostAt(policy, 50.0f, 0.0f, 15, 3000);   // a graveyard on map 0
    policy.Decide(At(50.0f, 15), 4000);
    auto onSky = [](WorldTickSnapshotView view)
    {
        view.bots[0].map_id = 2991;
        view.bots[0].corpse_map = view.bots[0].corpse_valid ? 2991 : 0;
        return view;
    };
    bool offMap = false;
    auto watch = [&](std::vector<BotAction> const& actions)
    {
        for (BotAction const& action : actions)
            offMap = offMap || (action.type == ActionType::kTeleport && action.spell_id != 2991);
    };
    for (std::uint64_t at : { 100000u, 160000u, 220000u })
    {
        watch(policy.Decide(onSky(At(2000.0f, 15)), at));
        WorldTickSnapshotView dead = At(2000.0f, 15, { Creature(2005.0f, 24, 251707) }, Creature(2005.0f, 24, 251707).guid);
        Corpse(dead.bots[0], 2000.0f);
        watch(policy.Decide(onSky(dead), at + 1000));
        WorldTickSnapshotView ghost = At(2050.0f, 15);
        Ghost(ghost.bots[0], 2050.0f, 0, 2000.0f);
        watch(policy.Decide(onSky(ghost), at + 2000));
        if (at != 220000u)
            watch(policy.Decide(onSky(At(2050.0f, 15)), at + 3000));
    }
    CHECK(Logged(events, "death_loop_break: bot=42 level=15 map=2991 pos=2000,0 entry=251707 deaths=3 stage=detect"));
    CHECK(Logged(events, "death_loop_break: bot=42 level=15 map=2991 stage=wait result=no_safe_yard"));
    WorldTickSnapshotView late = At(2050.0f, 15);
    Ghost(late.bots[0], 2050.0f, 0, 2000.0f);
    watch(policy.Decide(onSky(late), 523000));
    watch(policy.Decide(onSky(At(2050.0f, 15)), 524000));
    CHECK(Logged(events, "death_loop_break: bot=42 level=15 map=2991 stage=no_spot"));
    CHECK(!offMap);
}

void TheBreakerKeyIsOffByDefault()
{
    std::vector<std::string> warnings;
    CHECK(!ParseBotSettings({}, warnings).DeathLoopBreaker);
    CHECK(ParseBotSettings({ { "DeathLoopBreaker", "1" } }, warnings).DeathLoopBreaker && warnings.empty());
    CHECK(!PolicyOptions{}.DeathLoopBreaker);
}

} // namespace

int main()
{
    PlaceLevelsComeFromTheNearSpots();
    AWalkIsCheckedAlongItsLineAndAtItsEnd();
    GraveyardsAreRememberedBySideAndChosenByLevel();
    TheDeathsTopLineCountsAndStartsAgain();
    BelowTheMapIsSeenByHeightOrByAStorm();
    TheAuctionReachIsMeasuredIn3d();
    ALowBotInAHighPlaceIsMovedOut();
    AGhostIsRevivedAtAGraveyardThatFitsItsLevel();
    WithNoFittingGraveyardTheGhostDoesAsBefore();
    AQuestWalkIntoAHighPlaceIsDropped();
    TheBlockSkipsAreasFirst();
    TheDeathsTopLineIsDueOncePerPeriod();
    BelowTheMapThePolicyTeleportsUp();
    TheSettingsKeysHaveTheirDefaultsAndBounds();
    TheRavenHillPlaceMinimumWasWithinTheWideMargin();
    AThirdDeathToOneKillerMovesTheGhostAway();
    APlannedPartyWithoutAGroupDoesNotStopTheBreaker();
    AnUnsafeGraveyardIsSkippedByTheLevelSafeChoice();
    AGraveyardHeldByAHighHostileIsPassedOver();
    WithNoGraveyardTheGhostWaitsThenTheBotIsMoved();
    ASkyborneLoopStaysOnItsMap();
    TheBreakerKeyIsOffByDefault();
    if (failures == 0)
        std::printf("level_safe_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
