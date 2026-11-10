// SPDX-License-Identifier: GPL-2.0-or-later
// The party view adapter (party_view_adapter.h): layout 25 facts for the party planner and the Turn* steps.
#include "party_view_adapter.h"

#include <algorithm>
#include <cmath>

using forever_module::PartyMember;
using forever_module::PartyView;
using forever_module::Telegraph;
using forever_module::WorldTickSnapshotView;

namespace party_view {

namespace {

constexpr float kPi = 3.14159265358979f;

// An angle brought into [-pi, pi].
float Wrap(float angle)
{
    while (angle > kPi)
        angle -= 2.0f * kPi;
    while (angle < -kPi)
        angle += 2.0f * kPi;
    return angle;
}

float HalfAngleOf(Telegraph const& telegraph)
{
    float const degrees = telegraph.cone_angle > 0.0f ? telegraph.cone_angle : kDefaultConeDegrees;
    return degrees * kPi / 360.0f; // half of the full angle, in radians
}

} // namespace

std::vector<party_planner::Member> PlannerMembersOf(WorldTickSnapshotView const& view, PartyView const& party)
{
    std::vector<party_planner::Member> members;
    for (PartyMember const& member : view.MembersOf(party))
    {
        if (member.online == 0 || member.guid == 0)
            continue;
        party_planner::Member out;
        out.Guid = member.guid;
        out.Alive = member.life_state == 0;
        out.Tank = (member.role & forever_module::kPartyRoleTank) != 0;
        out.X = member.pos_x;
        out.Y = member.pos_y;
        members.push_back(out);
    }
    return members;
}

std::vector<LooseMob> LooseMobsOf(WorldTickSnapshotView const& view, PartyView const& party)
{
    std::vector<LooseMob> loose;
    std::span<PartyMember const> const members = view.MembersOf(party);
    std::size_t const count = std::min<std::size_t>(party.loose_count, forever_module::kMaxLooseMobs);
    for (std::size_t i = 0; i < count; ++i)
    {
        forever_module::LooseMob const& entry = party.loose_mobs[i];
        if (entry.unit_ref == 0)
            continue;
        LooseMob out;
        out.Unit = entry.unit_ref;
        out.Victim = entry.victim_ref;
        out.SinceMs = entry.since_ms;
        for (PartyMember const& member : members)
            if (member.guid == entry.victim_ref)
            {
                out.VictimIsHealer = (member.role & forever_module::kPartyRoleHealer) != 0;
                break;
            }
        loose.push_back(out);
    }
    return loose;
}

std::uint64_t TauntTargetFor(WorldTickSnapshotView const& view, PartyView const& party, std::uint64_t tankGuid)
{
    std::uint64_t tankVictim = 0;
    for (PartyMember const& member : view.MembersOf(party))
        if (member.guid == tankGuid)
        {
            tankVictim = member.victim_ref;
            break;
        }
    for (LooseMob const& mob : LooseMobsOf(view, party))
        if (mob.Unit != tankVictim)
            return mob.Unit;
    return 0;
}

std::optional<DispelJob> FirstDispelOf(WorldTickSnapshotView const& view, PartyView const& party, std::vector<std::uint8_t> const& types)
{
    for (PartyMember const& member : view.MembersOf(party))
    {
        if (member.online == 0 || member.life_state != 0)
            continue;
        std::size_t const count = std::min<std::size_t>(member.debuff_count, forever_module::kMaxPartyDebuffs);
        for (std::size_t i = 0; i < count; ++i)
        {
            forever_module::PartyDebuff const& debuff = member.debuffs[i];
            if (debuff.spell_id == 0 || debuff.dispel_type == 0)
                continue;
            if (!types.empty() && std::find(types.begin(), types.end(), debuff.dispel_type) == types.end())
                continue;
            DispelJob job;
            job.Member = member.guid;
            job.SpellId = debuff.spell_id;
            job.DispelType = debuff.dispel_type;
            job.RemainingMs = debuff.remaining_ms;
            return job;
        }
    }
    return std::nullopt;
}

bool InsideTelegraph(Telegraph const& telegraph, float x, float y)
{
    if (telegraph.spell_id == 0 || telegraph.radius <= 0.0f)
        return false;
    float const dx = x - telegraph.x;
    float const dy = y - telegraph.y;
    float const distance = std::sqrt(dx * dx + dy * dy);
    if (distance > telegraph.radius)
        return false;
    if (telegraph.kind != 2)
        return true;
    if (distance < 0.01f)
        return true; // on the caster: inside whatever the facing
    float const off = Wrap(std::atan2(dy, dx) - telegraph.facing);
    return std::fabs(off) <= HalfAngleOf(telegraph);
}

std::vector<Telegraph const*> TelegraphsHitting(WorldTickSnapshotView const& view, std::size_t botIndex)
{
    std::vector<Telegraph const*> hitting;
    if (botIndex >= view.bots.size())
        return hitting;
    forever_module::BotSnapshot const& bot = view.bots[botIndex];
    for (Telegraph const& telegraph : view.TelegraphsOf(botIndex))
        if (InsideTelegraph(telegraph, bot.pos_x, bot.pos_y))
            hitting.push_back(&telegraph);
    return hitting;
}

Point EscapeFrom(Telegraph const& telegraph, float x, float y, float margin)
{
    float const dx = x - telegraph.x;
    float const dy = y - telegraph.y;
    float const distance = std::sqrt(dx * dx + dy * dy);
    if (telegraph.kind != 2)
    {
        // A circle: straight away from the centre; from the centre itself, away from where the caster looks (behind it is as good a side as any).
        float const angle = distance < 0.01f ? telegraph.facing + kPi : std::atan2(dy, dx);
        float const reach = telegraph.radius + margin;
        return { telegraph.x + std::cos(angle) * reach, telegraph.y + std::sin(angle) * reach };
    }
    // A cone: step across the nearer edge, at the same distance from the caster, then `margin` yards further out from the axis. On the axis, or at
    // the caster, go out to the side at the cone's length.
    float const half = HalfAngleOf(telegraph);
    float const off = distance < 0.01f ? 0.0f : Wrap(std::atan2(dy, dx) - telegraph.facing);
    float const side = off >= 0.0f ? 1.0f : -1.0f;
    float const edge = telegraph.facing + side * half;
    float const along = distance < 0.01f ? telegraph.radius : distance;
    float const ex = telegraph.x + std::cos(edge) * along;
    float const ey = telegraph.y + std::sin(edge) * along;
    // perpendicular to the edge, away from the axis
    float const outward = edge + side * kPi / 2.0f;
    return { ex + std::cos(outward) * margin, ey + std::sin(outward) * margin };
}

} // namespace party_view
