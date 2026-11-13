// BotPolicy: planned healing (heal_plan.h, doc/planned-healing.md). The standing heal assignment of each party, kept next to the party planner's plan;
// PartyHeal (bot_policy_turn_cast.cpp) reads it.
#include "bot_policy_internal.h"

namespace
{
std::string Hex(std::uint64_t v)
{
    return std::to_string(v);
}
}   // namespace

heal_plan::Plan const* BotPolicy::HealPlanOf(std::uint64_t leaderGuid) const
{
    if (!_options.HealPlan.Enabled || !_options.PartyPlanner.Enabled)
        return nullptr;
    auto const at = _healPlans.find(leaderGuid);
    return at == _healPlans.end() || at->second.Healers.empty() ? nullptr : &at->second;
}

std::uint32_t BotPolicy::HealPlanTankThreshold(heal_plan::Plan const* plan, std::uint64_t healer, std::uint64_t tank, std::uint32_t base, std::uint64_t nowMs) const
{
    if (!plan)
        return base;
    auto const leader = _bots.find(tank);
    std::uint64_t const started = leader != _bots.end() && leader->second.InFight ? leader->second.FightStartMs : 0;
    return heal_plan::TankThreshold(_options.HealPlan, plan, healer, tank, base, nowMs, started);
}

void BotPolicy::UpdateHealPlan(std::uint64_t leaderGuid, std::vector<party_planner::Member> const& members)
{
    if (!_options.HealPlan.Enabled)
        return;
    if (heal_plan::Update(_options.HealPlan, members, _healPlans[leaderGuid]) && _options.OnEvent)
        _options.OnEvent("heal_plan: leader=" + Hex(leaderGuid) + " " + heal_plan::Describe(_healPlans[leaderGuid]));
}

void BotPolicy::PruneHealPlans(std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> const& membersOf)
{
    for (auto it = _healPlans.begin(); it != _healPlans.end();)
        it = membersOf.count(it->first) ? std::next(it) : _healPlans.erase(it);
}

// Whether the member's own healer is alive, within heal range of it and off its global cooldown: it will take the member in its own turn.
bool BotPolicy::OwnerCanHeal(DecideTick const& tick, heal_plan::Plan const& plan, forever_module::BotSnapshot const& member) const
{
    auto const owner = plan.HealerOf.find(member.guid);
    if (owner == plan.HealerOf.end())
        return false;
    auto const at = tick.indexOf.find(owner->second);
    if (at == tick.indexOf.end())
        return false;
    forever_module::BotSnapshot const& healer = tick.view.bots[at->second];
    if (healer.life_state != 0 || Distance2d(healer, member) > kHealRangeYards)
        return false;
    auto const state = _bots.find(owner->second);
    return state == _bots.end() || tick.nowMs >= state->second.NextCastMs;
}
