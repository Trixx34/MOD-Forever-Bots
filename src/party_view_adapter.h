// SPDX-License-Identifier: GPL-2.0-or-later
// ADAPTER for the party planner, layout 25 (SDK protocol 26). The planner (party_planner.h) is not changed here: this file turns the core's shared
// party view (forever_module::PartyView, PartyMember: the whole group, humans included, with roles, health, casts, debuffs and the loose mobs) and the
// enemy telegraphs (forever_module::Telegraph) into the planner's Member inputs and into a few plain facts the Turn* steps can read when the planner
// adopts them (taunt target, dispel job, hazard escape). Pure functions over the snapshot copy, no clock, no file access; tests/party_view_test.cpp.
//
// How the planner is meant to adopt it (not done here, feature/party-planner is another branch): in BotPolicy::PlanParties, when
// view.PartyViewOf(leaderIndex) is not null, start from PlannerMembersOf() instead of the bots-only loop (it lists human members too), then fill
// CanInterrupt / InterruptReady / CcSpell / CcReady / Holding for the module's own bots as today; LooseMobsOf() feeds an Order::TauntTarget rule and
// FirstDispelOf() a dispel assignment. With party_view_index == kNoPartyView (the core's Bot.Snapshot.PartyView off, or an older core) everything
// falls back to the bots-only path, so the switch-over is a no-op until the core fills the view.
#pragma once

#include "party_planner.h"

#include "forever_module/tier2_session.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace party_view {

// The planner's members from the view: one per online member of the group, bots and humans alike. Guid, Alive, Tank, X and Y come from the view;
// the rest (what the module knows about its own bots: interrupt rows, crowd control, cooldowns) is left at its defaults for the caller to fill.
std::vector<party_planner::Member> PlannerMembersOf(forever_module::WorldTickSnapshotView const& view, forever_module::PartyView const& party);

// A creature on a member who is not a tank, as the view lists them (most urgent first: on a healer, then the newest).
struct LooseMob
{
    std::uint64_t Unit = 0;
    std::uint64_t Victim = 0;
    std::uint32_t SinceMs = 0;
    bool VictimIsHealer = false;
};
std::vector<LooseMob> LooseMobsOf(forever_module::WorldTickSnapshotView const& view, forever_module::PartyView const& party);

// The unit a tank should taunt: the first loose mob that is not the tank's own victim already, 0 for none. Needs the core's Bot.Snapshot.PartyThreat.
std::uint64_t TauntTargetFor(forever_module::WorldTickSnapshotView const& view, forever_module::PartyView const& party, std::uint64_t tankGuid);

// A dispel a healer could do: the first member with a dispellable debuff (the view lists those first), with the debuff. Members are scanned in slot
// order; a member whose debuff is of a type in `types` (PartyDebuff::dispel_type values: 1 magic, 2 curse, 3 disease, 4 poison; empty = any) is taken.
struct DispelJob
{
    std::uint64_t Member = 0;
    std::uint32_t SpellId = 0;
    std::uint8_t DispelType = 0;
    std::uint32_t RemainingMs = 0;
};
std::optional<DispelJob> FirstDispelOf(forever_module::WorldTickSnapshotView const& view, forever_module::PartyView const& party,
                                       std::vector<std::uint8_t> const& types = {});

// Telegraph geometry. A cone's cone_angle is in degrees (0 = the spell system's 90); facing is the caster's orientation in radians, the WoW way
// (0 = +X, counter-clockwise).
constexpr float kDefaultConeDegrees = 90.0f;
bool InsideTelegraph(forever_module::Telegraph const& telegraph, float x, float y);

// The telegraphs of bots[botIndex] that the bot stands in, soonest to land first (the snapshot's order).
std::vector<forever_module::Telegraph const*> TelegraphsHitting(forever_module::WorldTickSnapshotView const& view, std::size_t botIndex);

// Where to step to be out of `telegraph` from (x, y): a circle is left radially, a cone across its nearer edge, each by `margin` yards past the edge.
struct Point
{
    float X = 0.0f;
    float Y = 0.0f;
};
Point EscapeFrom(forever_module::Telegraph const& telegraph, float x, float y, float margin);

} // namespace party_view
