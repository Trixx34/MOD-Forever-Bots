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

void BotPolicy::TurnAdvisor(DecideTick& tick, BotTurn& turn)
{
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    std::uint64_t const nowMs = tick.nowMs;
    // Out of combat, no target, nothing hostile in view, alive and on the ground: the only bots the advisor features speak or emote for.
    bool const quiet = bot.in_combat == 0 && bot.life_state == 0 && bot.in_flight == 0 && !turn.hasTarget && !turn.enemyNear && bot.threat_target_guid == 0;
    if (!quiet)
        return;
    std::uint64_t const seed = _options.RandomSeed;

    // Party mana sync (PartyMana): a low healer or caster of a dungeon party says so once; the party's walk holds for it (RouteReadiness asks the tracker).
    if (_options.PartyMana.Enabled && bot.group_role != 0 && _options.DungeonsByMap.count(bot.map_id) != 0)
    {
        std::uint32_t const classId = ClassFor(_options, bot.guid);
        bool const caster = classId == 5 || classId == 7 || classId == 8 || classId == 9 || classId == 11 || HealSpellFor(classId, 99) != 0;
        party_mana::Facts facts;
        facts.InDungeonParty = true;
        facts.ManaUser = caster && bot.power_type == 0;
        facts.InCombat = bot.in_combat != 0;
        facts.Power = bot.power;
        facts.MaxPower = bot.max_power;
        std::uint64_t const leader = bot.group_role == 2 ? bot.guid : (bot.group_leader_guid & forever_module::kUnitRefCounterMask);
        if (_partyMana[leader].Note(_options.PartyMana, facts, bot.guid, nowMs) && _options.Pools && !_options.Pools->PartyMana.empty())
        {
            advisor::Order order;
            order.What = advisor::Kind::PartySay;
            order.Bot = bot.guid;
            std::vector<std::string> const& lines = _options.Pools->PartyMana;
            order.Text = lines[bot_random::Below(bot_random::Draw(seed, bot.guid, 0xF1, nowMs / 1000), std::uint32_t(lines.size()))];
            _advisorOrders.push_back(std::move(order));
        }
    }

    // Trade-chat crafting ad (TradeAds): a bot with a crafting skill posts a templated line now and then. Not in a party (the Trade channel is the zone's).
    if (_options.TradeAds.Enabled && _options.Pools && bot.group_role == 0 && !_options.Pools->TradeAds.empty())
    {
        std::uint32_t const lines[2] = { bot.profession_skill[0], bot.profession_skill[1] };
        std::uint32_t const values[2] = { bot.profession_value[0], bot.profession_value[1] };
        std::string line = trade_ads::Next(_options.TradeAds, state.TradeAds, _options.Pools->TradeAds, lines, values, seed, bot.guid, nowMs);
        if (!line.empty())
        {
            advisor::Order order;
            order.What = advisor::Kind::TradeSay;
            order.Bot = bot.guid;
            order.Text = std::move(line);
            _advisorOrders.push_back(std::move(order));
        }
    }
}

void BotPolicy::OnGuildMemberLevel(forever_module::WorldTickSnapshotView const& view, std::uint64_t guild, std::uint64_t member, std::uint32_t level,
                                   std::uint64_t nowMs)
{
    if (!_options.GuildMilestone.Enabled || !_options.Pools || _options.Pools->GuildMilestones.empty() || _bots.count(member) != 0)
        return;   // off, no lines, or the member is one of our bots
    std::vector<std::uint64_t> members;
    for (forever_module::BotSnapshot const& bot : view.bots)
    {
        auto const known = _botGuild.find(bot.guid);
        if (known != _botGuild.end() && known->second == guild && bot.life_state == 0 && bot.in_combat == 0)
            members.push_back(bot.guid);
    }
    std::uint64_t const speaker = _guildSpeaker.Pick(_options.GuildMilestone, guild, member, level, members, _options.RandomSeed);
    if (speaker == 0)
        return;
    std::vector<std::string> const& lines = _options.Pools->GuildMilestones;
    advisor::Order order;
    order.What = advisor::Kind::GuildSay;
    order.Bot = speaker;
    order.Text = advisor::Replace(lines[bot_random::Below(bot_random::Draw(_options.RandomSeed, speaker, 0xE3, nowMs / 1000 + member), std::uint32_t(lines.size()))],
                                  "level", std::to_string(level));
    _advisorOrders.push_back(std::move(order));
}
