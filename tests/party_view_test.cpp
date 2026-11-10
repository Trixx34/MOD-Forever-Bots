// SPDX-License-Identifier: GPL-2.0-or-later
// Layout 25: the party view and the enemy telegraphs read from a crafted snapshot through the SDK's view helpers and the planner adapter
// (party_view_adapter.h). No core needed.
#include "party_view_adapter.h"

#include <cmath>
#include <cstdio>

using forever_module::BotSnapshot;
using forever_module::MakeUnitRef;
using forever_module::PartyMember;
using forever_module::PartyView;
using forever_module::Telegraph;
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

constexpr std::uint64_t kTank = MakeUnitRef(2, 0, 11);
constexpr std::uint64_t kHealer = MakeUnitRef(2, 0, 12);
constexpr std::uint64_t kHuman = MakeUnitRef(2, 0, 13);   // a human damage dealer in the party
constexpr std::uint64_t kOffline = MakeUnitRef(2, 0, 14);
constexpr std::uint64_t kMobA = MakeUnitRef(8, 1000, 1);
constexpr std::uint64_t kMobB = MakeUnitRef(8, 1001, 2);

PartyMember Member(std::uint64_t guid, std::uint8_t role, float x, bool online = true, bool bot = true)
{
    PartyMember m;
    m.guid = guid;
    m.role = role;
    m.pos_x = x;
    m.online = online ? 1 : 0;
    m.is_bot = bot ? 1 : 0;
    m.health = 1000;
    m.max_health = 1000;
    return m;
}

// Two bots (11 the tank, 12 the healer) in a group with a human (13) and an offline member (14); the tank is on mob A, mob B is loose on the healer.
WorldTickSnapshotView Craft()
{
    WorldTickSnapshotView view;
    view.tick_number = 5;
    view.max_nearby_units = 2;
    BotSnapshot tank;
    tank.guid = 11;
    tank.pos_x = 10.0f;
    tank.party_view_index = 0;
    tank.party_member_index = 0;
    BotSnapshot healer;
    healer.guid = 12;
    healer.pos_x = 30.0f;
    healer.pos_y = 0.0f;
    healer.party_view_index = 0;
    healer.party_member_index = 1;
    BotSnapshot loner;
    loner.guid = 15; // no group: the defaults say none
    view.bots = { tank, healer, loner };
    view.nearby.resize(view.bots.size() * view.max_nearby_units);

    PartyView party;
    party.group_id = 77;
    party.leader_ref = kTank;
    party.main_tank_ref = kTank;
    party.member_first = 0;
    party.member_count = 4;
    party.generation = 2;
    party.in_combat = 1;
    party.engaged_count = 2;
    party.loose_count = 1;
    party.loose_mobs[0].unit_ref = kMobB;
    party.loose_mobs[0].victim_ref = kHealer;
    party.loose_mobs[0].since_ms = 300;
    party.raid_marks[7] = kMobA;
    view.party_views.push_back(party);

    PartyMember tankMember = Member(kTank, forever_module::kPartyRoleTank, 10.0f);
    tankMember.victim_ref = kMobA;
    tankMember.member_flags = forever_module::kPartyMemberLeader | forever_module::kPartyMemberMainTank;
    PartyMember healerMember = Member(kHealer, forever_module::kPartyRoleHealer, 30.0f);
    healerMember.attacker_count = 1;
    healerMember.debuff_count = 2;
    healerMember.debuffs[0].spell_id = 589;      // Shadow Word: Pain, magic
    healerMember.debuffs[0].dispel_type = 1;
    healerMember.debuffs[0].remaining_ms = 9000;
    healerMember.debuffs[1].spell_id = 1604;     // Dazed, nothing to dispel
    PartyMember humanMember = Member(kHuman, forever_module::kPartyRoleDamage, 20.0f, true, false);
    humanMember.debuff_count = 1;
    humanMember.debuffs[0].spell_id = 702;       // a curse
    humanMember.debuffs[0].dispel_type = 2;
    PartyMember offlineMember = Member(kOffline, 0, 0.0f, false);
    view.party_members = { tankMember, healerMember, humanMember, offlineMember };
    return view;
}

void ViewHelpers()
{
    WorldTickSnapshotView const view = Craft();
    PartyView const* party = view.PartyViewOf(0);
    CHECK(party != nullptr && party == view.PartyViewOf(1));
    CHECK(view.PartyViewOf(2) == nullptr);   // no group
    CHECK(view.PartyViewOf(9) == nullptr);   // out of range
    if (!party)
        return;
    CHECK(party->group_id == 77 && party->main_tank_ref == kTank && party->raid_marks[7] == kMobA);
    CHECK(view.MembersOf(*party).size() == 4);
    CHECK(view.MemberOf(0) != nullptr && view.MemberOf(0)->guid == kTank);
    CHECK(view.MemberOf(1) != nullptr && view.MemberOf(1)->guid == kHealer && view.MemberOf(1)->attacker_count == 1);
    CHECK(view.MemberOf(2) == nullptr);
    // a view that points past the pool gives nothing
    PartyView broken = *party;
    broken.member_first = 3;
    broken.member_count = 2;
    CHECK(view.MembersOf(broken).empty());
}

void PlannerMembers()
{
    WorldTickSnapshotView const view = Craft();
    std::vector<party_planner::Member> const members = party_view::PlannerMembersOf(view, view.party_views[0]);
    CHECK(members.size() == 3);   // the offline member is not a planner input
    if (members.size() != 3)
        return;
    CHECK(members[0].Guid == kTank && members[0].Tank && members[0].Alive && members[0].X == 10.0f);
    CHECK(members[1].Guid == kHealer && !members[1].Tank);
    CHECK(members[2].Guid == kHuman && !members[2].Tank && members[2].X == 20.0f);   // the human is in the plan too
    // the module's own knowledge stays at the defaults for the caller to fill
    CHECK(!members[0].CanInterrupt && members[0].CcSpell == 0 && members[0].Holding == 0);
    // a planner call accepts the list as it is
    party_planner::Settings settings;
    settings.Enabled = true;
    party_planner::Order order;
    CHECK(party_planner::Plan(settings, members, {}, 1000, order));
}

void LooseMobsAndTaunt()
{
    WorldTickSnapshotView const view = Craft();
    PartyView const& party = view.party_views[0];
    std::vector<party_view::LooseMob> const loose = party_view::LooseMobsOf(view, party);
    CHECK(loose.size() == 1);
    if (loose.size() == 1)
        CHECK(loose[0].Unit == kMobB && loose[0].Victim == kHealer && loose[0].SinceMs == 300 && loose[0].VictimIsHealer);
    CHECK(party_view::TauntTargetFor(view, party, kTank) == kMobB);
    // the tank's own victim is never its taunt target
    WorldTickSnapshotView onOwn = view;
    onOwn.party_views[0].loose_mobs[0].unit_ref = kMobA;
    CHECK(party_view::TauntTargetFor(onOwn, onOwn.party_views[0], kTank) == 0);
    // no loose mobs (Bot.Snapshot.PartyThreat off): nothing
    WorldTickSnapshotView quiet = view;
    quiet.party_views[0].loose_count = 0;
    CHECK(party_view::LooseMobsOf(quiet, quiet.party_views[0]).empty());
    CHECK(party_view::TauntTargetFor(quiet, quiet.party_views[0], kTank) == 0);
}

void Dispels()
{
    WorldTickSnapshotView const view = Craft();
    PartyView const& party = view.party_views[0];
    std::optional<party_view::DispelJob> const any = party_view::FirstDispelOf(view, party);
    CHECK(any.has_value());
    if (any)
        CHECK(any->Member == kHealer && any->SpellId == 589 && any->DispelType == 1 && any->RemainingMs == 9000);
    // a druid that removes curses only: the human's curse
    std::optional<party_view::DispelJob> const curse = party_view::FirstDispelOf(view, party, { 2 });
    CHECK(curse.has_value() && curse->Member == kHuman && curse->SpellId == 702);
    // nothing of the kind
    CHECK(!party_view::FirstDispelOf(view, party, { 4 }).has_value());
    // a dead member is not dispelled
    WorldTickSnapshotView dead = view;
    dead.party_members[1].life_state = 1;
    std::optional<party_view::DispelJob> const next = party_view::FirstDispelOf(dead, dead.party_views[0]);
    CHECK(next.has_value() && next->Member == kHuman);
}

bool Near(float a, float b, float eps = 0.05f)
{
    return std::fabs(a - b) <= eps;
}

void TelegraphGeometry()
{
    Telegraph circle;
    circle.spell_id = 11829;
    circle.kind = 1;
    circle.x = 100.0f;
    circle.y = 100.0f;
    circle.radius = 8.0f;
    CHECK(party_view::InsideTelegraph(circle, 104.0f, 100.0f));
    CHECK(!party_view::InsideTelegraph(circle, 109.0f, 100.0f));
    party_view::Point const out = party_view::EscapeFrom(circle, 104.0f, 100.0f, 3.0f);
    CHECK(Near(out.X, 111.0f) && Near(out.Y, 100.0f));
    CHECK(!party_view::InsideTelegraph(circle, out.X, out.Y));
    // from the centre: a step of radius + margin, away from the facing
    circle.facing = 0.0f;
    party_view::Point const fromCentre = party_view::EscapeFrom(circle, 100.0f, 100.0f, 2.0f);
    CHECK(Near(fromCentre.X, 90.0f) && Near(fromCentre.Y, 100.0f));

    Telegraph cone;
    cone.spell_id = 15847;
    cone.kind = 2;
    cone.x = 0.0f;
    cone.y = 0.0f;
    cone.radius = 10.0f;
    cone.facing = 0.0f;       // looks along +X
    cone.cone_angle = 60.0f;  // 30 degrees each side
    CHECK(party_view::InsideTelegraph(cone, 5.0f, 1.0f));      // ahead, a little to the side
    CHECK(!party_view::InsideTelegraph(cone, 5.0f, 5.0f));     // 45 degrees off: outside
    CHECK(!party_view::InsideTelegraph(cone, -5.0f, 0.0f));    // behind
    CHECK(!party_view::InsideTelegraph(cone, 12.0f, 0.0f));    // beyond the length
    CHECK(party_view::InsideTelegraph(cone, 0.0f, 0.0f));      // on the caster
    // the default angle (0) is 90 degrees: 40 degrees off is inside
    cone.cone_angle = 0.0f;
    CHECK(party_view::InsideTelegraph(cone, 5.0f, 4.0f));
    cone.cone_angle = 60.0f;
    // escape goes across the nearer edge and ends outside
    party_view::Point const side = party_view::EscapeFrom(cone, 5.0f, 1.0f, 2.0f);
    CHECK(!party_view::InsideTelegraph(cone, side.X, side.Y));
    CHECK(side.Y > 1.0f);   // out to the +Y side, the nearer edge
    party_view::Point const other = party_view::EscapeFrom(cone, 5.0f, -1.0f, 2.0f);
    CHECK(!party_view::InsideTelegraph(cone, other.X, other.Y) && other.Y < -1.0f);
    // an unused slot hits nothing
    Telegraph none;
    CHECK(!party_view::InsideTelegraph(none, 0.0f, 0.0f));
}

void TelegraphsOfBot()
{
    WorldTickSnapshotView view = Craft();
    BotSnapshot& healer = view.bots[1];   // at (30, 0)
    healer.telegraph_count = 2;
    healer.telegraphs[0].spell_id = 1;
    healer.telegraphs[0].kind = 3;
    healer.telegraphs[0].x = 32.0f;
    healer.telegraphs[0].y = 0.0f;
    healer.telegraphs[0].radius = 5.0f;        // around a caster next to the healer: hits
    healer.telegraphs[0].cast_remaining_ms = 400;
    healer.telegraphs[1].spell_id = 2;
    healer.telegraphs[1].kind = 1;
    healer.telegraphs[1].x = 60.0f;
    healer.telegraphs[1].y = 0.0f;
    healer.telegraphs[1].radius = 5.0f;        // far away: misses
    healer.telegraphs[2].spell_id = 3;         // past telegraph_count: not read
    healer.telegraphs[2].kind = 1;
    healer.telegraphs[2].x = 30.0f;
    healer.telegraphs[2].radius = 50.0f;
    CHECK(view.TelegraphsOf(1).size() == 2);
    std::vector<Telegraph const*> const hitting = party_view::TelegraphsHitting(view, 1);
    CHECK(hitting.size() == 1 && hitting[0]->spell_id == 1);
    CHECK(party_view::TelegraphsHitting(view, 0).empty());
    CHECK(party_view::TelegraphsHitting(view, 9).empty());
}

} // namespace

int main()
{
    ViewHelpers();
    PlannerMembers();
    LooseMobsAndTaunt();
    Dispels();
    TelegraphGeometry();
    TelegraphsOfBot();
    if (failures == 0)
    {
        std::printf("party_view_test: all checks passed\n");
        return 0;
    }
    return 1;
}
