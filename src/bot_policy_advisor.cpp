// SPDX-License-Identifier: GPL-2.0-or-later
// The advisor features that need the policy's state (LFG queue fill, trade ads, party mana, guild milestones, city inspection, quest invites): each is a pure
// decision module (lfg_queue_fill.h and so on) asked here. All are off by default; each acts only for a bot that is out of combat, has no target and sees no
// hostile. The orders they make are sent by advisor_send.h.
#include "bot_policy_internal.h"

std::vector<advisor::Order> BotPolicy::TakeAdvisorOrders()
{
    std::vector<advisor::Order> out;
    out.swap(_advisorOrders);
    return out;
}

namespace {

level_dungeon::Role RoleOfBotGuid(PolicyOptions const& options, std::uint64_t guid)
{
    std::uint32_t const classId = ClassFor(options, guid);
    return classId == 0 ? level_dungeon::Role::Dps : level_dungeon::RoleOfClass(classId);
}

std::uint32_t RoleFlagOf(level_dungeon::Role role)
{
    return role == level_dungeon::Role::Tank ? lfg_queue_fill::kTank : role == level_dungeon::Role::Healer ? lfg_queue_fill::kHealer : lfg_queue_fill::kDps;
}

}  // namespace

void BotPolicy::OnLfgQueue(forever_module::WorldTickSnapshotView const& view, lfg_queue_fill::Queue const& queue, std::uint64_t nowMs)
{
    std::uint32_t const role = lfg_queue_fill::RoleToFill(_options.LfgQueueFill, queue);
    if (role == 0 || !_lfgQueueLimiter.Ready(queue.MapId, nowMs, _options.LfgQueueFill.CooldownSec))
        return;
    for (std::size_t i = 0; i < view.bots.size(); ++i)
    {
        forever_module::BotSnapshot const& bot = view.bots[i];
        // Idle: alive, out of combat, no target, no group, not flying, and no hostile in view.
        if (bot.life_state != 0 || bot.in_combat != 0 || bot.threat_target_guid != 0 || bot.group_role != 0 || bot.in_flight != 0 || bot.group_invite_from != 0)
            continue;
        if (_options.IsAlt && _options.IsAlt(bot.guid))
            continue;
        auto const state = _bots.find(bot.guid);
        if (state != _bots.end())
        {
            BotState const& s = state->second;
            if (s.Traveling || s.SupplyTrip || s.ChatStay || s.ChatFollow != 0 || s.LevelRun || s.TankLead || s.LevelHoldUntilMs > nowMs)
                continue;
        }
        bool hostile = false;
        for (forever_module::NearbyUnit const& unit : view.NearbyOf(i))
            hostile = hostile || unit.is_hostile;
        if (hostile || RoleFlagOf(RoleOfBotGuid(_options, bot.guid)) != role)
            continue;
        advisor::Order order;
        order.What = advisor::Kind::LfgJoin;
        order.Bot = bot.guid;
        order.Arg = role;
        order.Map = queue.MapId;
        _advisorOrders.push_back(order);
        _lfgQueueLimiter.Note(queue.MapId, nowMs);
        if (_options.OnEvent)
            _options.OnEvent("lfg_queue_fill: bot=" + std::to_string(bot.guid) + " role=" + std::to_string(role) + " map=" + std::to_string(queue.MapId) +
                             " waited_s=" + std::to_string(queue.QueuedSec));
        return;
    }
}
