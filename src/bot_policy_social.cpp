// SPDX-License-Identifier: GPL-2.0-or-later
// Duels, battlegrounds, guild join and mail gifts (settings Duels, Battlegrounds, GuildJoin, MailGifts; each off by default): the per-bot turn that asks the
// decision modules (duels.h, battlegrounds.h, guild_join.h, mail_gifts.h) what to do and queues a social::Order, and the handling of the core's answers.
#include "bot_policy_internal.h"

namespace {

constexpr float kDuelRivalYards = 25.0f;               // a rival this close is challenged (the core allows 40 yards and needs line of sight)
constexpr std::uint64_t kSocialBusyMs = 30000;         // an order whose outcome never arrives stops blocking the bot after this long
constexpr std::uint64_t kGuildPollMs = 60000;          // a bot with no guild asks the core for a pending guild invitation this often (the snapshot has none)

}  // namespace

void BotPolicy::TurnSocial(DecideTick& tick, BotTurn& turn)
{
    if (!_options.Duels.Enabled && !_options.Battlegrounds.Enabled && !_options.GuildJoin.Enabled && !_options.MailGifts.Enabled)
        return;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    std::uint64_t const nowMs = tick.nowMs;

    // A battleground the bot stood in and has left is over: the next queue waits out the cooldown.
    if (_options.Battlegrounds.Enabled)
    {
        bool const inside = social::IsBattlegroundMap(bot.map_id);
        if (inside)
            state.BgWasInside = true;
        else if (state.BgWasInside)
        {
            state.BgWasInside = false;
            battlegrounds::NoteLeft(state.BgState, _options.Battlegrounds, nowMs);
        }
    }

    if (nowMs < state.SocialBusyUntilMs)
        return;
    // Nothing is asked of a bot in a party, flying, dead, or in a fight: the features are for a free bot standing about.
    bool const free = bot.life_state == 0 && bot.in_flight == 0 && bot.group_role == 0;
    bool const idle = turn.idle && free;
    bool const fighting = bot.in_combat != 0;

    auto const queue = [&](social::Order order)
    {
        order.Bot = bot.guid;
        _socialOrders.push_back(order);
        state.SocialBusyUntilMs = nowMs + kSocialBusyMs;
    };

    // Guild: an invitation is not in the snapshot, so a bot without a guild asks the core to accept a pending one once a minute (the core refuses with
    // "no pending guild invitation" when there is none, which is not logged). A leave was asked for by a player (NoteGuildLeaveRequest).
    if (_options.GuildJoin.Enabled)
    {
        guild::Context ctx;
        ctx.Idle = idle;
        ctx.InCombat = fighting;
        ctx.HasGuild = state.GuildHas;
        ctx.LeaveRequested = state.GuildLeaveRequested;
        ctx.InvitePending = !state.GuildHas && nowMs >= state.GuildPollAtMs;
        guild::Action const action = guild::Decide(_options.GuildJoin, ctx);
        if (action == guild::Action::Join)
        {
            state.GuildPollAtMs = nowMs + kGuildPollMs;
            social::Order order;
            order.What = social::Kind::Guild;
            order.Op = 0;   // kAccept
            queue(order);
            return;
        }
        if (action == guild::Action::Leave)
        {
            state.GuildLeaveRequested = false;
            social::Order order;
            order.What = social::Kind::Guild;
            order.Op = 2;   // kLeave
            queue(order);
            return;
        }
    }

    // Duels: a challenge to the bot is accepted (NoteDuelChallenge); otherwise an idle bot challenges the nearest player that is not one of ours.
    if (_options.Duels.Enabled)
    {
        duels::Context ctx;
        ctx.Idle = idle;
        ctx.InCombat = fighting;
        ctx.Dead = bot.life_state != 0;
        ctx.Level = bot.level;
        ctx.ChallengeReceived = state.DuelChallenger != 0;
        std::uint64_t rival = 0;
        float best = kDuelRivalYards;
        for (NearbyUnit const& unit : turn.nearby)
        {
            if (forever_module::UnitRefType(unit.guid) != kPlayerType || unit.is_hostile || tick.IsOurBot(unit.guid) || unit.health == 0)
                continue;
            float const distance = DistanceTo(bot, Point{ unit.pos_x, unit.pos_y, unit.pos_z });
            if (distance < best)
            {
                best = distance;
                rival = forever_module::UnitRefCounter(unit.guid);
            }
        }
        ctx.RivalNearby = rival != 0;
        duels::Action const action = duels::Decide(_options.Duels, state.DuelState, ctx, nowMs);
        if (action == duels::Action::Accept)
        {
            social::Order order;
            order.What = social::Kind::Duel;
            order.Op = 1;   // kAccept
            order.Player = state.DuelChallenger;
            state.DuelChallenger = 0;
            duels::NoteStarted(state.DuelState, _options.Duels, nowMs);
            queue(order);
            return;
        }
        if (action == duels::Action::Challenge)
        {
            social::Order order;
            order.What = social::Kind::Duel;
            order.Op = 0;   // kChallenge
            order.Player = rival;
            duels::NoteStarted(state.DuelState, _options.Duels, nowMs);
            queue(order);
            return;
        }
    }

    // Battlegrounds: an idle bot queues for one its level allows; a queue that has not popped in time is left.
    if (_options.Battlegrounds.Enabled)
    {
        battlegrounds::Context ctx;
        ctx.Idle = idle;
        ctx.InCombat = fighting;
        ctx.Level = bot.level;
        ctx.InBattleground = social::IsBattlegroundMap(bot.map_id);
        battlegrounds::Action const action = battlegrounds::Decide(_options.Battlegrounds, state.BgState, ctx, nowMs);
        if (action == battlegrounds::Action::Queue)
        {
            std::uint32_t const id = social::PickBattleground(bot.level, bot.guid + nowMs / 3600000);
            if (id != 0)
            {
                state.BgId = id;
                social::Order order;
                order.What = social::Kind::Battleground;
                order.Op = 0;   // kQueue
                order.Id = id;
                queue(order);
                return;
            }
        }
        else if (action == battlegrounds::Action::Leave)
        {
            social::Order order;
            order.What = social::Kind::Battleground;
            order.Op = 1;   // kLeaveQueue
            order.Id = state.BgId;
            battlegrounds::NoteLeft(state.BgState, _options.Battlegrounds, nowMs);
            queue(order);
            return;
        }
    }

    // Gifts: one small stack to a player the bot was told to thank. The bags are listed and the stack chosen when the order is sent.
    if (_options.MailGifts.Enabled)
    {
        gifts::Context ctx;
        ctx.Idle = idle;
        ctx.InCombat = fighting;
        ctx.GiftableItems = 1;   // unknown here: the sender lists the bags, and a refund follows an outcome with no gift
        ctx.RecipientKnown = state.GiftRecipient != 0;
        if (gifts::Decide(_options.MailGifts, state.GiftState, ctx, nowMs) == gifts::Action::Send)
        {
            social::Order order;
            order.What = social::Kind::Gift;
            order.Player = state.GiftRecipient;
            order.Seed = bot.guid + nowMs / gifts::kDayMs;
            state.GiftRecipient = 0;
            queue(order);
            return;
        }
    }
}

void BotPolicy::NoteSocialOutcome(social::Outcome const& outcome, std::uint64_t nowMs)
{
    auto const known = _bots.find(outcome.Bot);
    if (known == _bots.end())
        return;
    BotState& state = known->second;
    state.SocialBusyUntilMs = 0;

    if (outcome.Success)
    {
        switch (outcome.What)
        {
            case social::Kind::Battleground:
                if (outcome.Op == 0)
                    battlegrounds::NoteQueued(state.BgState, nowMs);
                break;
            case social::Kind::Guild:
                if (outcome.Op == 0)
                    state.GuildHas = true;
                else if (outcome.Op == 2)
                    state.GuildHas = false;
                break;
            default:
                break;
        }
        if (_options.OnEvent)
            _options.OnEvent(std::string("social_done: kind=") + social::GapEventName(outcome.What) + " bot=" + std::to_string(outcome.Bot) + " op=" + std::to_string(outcome.Op));
        return;
    }

    // A refusal. A queue the core refused (level, faction, deserter) waits out the cooldown; a gift that was not sent gives its daily slot back.
    if (outcome.What == social::Kind::Battleground && outcome.Op == 0)
        battlegrounds::NoteLeft(state.BgState, _options.Battlegrounds, nowMs);
    if (outcome.What == social::Kind::Gift && state.GiftState.SentToday > 0)
        --state.GiftState.SentToday;
    // The guild poll is expected to be refused nearly always ("no pending guild invitation"): not worth a line.
    if (outcome.What == social::Kind::Guild && outcome.Op == 0 && !social::IsUnknownActionReason(outcome.Reason))
        return;
    if (!_options.OnEvent)
        return;
    std::string const line = std::string(social::IsUnknownActionReason(outcome.Reason) ? social::GapEventName(outcome.What) : "social_refused") +
                             ": bot=" + std::to_string(outcome.Bot) + " op=" + std::to_string(outcome.Op) + " reason=" + outcome.Reason;
    _options.OnEvent(line);
}
