// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the flexible tank and healer roles (flex_roles.h). No core needed.
#include "flex_roles.h"

#include <cstdio>

using namespace flex_roles;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

// Classes in guid order 1, 2, 3...
std::vector<Member> Party(std::vector<std::uint32_t> const& classes, std::uint32_t level = 20)
{
    std::vector<Member> party;
    std::uint64_t guid = 1;
    for (std::uint32_t classId : classes)
        party.push_back(Member{ guid++, classId, level, 0 });
    return party;
}

std::uint32_t RoleOf(Result const& r, std::uint64_t guid)
{
    auto const at = r.Role.find(guid);
    return at == r.Role.end() ? 0u : at->second;
}

void AWarriorStaysTank()
{
    Result const r = Assign(Party({ 2, 1, 5, 8, 4 }));
    CHECK(r.HasTank && r.HasHealer);
    CHECK(RoleOf(r, 2) == 0);      // the warrior is left on Auto
    CHECK(RoleOf(r, 3) == 2);      // the priest heals
    CHECK(RoleOf(r, 1) == 0);      // the paladin is not made a tank
}

void PaladinBeforeDruidBeforeNothing()
{
    Result r = Assign(Party({ 11, 2, 5, 8, 4 }));
    CHECK(RoleOf(r, 2) == 1 && RoleOf(r, 3) == 2 && RoleOf(r, 1) == 0);
    r = Assign(Party({ 11, 8, 5, 8, 4 }));
    CHECK(RoleOf(r, 1) == 1 && RoleOf(r, 3) == 2);
}

void BothRolesAreFilledFromPaladinAndDruid()
{
    // paladin + druid and no other healer: the paladin tanks, the druid heals, whatever the guid order
    for (auto const& classes : { std::vector<std::uint32_t>{ 2, 11, 8, 4, 3 }, std::vector<std::uint32_t>{ 11, 2, 8, 4, 3 } })
    {
        Result const r = Assign(Party(classes));
        CHECK(r.HasTank && r.HasHealer);
        std::uint64_t const paladin = classes[0] == 2 ? 1 : 2, druid = paladin == 1 ? 2 : 1;
        CHECK(RoleOf(r, paladin) == 1 && RoleOf(r, druid) == 2);
    }
}

void TwoDruidsAndTwoPaladinsSplitDeterministically()
{
    Result r = Assign(Party({ 11, 11, 8, 4, 3 }));
    CHECK(RoleOf(r, 1) == 1 && RoleOf(r, 2) == 2);
    r = Assign(Party({ 2, 2, 8, 4, 3 }));
    CHECK(RoleOf(r, 1) == 1 && RoleOf(r, 2) == 2);
    Result const again = Assign(Party({ 2, 2, 8, 4, 3 }));
    CHECK(again.Role == r.Role && again.Reason == r.Reason);
}

void ShamanAndPriestHealWithADruidTank()
{
    Result r = Assign(Party({ 11, 7, 8, 4, 3 }));
    CHECK(RoleOf(r, 1) == 1 && RoleOf(r, 2) == 2);
    r = Assign(Party({ 11, 7, 5, 4, 3 }));
    CHECK(RoleOf(r, 1) == 1 && RoleOf(r, 3) == 2);    // the priest comes before the shaman
}

void ALoneTankCandidateTanksOverHealing()
{
    Result const r = Assign(Party({ 2, 8, 4, 3, 9 }));
    CHECK(r.HasTank && !r.HasHealer);
    CHECK(RoleOf(r, 1) == 1);
    CHECK(r.Reason.find("tank_over_healer") != std::string::npos);
}

void UnsupportedPartiesAreSaidSo()
{
    Result r = Assign(Party({ 8, 4, 3, 9, 8 }));               // five damage classes: nobody can tank or heal
    CHECK(!r.HasTank && !r.HasHealer && r.Role.empty());
    CHECK(r.Reason.find("tank=none") != std::string::npos);
    r = Assign(Party({ 5, 7, 8, 4, 3 }));                      // priest and shaman and no tank class: a healer only
    CHECK(!r.HasTank && r.HasHealer && RoleOf(r, 1) == 2);
    r = Assign(Party({ 1, 8, 4, 3, 9 }));                      // a warrior and nobody who heals
    CHECK(r.HasTank && !r.HasHealer);
}

void ALowLevelPaladinOrDruidIsNoTank()
{
    Result r = Assign(Party({ 2, 5, 8, 4, 3 }, 8));
    CHECK(!r.HasTank && RoleOf(r, 2) == 2);
    r = Assign(Party({ 2, 5, 8, 4, 3 }, 10));
    CHECK(r.HasTank && RoleOf(r, 1) == 1);
    CHECK(TankRank(1, 1) == 1 && TankRank(11, 9) == 0);
}

void ExplicitRolesAreKept()
{
    std::vector<Member> party = Party({ 2, 11, 5, 8, 4 });
    party[0].Explicit = 3;                               // the paladin is a damage dealer by its spec
    Result r = Assign(party);
    CHECK(RoleOf(r, 1) == 0 && RoleOf(r, 2) == 1 && RoleOf(r, 3) == 2);
    party = Party({ 2, 11, 5, 8, 4 });
    party[1].Explicit = 1;                               // an explicit tank fills the slot
    r = Assign(party);
    CHECK(r.HasTank && RoleOf(r, 1) == 0 && RoleOf(r, 2) == 0 && RoleOf(r, 3) == 2);
    party[2].Explicit = 2;                               // and an explicit healer
    r = Assign(party);
    CHECK(r.Role.empty() && r.HasTank && r.HasHealer);
    party = Party({ 1, 5, 8, 4, 3 });
    party[0].Explicit = 3;                               // an arms warrior is no tank: nobody else can either
    r = Assign(party);
    CHECK(!r.HasTank);
}

void EveryFiveClassPartyWithATankAndAHealerCandidateGetsBoth()
{
    // Any 5 classes of the nine: when the party holds a tank class (warrior, paladin, druid) and a second bot that heals, it gets one tank and one healer.
    std::uint32_t const all[] = { 1, 2, 3, 4, 5, 7, 8, 9, 11 };
    int checkedParties = 0;
    for (int mask = 0; mask < (1 << 9); ++mask)
    {
        std::vector<std::uint32_t> classes;
        for (int i = 0; i < 9; ++i)
            if (mask & (1 << i))
                classes.push_back(all[i]);
        if (classes.size() != 5)
            continue;
        ++checkedParties;
        Result const r = Assign(Party(classes));
        int tankCapable = 0, healCapable = 0;
        for (std::uint32_t c : classes)
        {
            tankCapable += TankRank(c, 20) != 0;
            healCapable += HealerRank(c) != 0;
        }
        // a tank and a healer exist as two different bots: the classes with both abilities are counted by a small matching
        bool possible = false;
        for (std::size_t t = 0; t < classes.size(); ++t)
            for (std::size_t h = 0; h < classes.size(); ++h)
                possible = possible || (t != h && TankRank(classes[t], 20) != 0 && HealerRank(classes[h]) != 0);
        CHECK(r.HasTank == (tankCapable > 0));
        CHECK((r.HasTank && r.HasHealer) == possible);
        int tanks = 0, healers = 0;
        for (auto const& pick : r.Role)
            (pick.second == 1 ? tanks : healers) += 1;
        CHECK(tanks <= 1 && healers <= 1);
    }
    CHECK(checkedParties == 126);
}

void PullCapsAreParameterised()
{
    CHECK(DungeonPullCap(1, 4) == 4);
    CHECK(DungeonPullCap(2, 4) == 3);
    CHECK(DungeonPullCap(11, 4) == 3);
    CHECK(DungeonPullCap(2, 1) == 1);
}

} // namespace

int main()
{
    AWarriorStaysTank();
    PaladinBeforeDruidBeforeNothing();
    BothRolesAreFilledFromPaladinAndDruid();
    TwoDruidsAndTwoPaladinsSplitDeterministically();
    ShamanAndPriestHealWithADruidTank();
    ALoneTankCandidateTanksOverHealing();
    UnsupportedPartiesAreSaidSo();
    ALowLevelPaladinOrDruidIsNoTank();
    ExplicitRolesAreKept();
    EveryFiveClassPartyWithATankAndAHealerCandidateGetsBoth();
    PullCapsAreParameterised();
    if (failures == 0)
        std::printf("flex_roles_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
