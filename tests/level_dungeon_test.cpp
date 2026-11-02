// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the level-dungeon schedule and the LFG planning (level_dungeon.h). No core needed.
#include "level_dungeon.h"

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

} // namespace

int main()
{
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
