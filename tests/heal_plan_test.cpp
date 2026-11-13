// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for planned healing (heal_plan.h). No core needed.
#include "heal_plan.h"

#include <cstdio>

using namespace heal_plan;
using party_planner::Member;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

Member Healer(std::uint64_t guid)
{
    Member m;
    m.Guid = guid;
    m.CanHeal = true;
    return m;
}

Member Tank(std::uint64_t guid)
{
    Member m;
    m.Guid = guid;
    m.Tank = true;
    return m;
}

Member Dps(std::uint64_t guid)
{
    Member m;
    m.Guid = guid;
    return m;
}

void MainHealerTakesTheTank()
{
    Settings s;
    Plan plan;
    std::vector<Member> party{ Tank(1), Healer(7), Healer(5), Dps(2), Dps(3) };
    CHECK(Update(s, party, plan));
    CHECK(plan.MainHealer == 5);                       // the lowest healer guid
    CHECK(plan.HealerOf.at(1) == 5);
    CHECK(IsAssigned(&plan, 5, 1) && !IsAssigned(&plan, 7, 1));
    CHECK(plan.HealerOf.size() == 5);
    // the other healer heals the dps; each dps goes to the least loaded healer (the main healer carries the tank, load 2)
    CHECK(plan.HealerOf.at(2) == 7 && plan.HealerOf.at(3) == 7);
}

void LeaderIsTheTankWhenNobodyPlaysIt()
{
    Settings s;
    Plan plan;
    Member leader = Dps(9);
    leader.Leader = true;
    CHECK(Update(s, { leader, Healer(4), Healer(6) }, plan));
    CHECK(plan.HealerOf.at(9) == plan.MainHealer);
}

void OneHealerHealsEveryone()
{
    Settings s;
    Plan plan;
    Update(s, { Tank(1), Healer(2), Dps(3), Dps(4) }, plan);
    CHECK(plan.HealerOf.at(1) == 2 && plan.HealerOf.at(2) == 2 && plan.HealerOf.at(3) == 2 && plan.HealerOf.at(4) == 2);
    CHECK(Judge(&plan, 2, 3, false, false, 10, 20) == Verdict::Own);
}

void NoHealerNoPlan()
{
    Settings s;
    Plan plan;
    Update(s, { Tank(1), Dps(2) }, plan);
    CHECK(plan.Healers.empty() && plan.HealerOf.empty());
    CHECK(Describe(plan) == "healers=0");
    CHECK(Judge(&plan, 5, 1, true, true, 10, 20) == Verdict::Own);
    CHECK(Judge(nullptr, 5, 1, true, true, 10, 20) == Verdict::Own);
}

void ChangeOnlyWhenAnAssignmentChanges()
{
    Settings s;
    Plan plan;
    std::vector<Member> party{ Tank(1), Healer(5), Healer(7), Dps(2), Dps(3) };
    CHECK(Update(s, party, plan));
    CHECK(!Update(s, party, plan));                    // same input: no event
    party[3].Alive = false;                            // a dead member keeps its assignment
    CHECK(!Update(s, party, plan));
    party.push_back(Dps(8));                           // a new member is a change
    CHECK(Update(s, party, plan));
    CHECK(plan.HealerOf.count(8) == 1);
}

void AssignmentsStayWhenAMemberJoins()
{
    Settings s;
    Plan plan;
    std::vector<Member> party{ Tank(1), Healer(5), Healer(7), Dps(2), Dps(3), Dps(4) };
    Update(s, party, plan);
    auto const before = plan.HealerOf;
    party.push_back(Dps(6));
    Update(s, party, plan);
    for (auto const& pair : before)
        CHECK(plan.HealerOf.at(pair.first) == pair.second);
}

void DeadHealerHandsOverAndMainHealerMoves()
{
    Settings s;
    Plan plan;
    std::vector<Member> party{ Tank(1), Healer(5), Healer(7), Dps(2), Dps(3) };
    Update(s, party, plan);
    CHECK(plan.MainHealer == 5);
    party[1].Alive = false;                            // the main healer dies
    CHECK(Update(s, party, plan));
    CHECK(plan.MainHealer == 7);
    CHECK(plan.HealerOf.at(1) == 7);
    for (auto const& pair : plan.HealerOf)
        CHECK(pair.second == 7);
    party[1].Alive = true;                             // it comes back: the main healer stays 7 (no flapping), the healer set changed so members are rebuilt
    Update(s, party, plan);
    CHECK(plan.MainHealer == 7 && plan.HealerOf.at(1) == 7);
}

void RaidSplitsEvenly()
{
    Settings s;
    Plan plan;
    std::vector<Member> raid{ Tank(1), Tank(2) };
    for (std::uint64_t g = 10; g < 14; ++g)
        raid.push_back(Healer(g));
    for (std::uint64_t g = 20; g < 36; ++g)
        raid.push_back(Dps(g));
    Update(s, raid, plan);
    CHECK(plan.MainHealer == 10 && plan.HealerOf.at(1) == 10 && plan.HealerOf.at(2) == 10);
    std::unordered_map<std::uint64_t, int> count;
    for (auto const& pair : plan.HealerOf)
        ++count[pair.second];
    int lo = 99, hi = 0;
    for (std::uint64_t h : plan.Healers)
    {
        lo = count[h] < lo ? count[h] : lo;
        hi = count[h] > hi ? count[h] : hi;
    }
    CHECK(hi - lo <= 2);
    CHECK(count[10] < count[11]);                      // the tank healer carries fewer members
    for (std::uint64_t h : plan.Healers)
        CHECK(plan.HealerOf.at(h) != h);               // a healer is not its own assignment while others exist
}

void JudgeSkipsAndSwitches()
{
    Settings s;
    Plan plan;
    Update(s, { Tank(1), Healer(5), Healer(7), Dps(2), Dps(3) }, plan);
    // 7 holds 2 and 3; healer 5 holds the tank
    CHECK(Judge(&plan, 5, 2, true, true, 40, 20) == Verdict::Skip);      // its own member needs a heal: leave 2 alone
    CHECK(Judge(&plan, 5, 2, false, true, 40, 20) == Verdict::Skip);     // the owner is free to take it
    CHECK(Judge(&plan, 5, 2, false, false, 40, 20) == Verdict::Switch);  // its own are fine and the owner cannot: take it
    CHECK(Judge(&plan, 5, 2, true, false, 10, 20) == Verdict::Own);      // an emergency is everyone's
    CHECK(Judge(&plan, 5, 1, true, true, 40, 20) == Verdict::Own);       // its own member
    CHECK(Judge(&plan, 5, 99, true, true, 40, 20) == Verdict::Own);      // not in the plan at all
}

void PreHealWindow()
{
    Settings s;
    Plan plan;
    Update(s, { Tank(1), Healer(5), Healer(7), Dps(2) }, plan);
    CHECK(TankThreshold(s, &plan, 5, 1, 75, 1000, 1000) == 96);          // main healer at the pull
    CHECK(TankThreshold(s, &plan, 5, 1, 75, 4900, 1000) == 96);
    CHECK(TankThreshold(s, &plan, 5, 1, 75, 5100, 1000) == 75);          // window over
    CHECK(TankThreshold(s, &plan, 7, 1, 75, 1500, 1000) == 75);          // not the tank's healer
    CHECK(TankThreshold(s, &plan, 5, 1, 75, 1500, 0) == 75);             // no fight
    CHECK(TankThreshold(s, &plan, 5, 1, 99, 1500, 1000) == 99);          // never lowers the base
    Settings off = s;
    off.PreHealMs = 0;
    CHECK(TankThreshold(off, &plan, 5, 1, 75, 1500, 1000) == 75);
    off = s;
    off.Enabled = false;
    Plan none;
    CHECK(!Update(off, { Tank(1), Healer(5) }, none) && none.Healers.empty());   // off: no plan, no event
}

void DescribeIsStable()
{
    Settings s;
    Plan plan;
    Update(s, { Tank(1), Healer(5), Healer(7), Dps(2) }, plan);
    CHECK(plan.Signature.rfind("healers=2 main=5 5:[", 0) == 0);
}

} // namespace

int main()
{
    MainHealerTakesTheTank();
    LeaderIsTheTankWhenNobodyPlaysIt();
    OneHealerHealsEveryone();
    NoHealerNoPlan();
    ChangeOnlyWhenAnAssignmentChanges();
    AssignmentsStayWhenAMemberJoins();
    DeadHealerHandsOverAndMainHealerMoves();
    RaidSplitsEvenly();
    JudgeSkipsAndSwitches();
    PreHealWindow();
    DescribeIsStable();
    if (failures)
        std::printf("%d failure(s)\n", failures);
    else
        std::printf("heal_plan_test: ok\n");
    return failures ? 1 : 0;
}
