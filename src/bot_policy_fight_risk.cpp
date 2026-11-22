// SPDX-License-Identifier: GPL-2.0-or-later
// BotPolicy: the fight-risk retreat (2026-11-19, FightRiskRetreat, default off; fight_risk.h, doc/survival.md). A solo bot in the open world that is losing a fight (its
// time to die, from its health loss and the big hits of the core's damage events, is shorter than its time to kill, from its target's health loss) drinks a health potion
// or bandages (what it carries), then walks away toward the last place it stood safe; still losing after fight_risk::kDisengageMs, and with FightRiskTeleport on (which
// needs the core's Bot.Actions.TeleportInCombat = 1), it takes a rescue teleport to a grind spot for its level on the same map, never another map (the Skyborne rule:
// a bot on 2991 stays on 2991), at most once per fight and per fight_risk::kTeleportCooldownMs. Never in a group, a dungeon route, a dungeon or raid map, or on a transport.
// Event fight_retreat with stage=potion|bandage|disengage|teleport|teleport_refused|no_spot|end|died.
#include "bot_policy_internal.h"

#include <cstdio>

namespace
{
std::string Pos(float x, float y)
{
    return std::to_string(int(x)) + "," + std::to_string(int(y));
}

std::string Seconds(float s)
{
    if (s >= 1e8f)
        return "never";
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", s);
    return text;
}
}

void BotPolicy::FightRiskLine(BotTurn const& turn, char const* stage, fight_risk::Estimate const& estimate, std::string const& extra)
{
    BotSnapshot const& bot = turn.bot;
    LogEvent("fight_retreat: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) + " stage=" + stage +
             " hp=" + std::to_string(HealthPct(bot, 100)) + " in_dps=" + std::to_string(int(estimate.InDps)) + " out_dps=" + std::to_string(int(estimate.OutDps)) +
             " ttd_s=" + Seconds(estimate.TimeToDieS) + " ttk_s=" + Seconds(estimate.TimeToKillS) + " entry=" + std::to_string(turn.state.RiskEntry) + extra);
}

BotPolicy::TurnFlow BotPolicy::TurnFightRisk(DecideTick& tick, BotTurn& turn)
{
    if (!Opt().FightRisk.Enabled)
        return TurnFlow::kNext;
    std::uint64_t const nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    bool const onTransport = bot.transport_guid != 0 || (bot.movement_flags & 8u) != 0;
    bool const solo = bot.group_leader_guid == 0 && !IsRouteActive(bot.guid) && Opt().DungeonsByMap.count(bot.map_id) == 0 &&
                      !(Opt().RaidRules && Opt().RaidRules->Find(bot.map_id));
    auto const endRetreat = [&](char const* result)
    {
        if (state.RiskStep != fight_risk::Step::kNone)
            FightRiskLine(turn, "end", fight_risk::Estimate{}, std::string(" result=") + result + " last=" + fight_risk::StepName(state.RiskStep));
        state.RiskStep = fight_risk::Step::kNone;
        state.RiskWatch.Reset();
        state.RiskDisengageSinceMs = 0;
        state.RiskBandaged = false;
        state.RiskTeleportSent = false;
    };
    if (bot.life_state != 0 || bot.in_flight != 0 || !solo || onTransport)
    {
        endRetreat("not_solo");
        return TurnFlow::kNext;
    }
    // A refused rescue teleport (Bot.Actions.TeleportInCombat off in the core, or the core's own checks) is logged once; the walk away goes on.
    if (state.RiskTeleportSent && bot.last_action_type == std::uint8_t(ActionType::kTeleport) && bot.last_action_status != BotActionStatus::kApplied &&
        bot.last_action_status != BotActionStatus::kNone && bot.last_action_sequence != state.RiskTeleportSeq)
    {
        state.RiskTeleportSeq = bot.last_action_sequence;
        FightRiskLine(turn, "teleport_refused", fight_risk::Estimate{}, std::string(" status=") + ActionStatusName(bot.last_action_status));
    }
    if (bot.in_combat == 0)
    {
        endRetreat("out_of_combat");
        // The safe point: where the bot stands out of combat, healthy, with no live hostile near (a retreat walks back here).
        if (HealthPct(bot, 100) >= kRiskSafeHealthPct)
        {
            bool calm = true;
            for (NearbyUnit const& unit : turn.nearby)
                calm = calm && !(IsLiveMob(unit) && rotation::DistanceYards(bot, unit) <= kRiskSafeHostileYards);
            if (calm)
            {
                state.SafePointSet = true;
                state.SafePointMap = bot.map_id;
                state.SafePoint = Point{ bot.pos_x, bot.pos_y, bot.pos_z };
            }
        }
        return TurnFlow::kNext;
    }
    // The fight: one sample per turn, then the estimate.
    NearbyUnit const* target = nullptr;
    for (NearbyUnit const& unit : turn.nearby)
        if (unit.guid == bot.threat_target_guid)
            target = &unit;
    state.RiskWatch.Note(nowMs, bot.health, target ? target->guid : 0, target ? target->health : 0);
    std::uint32_t hitDamage = 0;
    for (damage_events::Hit const& hit : _damage.Recent(bot.guid))
        if (hit.AtMs + fight_risk::kWindowMs >= nowMs && hit.AtMs <= nowMs)
            hitDamage += hit.Amount;
    std::uint32_t const healthPct = HealthPct(bot, 100);
    fight_risk::Estimate const estimate = state.RiskWatch.Judge(Opt().FightRisk, nowMs, healthPct, hitDamage);
    bool hitRecently = false;
    for (damage_events::Hit const& hit : _damage.Recent(bot.guid, 2))
        hitRecently = hitRecently || (hit.AtMs <= nowMs && nowMs - hit.AtMs < fight_risk::kHitQuietMs);

    fight_risk::Facts facts;
    facts.Losing = estimate.Losing;
    facts.HasPotion = bot.health_potion_count > 0;
    facts.PotionReady = nowMs >= state.NextPotionMs;
    facts.HasBandage = bot.bandage_count > 0;
    facts.Bandaged = state.RiskBandaged;
    facts.HitRecently = hitRecently;
    facts.Disengaging = state.RiskDisengageSinceMs != 0;
    facts.DisengageForMs = state.RiskDisengageSinceMs != 0 ? nowMs - state.RiskDisengageSinceMs : 0;
    facts.TeleportAllowed = Opt().FightRisk.Teleport;
    facts.TeleportReady = !state.RiskTeleportSent && (state.RiskTeleportAtMs == 0 || nowMs - state.RiskTeleportAtMs >= fight_risk::kTeleportCooldownMs);
    fight_risk::Step const step = fight_risk::Next(facts);
    if (step == fight_risk::Step::kNone)
    {
        if (state.RiskStep == fight_risk::Step::kDisengage || state.RiskStep == fight_risk::Step::kTeleport)
        {
            // Not losing any more (nothing hits it): the walk away ends, the bot fights on if it is still in combat.
            FightRiskLine(turn, "end", estimate, " result=not_losing last=" + std::string(fight_risk::StepName(state.RiskStep)));
            state.RiskStep = fight_risk::Step::kNone;
            state.RiskDisengageSinceMs = 0;
        }
        return TurnFlow::kNext;
    }
    if (state.RiskStep == fight_risk::Step::kNone)
        state.RiskEntry = target ? forever_module::UnitRefEntry(target->guid) : 0;
    switch (step)
    {
        case fight_risk::Step::kPotion:
        {
            BotAction potion;
            potion.bot_guid = bot.guid;
            potion.type = ActionType::kConsume;
            potion.spell_id = 3; // a health potion (supply::PotionNow's code)
            tick.actions.push_back(potion);
            state.NextPotionMs = nowMs + Opt().Supplies.PotionEveryMs;
            state.RiskStep = step;
            FightRiskLine(turn, "potion", estimate, " potions=" + std::to_string(bot.health_potion_count));
            return TurnFlow::kNext; // it fights on
        }
        case fight_risk::Step::kBandage:
        {
            BotAction bandage;
            bandage.bot_guid = bot.guid;
            bandage.type = ActionType::kConsume;
            bandage.spell_id = 5; // a bandage (supply::BandageNow's code)
            tick.actions.push_back(bandage);
            state.RiskBandaged = true;
            state.RiskStep = step;
            FightRiskLine(turn, "bandage", estimate, " bandages=" + std::to_string(bot.bandage_count));
            return TurnFlow::kNext;
        }
        case fight_risk::Step::kTeleport:
        {
            // The nearest grind spot for its level on this map, level-safe, not dangerous, at least kRiskRescueMinYards from the fight.
            std::uint8_t const side = Opt().SideOf ? Opt().SideOf(bot.guid) : 0;
            std::optional<std::size_t> pick;
            if (Opt().Grind && !Opt().Grind->Spots.empty() && bot.level != 0)
                for (std::size_t const index : GrindSpotsFor(*Opt().Grind, bot.map_id, bot.level, side, bot.pos_x, bot.pos_y))
                {
                    GrindSpot const& spot = Opt().Grind->Spots[index];
                    if (spot.Map != bot.map_id || std::hypot(spot.X - bot.pos_x, spot.Y - bot.pos_y) < kRiskRescueMinYards ||
                        (!_levelMap.Empty() && _levelMap.TooHigh(spot.Map, spot.X, spot.Y, bot.level, Opt().LevelSafe.MarginLevels)) ||
                        _dangerMap.Dangerous(spot.Map, spot.X, spot.Y, bot.level, nowMs))
                        continue;
                    pick = index;
                    break;
                }
            state.RiskTeleportSent = true; // one try per fight, sent or not
            if (!pick)
            {
                FightRiskLine(turn, "no_spot", estimate, " pos=" + Pos(bot.pos_x, bot.pos_y));
                break; // walks away on
            }
            GrindSpot const& spot = Opt().Grind->Spots[*pick];
            BotAction rescue;
            rescue.bot_guid = bot.guid;
            rescue.type = ActionType::kTeleport;
            rescue.spell_id = bot.map_id; // the same map, always
            rescue.dest_x = spot.X;
            rescue.dest_y = spot.Y;
            rescue.dest_z = spot.Z;
            tick.actions.push_back(rescue);
            state.RiskTeleportAtMs = nowMs;
            state.RiskTeleportSeq = bot.last_action_sequence;
            state.RiskStep = step;
            state.Last.reset();
            FightRiskLine(turn, "teleport", estimate,
                          " from=" + Pos(bot.pos_x, bot.pos_y) + " spot=" + std::to_string(*pick) + " to=" + Pos(spot.X, spot.Y) + " dist=" +
                              std::to_string(int(std::hypot(spot.X - bot.pos_x, spot.Y - bot.pos_y))));
            return TurnFlow::kSkip;
        }
        default:
            break;
    }
    // Disengage: walk to the last safe point (on this map, not too far), else straight away from the target. The bot does not attack meanwhile.
    Point away{ bot.pos_x, bot.pos_y, bot.pos_z };
    bool toSafePoint = false;
    if (state.SafePointSet && state.SafePointMap == bot.map_id)
    {
        float const dist = std::hypot(state.SafePoint.X - bot.pos_x, state.SafePoint.Y - bot.pos_y);
        if (dist >= 10.0f && dist <= kRiskSafePointMaxYards)
        {
            away = state.SafePoint;
            toSafePoint = true;
        }
    }
    if (!toSafePoint)
    {
        float dx = bot.pos_x - (target ? target->pos_x : bot.pos_x - 1.0f);
        float dy = bot.pos_y - (target ? target->pos_y : bot.pos_y);
        float const len = std::hypot(dx, dy);
        if (len < 0.1f)
        {
            dx = 1.0f;
            dy = 0.0f;
        }
        else
        {
            dx /= len;
            dy /= len;
        }
        away = Point{ bot.pos_x + dx * kRiskAwayYards, bot.pos_y + dy * kRiskAwayYards, bot.pos_z };
    }
    if (state.RiskDisengageSinceMs == 0)
    {
        state.RiskDisengageSinceMs = nowMs;
        state.RiskStep = fight_risk::Step::kDisengage;
        FightRiskLine(turn, "disengage", estimate, std::string(" to=") + Pos(away.X, away.Y) + " safe_point=" + (toSafePoint ? "1" : "0"));
    }
    Decision const walk{ ActionType::kMoveTo, 0, away };
    if (!state.Last || state.Last->Type != ActionType::kMoveTo || std::hypot(state.Last->Dest.X - away.X, state.Last->Dest.Y - away.Y) > 5.0f)
    {
        BotAction move;
        move.bot_guid = bot.guid;
        move.type = ActionType::kMoveTo;
        move.dest_x = away.X;
        move.dest_y = away.Y;
        move.dest_z = away.Z;
        tick.actions.push_back(move);
        state.Last = walk;
        state.LastMove = walk;
        state.LastMoveWhy = "fight_risk_retreat";
    }
    return TurnFlow::kSkip;
}
