// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the level-dungeon schedule and the LFG planning (level_dungeon.h). No core needed.
#include "level_dungeon.h"

#include <string>

#include <cstdio>

using namespace level_dungeon;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

std::vector<Dungeon> Dungeons()
{
    return {
        { "rfc", "Ragefire Chasm", 13, 18, 389 },
        { "wc", "Wailing Caverns", 15, 21, 43 },
        { "dm", "The Deadmines", 17, 26, 36 },
    };
}

void DueOnlyAtTheMaxLevel()
{
    auto const dungeons = Dungeons();
    Book book;
    CHECK(Due(dungeons, 17, book, 2) == -1);
    CHECK(Due(dungeons, 18, book, 2) == 0);     // rfc reaches its max level
    CHECK(Due(dungeons, 21, book, 2) == 1);     // wc
    CHECK(Due(dungeons, 26, book, 2) == 2);
    CHECK(Due(dungeons, 29, book, 2) == -1);    // too far past: no catch-up
    CHECK(Due(dungeons, 20, book, 2) == 0);     // within the grace of rfc
}

void LowestMaxLevelFirstWhenSeveralAreDue()
{
    auto const dungeons = Dungeons();
    Book book;
    CHECK(Due(dungeons, 20, book, 2) == 0);
    NoteStarted(book, "rfc", 20);
    NoteCleared(book, "rfc");
    CHECK(Due(dungeons, 21, book, 2) == 1);
}

void SecondAttemptWaitsForTheNextLevelAndStopsAtTheLimit()
{
    auto const dungeons = Dungeons();
    Book book;
    NoteStarted(book, "rfc", 18);
    CHECK(Due(dungeons, 18, book, 2) == -1);    // once per level
    CHECK(Due(dungeons, 19, book, 2) == 0);     // second attempt, next level
    NoteStarted(book, "rfc", 19);
    CHECK(Due(dungeons, 20, book, 2) == -1);    // two runs are all
    CHECK(Due(dungeons, 20, book, 3) == 0);     // a limit of 3 allows one more
}

void ClearedIsDone()
{
    auto const dungeons = Dungeons();
    Book book;
    NoteStarted(book, "rfc", 18);
    NoteCleared(book, "rfc");
    CHECK(Due(dungeons, 19, book, 5) == -1);
}

void RunsAreClamped()
{
    CHECK(ClampRuns(0) == 1);
    CHECK(ClampRuns(2) == 2);
    CHECK(ClampRuns(99) == 5);
    auto const dungeons = Dungeons();
    Book book;
    NoteStarted(book, "rfc", 18);
    CHECK(Due(dungeons, 19, book, 0) == -1);    // 0 reads as 1
}

void FindsByKeyOrName()
{
    auto const dungeons = Dungeons();
    CHECK(FindDungeon(dungeons, "rfc") == 0);
    CHECK(FindDungeon(dungeons, "RFC") == 0);
    CHECK(FindDungeon(dungeons, "ragefire") == 0);
    CHECK(FindDungeon(dungeons, "Ragefire Chasm") == 0);
    CHECK(FindDungeon(dungeons, "deadmines") == 2);
    CHECK(FindDungeon(dungeons, "caverns") == 1);
    CHECK(FindDungeon(dungeons, "nope") == -1);
    CHECK(FindDungeon(dungeons, "the") == 2);
    CHECK(FindDungeon(dungeons, "") == -1);
    CHECK(KeyList(dungeons) == "dm, rfc, wc");
}

void AmbiguousNameWordMatchesNothing()
{
    std::vector<Dungeon> const dungeons = { { "sfk", "Shadowfang Keep", 22, 30, 33 }, { "sm", "Scarlet Monastery", 28, 38, 189 }, { "dm", "The Deadmines", 17, 26, 36 } };
    CHECK(FindDungeon(dungeons, "keep") == 0);
    CHECK(FindDungeon(dungeons, "scarlet") == 1);
    std::vector<Dungeon> const two = { { "sm1", "Scarlet Monastery Graveyard", 28, 38, 189 }, { "sm2", "Scarlet Monastery Library", 28, 38, 189 } };
    CHECK(FindDungeon(two, "scarlet") == -1);
    CHECK(FindDungeon(two, "library") == 1);
}

void PlansTheMissingRoles()
{
    // A lone player: tank, healer, three dps would be 5 total, so 4 slots: tank, healer, 2 dps.
    auto roles = MissingRoles(1, 0, 0);
    CHECK(roles.size() == 4);
    CHECK(roles[0] == Role::Tank && roles[1] == Role::Healer && roles[2] == Role::Dps && roles[3] == Role::Dps);
    CHECK(RoleList(roles) == "tank, healer, dps x2");
    // The speaker and a warrior: a healer and two dps.
    roles = MissingRoles(2, 1, 0);
    CHECK(RoleList(roles) == "healer, dps x2");
    // Three with a tank and a healer: two dps.
    roles = MissingRoles(3, 1, 1);
    CHECK(RoleList(roles) == "dps x2");
    // Four with no tank: just the tank.
    roles = MissingRoles(4, 0, 1);
    CHECK(RoleList(roles) == "tank");
    // Full: nothing.
    CHECK(MissingRoles(5, 0, 0).empty());
    CHECK(MissingRoles(7, 0, 0).empty());
    CHECK(RoleList({}).empty());
}

void ClassesFillRoles()
{
    CHECK(RoleOfClass(1) == Role::Tank);
    CHECK(RoleOfClass(2) == Role::Healer);
    CHECK(RoleOfClass(5) == Role::Healer);
    CHECK(RoleOfClass(7) == Role::Healer);
    CHECK(RoleOfClass(11) == Role::Healer);
    CHECK(RoleOfClass(3) == Role::Dps);
    CHECK(RoleOfClass(4) == Role::Dps);
    CHECK(RoleOfClass(8) == Role::Dps);
    CHECK(RoleOfClass(9) == Role::Dps);
}

void EarlyLevelsZeroKeepsTheOldSchedule()
{
    auto const dungeons = Dungeons();
    Book book;
    for (std::uint32_t level = 1; level <= 40; ++level)
        CHECK(Due(dungeons, level, book, 2, DueRules{}) == Due(dungeons, level, book, 2));
    CHECK(FirstDueLevel(dungeons[0], 0) == 18);
}

void EarlyLevelsOpenTheBandBelowTheMax()
{
    auto const dungeons = Dungeons();
    Book book;
    DueRules rules;
    rules.EarlyLevels = 3;
    CHECK(FirstDueLevel(dungeons[0], 3) == 15);
    CHECK(Due(dungeons, 14, book, 2, rules) == -1);
    DueResult const at15 = DueWhy(dungeons, 15, book, 2, rules);
    CHECK(at15.Index == 0 && at15.Early && at15.Why == Skip::None);   // rfc from 15
    CHECK(Due(dungeons, 18, book, 2, rules) == 0);
    CHECK(!DueWhy(dungeons, 18, book, 2, rules).Early);
    CHECK(Due(dungeons, 20, book, 2, rules) == 0);                    // the grace above the max is unchanged
    // Never below the min level, however early.
    rules.EarlyLevels = 20;
    CHECK(FirstDueLevel(dungeons[0], 20) == 13);
    CHECK(Due(dungeons, 12, book, 2, rules) == -1);
    CHECK(Due(dungeons, 13, book, 2, rules) == 0);
    // Once per level still holds.
    rules.EarlyLevels = 3;
    NoteStarted(book, "rfc", 15);
    DueResult const again = DueWhy(dungeons, 15, book, 2, rules);
    CHECK(again.Index == -1 && again.Why == Skip::SameLevel);
    CHECK(Due(dungeons, 16, book, 2, rules) == 0);
}

void SidesKeepAPartyOutOfTheOtherSidesDungeon()
{
    std::vector<Dungeon> dungeons = { { "hot", "Hall of Thanes", 13, 18, 3065 }, { "rfc", "Ragefire Chasm", 13, 18, 389 } };
    Book book;
    CHECK(Due(dungeons, 18, book, 2) == 0);           // no sides: hot first by id, for both sides
    dungeons[0].Side = 1;                              // hot: Alliance
    dungeons[1].Side = 2;                              // rfc: Horde
    DueRules horde;
    horde.PartySide = 2;
    DueRules alliance;
    alliance.PartySide = 1;
    CHECK(Due(dungeons, 18, book, 2, horde) == 1);
    CHECK(Due(dungeons, 18, book, 2, alliance) == 0);
    DueRules unknown;                                  // side unknown: no sided dungeon
    DueResult const none = DueWhy(dungeons, 18, book, 2, unknown);
    CHECK(none.Index == -1 && none.Why == Skip::WrongSide);
    // A cleared own-side dungeon is reported as cleared (the closer reason), not as wrong_side.
    NoteStarted(book, "rfc", 18);
    NoteCleared(book, "rfc");
    CHECK(DueWhy(dungeons, 19, book, 2, horde).Why == Skip::Cleared);
    CHECK(std::string(SkipName(Skip::WrongSide)) == "wrong_side" && std::string(SkipName(Skip::NoDungeon)) == "no_dungeon");
}

void ParsesTheSideList()
{
    std::vector<std::string> warnings;
    auto const sides = ParseSides(" RFC:horde, hot:Alliance ,dm:1,wc:h,sfk:any,bad,zz:mars,", warnings);
    CHECK(sides.size() == 5);
    CHECK(sides.at("rfc") == 2 && sides.at("hot") == 1 && sides.at("dm") == 1 && sides.at("wc") == 2 && sides.at("sfk") == 0);
    CHECK(warnings.size() == 2);                       // "bad" and "zz:mars"
    warnings.clear();
    CHECK(ParseSides("", warnings).empty() && warnings.empty());
}

void RunCapFromTheFile()
{
    constexpr std::uint64_t k45 = 45 * 60 * 1000;
    CHECK(RunCapMs(1800, false, k45) == k45);          // key off: 45 minutes
    CHECK(RunCapMs(1800, true, k45) == 1800000);       // key on: the file's duration_sec
    CHECK(RunCapMs(0, true, k45) == k45);              // no duration in the file: 45 minutes
}

void TankAndHealerCheck()
{
    CHECK(HasTankAndHealer({ Role::Tank, Role::Healer, Role::Dps }));
    CHECK(!HasTankAndHealer({ Role::Tank, Role::Dps, Role::Dps }));
    CHECK(!HasTankAndHealer({ Role::Healer, Role::Healer, Role::Dps }));
    CHECK(!HasTankAndHealer({}));
    CHECK(HasTankAndHealer({ RoleOfClass(1), RoleOfClass(5), RoleOfClass(8) }));   // warrior, priest, mage
}

} // namespace

int main()
{
    EarlyLevelsZeroKeepsTheOldSchedule();
    EarlyLevelsOpenTheBandBelowTheMax();
    SidesKeepAPartyOutOfTheOtherSidesDungeon();
    ParsesTheSideList();
    RunCapFromTheFile();
    TankAndHealerCheck();
    DueOnlyAtTheMaxLevel();
    LowestMaxLevelFirstWhenSeveralAreDue();
    SecondAttemptWaitsForTheNextLevelAndStopsAtTheLimit();
    ClearedIsDone();
    RunsAreClamped();
    FindsByKeyOrName();
    AmbiguousNameWordMatchesNothing();
    PlansTheMissingRoles();
    ClassesFillRoles();
    if (failures == 0)
        std::printf("level_dungeon_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
