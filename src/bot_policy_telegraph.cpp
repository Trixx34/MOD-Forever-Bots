// SPDX-License-Identifier: GPL-2.0-or-later
// Telegraph escape (layout 25): a bot inside the area of an enemy cast that is about to land (Onyxia's Fireball and Deep Breath) steps out of it, on maps that have raid rules.
#include "bot_policy_internal.h"

#include "party_view_adapter.h"

BotPolicy::TurnFlow BotPolicy::TurnTelegraphEscape(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t const nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    if (!_options.TelegraphEscape || !_options.RaidRules || bot.life_state != 0 || bot.in_flight != 0 || bot.telegraph_count == 0 || !_options.RaidRules->Find(bot.map_id))
    {
        state.TelegraphSetMs = 0;
        return TurnFlow::kNext;
    }
    raid_rules::Table const& rules = *_options.RaidRules;
    std::uint32_t const classId = ClassFor(_options, bot.guid);
    bool const tank = state.Role_ == BotState::Role::Tank || (state.Role_ == BotState::Role::Auto && classId == kWarriorClass && _options.PartySize != 0);
    bool const midCast = bot.casting_spell != 0 && bot.cast_remaining_ms != 0;

    forever_module::Telegraph const* chosen = nullptr;
    for (forever_module::Telegraph const* telegraph : party_view::TelegraphsHitting(tick.view, turn.i))
    {
        bool const lethal = rules.IsLethalSpell(bot.map_id, telegraph->spell_id);
        bool const frontal = rules.IsFrontalAny(bot.map_id, telegraph->spell_id);
        if (tank && !lethal && !frontal)
            continue;   // the tank holds the boss where it is
        if (midCast && !lethal && !_options.TelegraphEscapeCasters)
            continue;   // a healer or caster finishes the cast unless the hit would kill
        chosen = telegraph;
        break;
    }
    if (!chosen)
    {
        // Keep walking to the end point for a moment so the bot is not stopped at the edge by the next tick's snapshot.
        bool const keep = state.TelegraphSetMs != 0 && nowMs < state.TelegraphSetMs + kAvoidKeepMs &&
                          DistanceTo(bot, Point{ state.TelegraphDest.X, state.TelegraphDest.Y, bot.pos_z }) > 1.0f;
        if (!keep)
        {
            state.TelegraphSetMs = 0;
            return TurnFlow::kNext;
        }
    }
    else if (state.TelegraphSetMs == 0 || nowMs >= state.TelegraphSetMs + kAvoidKeepMs ||
             party_view::InsideTelegraph(*chosen, state.TelegraphDest.X, state.TelegraphDest.Y))
    {
        party_view::Point const out = party_view::EscapeFrom(*chosen, bot.pos_x, bot.pos_y, ground_avoid::kExitMarginYards);
        state.TelegraphDest = ground_avoid::Point2{ out.X, out.Y };
        state.TelegraphSetMs = nowMs;
        if (_options.OnEvent && nowMs - state.TelegraphLoggedMs >= 10000)
        {
            state.TelegraphLoggedMs = nowMs;
            _options.OnEvent("telegraph_escape: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " spell=" + std::to_string(chosen->spell_id) +
                             " to=" + std::to_string(int(out.X)) + "," + std::to_string(int(out.Y)));
        }
    }

    Decision const out{ ActionType::kMoveTo, 0, Point{ state.TelegraphDest.X, state.TelegraphDest.Y, bot.pos_z } };
    if (!state.Last || state.Last->Type != ActionType::kMoveTo || state.Last->Dest.X != out.Dest.X || state.Last->Dest.Y != out.Dest.Y)
    {
        BotAction move;
        move.bot_guid = bot.guid;
        move.type = ActionType::kMoveTo;
        move.dest_x = out.Dest.X;
        move.dest_y = out.Dest.Y;
        move.dest_z = out.Dest.Z;
        tick.actions.push_back(move);
        state.Last = out;
        state.LastMove = out;
    }
    return TurnFlow::kSkip;
}
