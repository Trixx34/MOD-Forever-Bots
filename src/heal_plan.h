// Planned healing (feature/planned-healing): the healers of a party or raid split the heal duty instead of every healer scanning the whole group.
// Plain structs and free functions over party_planner::Member, no SDK dependency, unit-tested with hand-built input (tests/heal_plan_test.cpp).
//
// The standing assignment: the main healer (kept while it lives, otherwise the lowest healer guid) takes the tanks; every other member goes to the
// healer with the fewest members so far (a tank counts 2), a healer preferably to another healer. An assignment is kept across ticks while its healer
// lives and the set of healers is unchanged; dead members keep theirs. The consumers (PartyHeal in bot_policy_turn_cast.cpp):
//  - a healer whose assigned members need a heal heals only those (and anyone in an emergency); when none of them does it switches to the member
//    in the group with the lowest health (heal_assign_switch);
//  - the main healer pre-heals the tank during the first PreHealMs of a fight (a higher threshold for the tank).
// Module.forever-bots.HealPlan = 0 turns it all off (the reactive planner and the claims then behave as before).
#pragma once

#include "party_planner.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace heal_plan {

struct Settings
{
    bool Enabled = true;
    std::uint64_t PreHealMs = 4000;         // the main healer tops the tank up for this long after the fight starts (0 = no pre-heal)
    std::uint32_t PreHealBelowPct = 96;     // the tank is topped up below this share during the pre-heal window
};

struct Plan
{
    std::uint64_t MainHealer = 0;
    std::vector<std::uint64_t> Healers;                         // the living healers, guid order
    std::unordered_map<std::uint64_t, std::uint64_t> HealerOf;  // member -> its healer
    std::vector<std::uint64_t> Tanks;                           // the members the main healer was given as tanks
    std::string Signature;                                      // changes exactly when an assignment does
};

enum class Verdict : std::uint8_t
{
    Own,     // heal it as before: it is this healer's, the plan does not cover this healer, or it is an emergency
    Skip,    // another healer's member while this healer's own members need heals
    Switch,  // another healer's member, taken because this healer's own members are all fine
};

// Rebuilds `plan` from the group's members. Returns true when an assignment changed (the caller emits heal_plan:). With no healer the plan is empty.
bool Update(Settings const& settings, std::vector<party_planner::Member> const& members, Plan& plan);

// One line for the heal_plan: event: "healers=2 main=5 5:[9] 7:[1,2,3]" (member lists cut at 10).
std::string Describe(Plan const& plan);

// Whether the plan gives `healer` any member.
bool Covers(Plan const* plan, std::uint64_t healer);

// Whether `member` is one of `healer`'s.
bool IsAssigned(Plan const* plan, std::uint64_t healer, std::uint64_t member);

// What `healer` does with `member`: `assignedNeed` is whether any of its own members needs a heal now; `pct` the member's health share;
// `ownerReady` whether the member's own healer is alive, in reach and off its global cooldown (then it takes the member itself: Skip, not Switch).
Verdict Judge(Plan const* plan, std::uint64_t healer, std::uint64_t member, bool assignedNeed, bool ownerReady, std::uint32_t pct, std::uint32_t emergencyPct);

// The tank's heal threshold: `base` normally, max(base, PreHealBelowPct) for the main healer inside the first PreHealMs of the fight (fightStartMs 0 = not started).
std::uint32_t TankThreshold(Settings const& settings, Plan const* plan, std::uint64_t healer, std::uint64_t tank, std::uint32_t base, std::uint64_t nowMs, std::uint64_t fightStartMs);

}   // namespace heal_plan
