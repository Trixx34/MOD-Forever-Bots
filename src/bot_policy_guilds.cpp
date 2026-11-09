// SPDX-License-Identifier: GPL-2.0-or-later
// Bot guilds (GuildsEnabled, off by default): the policy's pool-level turn. bot_guilds.h decides who joins which guild (PlanMoves); this turns the plan into
// social orders: a create (the master names a new guild), an invite (a bot of the guild invites the bot), an accept (the invited bot answers a few seconds
// later) and a leave. One plan per kGuildPlanMs; each order goes to a bot that has no order in flight, and a plan queues at most kGuildOrdersPerPlan.
#include "bot_policy_internal.h"

#include <algorithm>
#include <iterator>
#include <vector>

namespace {
constexpr std::uint64_t kGuildPlanMs = 30000;          // the pool is planned this often
constexpr std::uint64_t kGuildAcceptDelayMs = 3000;    // an invited bot accepts this long after the invite was queued
constexpr std::uint64_t kGuildPendingMs = 60000;       // a create whose guild is not in any snapshot after this long is forgotten (the planner tries again)
constexpr std::uint64_t kGuildOrderBusyMs = 30000;     // a bot with a guild order in flight is left alone for this long
constexpr std::size_t kGuildOrdersPerPlan = 6;
}  // namespace

void BotPolicy::DriveGuilds(forever_module::WorldTickSnapshotView const& view, std::uint64_t nowMs)
{
    if (!_options.Guilds.Enabled)
        return;

    auto const busy = [&](std::uint64_t guid)
    {
        auto const known = _bots.find(guid);
        return known != _bots.end() && known->second.SocialBusyUntilMs > nowMs;
    };
    auto const queue = [&](social::Order order)
    {
        if (auto const known = _bots.find(order.Bot); known != _bots.end())
            known->second.SocialBusyUntilMs = nowMs + kGuildOrderBusyMs;
        _socialOrders.push_back(std::move(order));
    };

    // The accepts that fall due. A refused accept (the invite did not go through) is not logged: "no pending guild invitation" is expected then.
    for (auto it = _guildAcceptDueMs.begin(); it != _guildAcceptDueMs.end();)
    {
        auto const bot = std::find_if(view.bots.begin(), view.bots.end(), [&](BotSnapshot const& b) { return b.guid == it->first; });
        if (bot == view.bots.end())
        {
            it = _guildAcceptDueMs.erase(it);   // offline or gone: the invitation is left to the core
            continue;
        }
        if (nowMs < it->second || busy(it->first))
        {
            ++it;
            continue;
        }
        if (bot->guild_guid == 0)
        {
            social::Order accept;
            accept.What = social::Kind::Guild;
            accept.Op = 0;
            accept.Bot = it->first;
            queue(accept);
        }
        it = _guildAcceptDueMs.erase(it);
    }

    if (nowMs < _guildPlanAtMs)
        return;
    _guildPlanAtMs = nowMs + kGuildPlanMs;

    // A create becomes a known guild once its master's snapshot shows the guild id; a create nobody confirms is forgotten.
    for (BotSnapshot const& bot : view.bots)
    {
        auto const pending = _pendingGuilds.find(bot.guid);
        if (pending == _pendingGuilds.end() || bot.guild_guid == 0)
            continue;
        _botGuilds[bot.guild_guid] = bot_guilds::Guild{ bot.guild_guid, pending->second.Kind, pending->second.Alliance };
        _pendingGuilds.erase(pending);
    }
    for (auto it = _pendingGuilds.begin(); it != _pendingGuilds.end();)
        it = nowMs - it->second.SinceMs > kGuildPendingMs ? _pendingGuilds.erase(it) : std::next(it);

    std::vector<bot_guilds::Guild> guilds;
    guilds.reserve(_botGuilds.size());
    for (auto const& entry : _botGuilds)
        guilds.push_back(entry.second);
    std::sort(guilds.begin(), guilds.end(), [](bot_guilds::Guild const& a, bot_guilds::Guild const& b) { return a.Guid < b.Guid; });

    std::vector<bot_guilds::Bot> pool;
    pool.reserve(view.bots.size());
    for (BotSnapshot const& bot : view.bots)
    {
        bot_guilds::Bot member;
        member.Guid = bot.guid;
        member.Guild = bot.guild_guid;
        member.Alliance = _options.SideOf && _options.SideOf(bot.guid) == 1;
        member.Online = true;
        member.Level = bot.level;
        pool.push_back(member);
    }

    bot_guilds::Plan const plan = bot_guilds::PlanMoves(_options.Guilds, guilds, pool);
    for (std::uint64_t const guild : plan.Dissolved)
        _botGuilds.erase(guild);

    std::size_t queued = 0;

    // Creates: the master (the lowest guid of the new guild's members) names it. Its other members join once the guild is known.
    for (bot_guilds::NewGuild const& created : plan.Created)
    {
        if (queued >= kGuildOrdersPerPlan || created.Members.empty())
            break;
        std::uint64_t const master = created.Master;
        if (busy(master))
            continue;
        social::Order order;
        order.What = social::Kind::Guild;
        order.Op = 3;
        order.Bot = master;
        order.Text = created.Name;
        queue(order);
        _pendingGuilds[master] = PendingGuild{ created.Kind, created.Alliance, nowMs };
        ++queued;
    }

    // Moves: a bot in a guild leaves it first (a switch is a leave now and a join on a later plan); a bot with no guild is invited into its guild by the
    // lowest-guid member that is online and free. Joins into a guild created in this plan wait for the next plan, when the guild's id is known.
    for (bot_guilds::Move const& move : plan.Moves)
    {
        if (queued >= kGuildOrdersPerPlan)
            break;
        if (move.NewGuild >= 0)
            continue;
        if (move.From != 0)
        {
            if (busy(move.Bot))
                continue;
            social::Order leave;
            leave.What = social::Kind::Guild;
            leave.Op = 2;
            leave.Bot = move.Bot;
            queue(leave);
            ++queued;
            continue;
        }
        if (move.To == 0)
            continue;
        std::uint64_t inviter = 0;   // the lowest guid of the guild's online members that is free: the create's master, normally
        for (BotSnapshot const& member : view.bots)
        {
            if (member.guild_guid == move.To && member.guid != move.Bot && !busy(member.guid) && (inviter == 0 || member.guid < inviter))
                inviter = member.guid;
        }
        if (inviter == 0 || busy(move.Bot))
            continue;
        social::Order invite;
        invite.What = social::Kind::Guild;
        invite.Op = 4;
        invite.Bot = inviter;
        invite.Player = move.Bot;
        queue(invite);
        _guildAcceptDueMs[move.Bot] = nowMs + kGuildAcceptDelayMs;
        ++queued;
    }
}
