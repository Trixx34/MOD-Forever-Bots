// SPDX-License-Identifier: GPL-2.0-or-later
// Raid rules that act on positions and auras (raid_rules.h): the `spread` and `avoid_enemy` rows (TurnRaidSpacing), the `purge` and `mindcontrol` rows (TurnRaidAuras).
// All four do nothing without a row of their kind for the bot's map, and each has a settings switch (RaidSpread, RaidAvoidEnemy, RaidPurge, RaidMindControl).
#include "bot_policy_internal.h"

#include "raid_rules.h"

namespace {

constexpr std::uint64_t kSpreadCooldownMs = 5000;   // a bot is given a new spread nudge no sooner than this after the last one began (the walk itself keeps kAvoidKeepMs)
constexpr std::uint64_t kControlRetryMs = 6000;     // a polymorph on a member is sent again no sooner than this (it lasts about 10 s on a player)
constexpr std::uint64_t kControlAvoidMs = 2000;     // how long a controlled member stays on a bot's do-not-attack list after the plan last named it
constexpr float kRaidSpellRangeYards = 30.0f;
constexpr float kArrivedYards = 1.5f;

} // namespace

namespace bot_policy_detail {

std::uint8_t SpreadKindOf(std::uint32_t classId, bool canHeal)
{
    if (canHeal)
        return raid_rules::kWhoHealers;
    switch (classId)
    {
        case 1:     // warrior
        case 2:     // paladin
        case 4:     // rogue
            return raid_rules::kWhoMelee;
        case 3:     // hunter
        case 5:     // priest
        case 7:     // shaman
        case 8:     // mage
        case 9:     // warlock
        case 11:    // druid
            return raid_rules::kWhoRanged;
        default:
            return 0;
    }
}

std::uint32_t PurgeSpellFor(std::uint32_t classId, std::uint32_t level)
{
    // Both remove a magic effect from a hostile target (SpellEffect effect 38, misc value 1: dispel type magic; the learn levels are in data/spell_costs.txt). A hunter's Tranquilizing
    // Shot 19801 removes dispel type 9 (enrage) only, and the module has no enemy-buff row for enrage, so it is not used. Mages have no purge in this client (no Spellsteal in the spell data).
    switch (classId)
    {
        case 5: return level == 0 || level >= 18 ? 527u : 0u;     // priest: Dispel Magic
        case 7: return level == 0 || level >= 12 ? 370u : 0u;     // shaman: Purge
        default: return 0;
    }
}

std::uint32_t ControlCcSpellFor(std::uint32_t classId, std::uint32_t level)
{
    // Polymorph works on a player in combat. A rogue's Sap needs a target out of combat, a priest's Shackle Undead only holds undead: neither is used.
    return classId == 8 && (level == 0 || level >= CcLearnLevel(kPolymorphSpell)) ? kPolymorphSpell : 0u;
}

} // namespace bot_policy_detail

BotPolicy::TurnFlow BotPolicy::TurnRaidSpacing(DecideTick& tick, BotTurn& turn)
{
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    if (!_options.RaidRules || (!_options.RaidSpread && !_options.RaidAvoidEnemy))
        return TurnFlow::kNext;
    raid_rules::Table const& rules = *_options.RaidRules;
    if (bot.life_state != 0 || bot.in_flight != 0 || !rules.Find(bot.map_id))
    {
        state.SpacingSetMs = 0;
        return TurnFlow::kNext;
    }
    std::uint64_t const nowMs = tick.nowMs;
    std::uint32_t const classId = ClassFor(_options, bot.guid);
    bool const tank = state.Role_ == BotState::Role::Tank || (state.Role_ == BotState::Role::Auto && classId == kWarriorClass && _options.PartySize != 0);
    bool const midCast = bot.casting_spell != 0 && bot.cast_remaining_ms != 0;

    // A walk already begun keeps its end point for a few seconds, so the next snapshot does not stop the bot at the edge.
    bool walking = state.SpacingSetMs != 0 && nowMs < state.SpacingSetMs + kAvoidKeepMs &&
                   DistanceTo(bot, Point{ state.SpacingDest.X, state.SpacingDest.Y, bot.pos_z }) > kArrivedYards;

    // avoid_enemy: a non-tank member of the row's group stays out of a circle of `yards` around a live hostile of the entry. A bot mid-cast holds unless that unit is casting.
    // avoid_enemy_aura rows: the same circle, only while the unit carries the row's aura (DecideTick::UnitAuraSeen; unknown counts as carrying it).
    if (_options.RaidAvoidEnemy && !tank && rules.HasAvoidEnemy(bot.map_id))
    {
        bool const healer = state.Role_ != BotState::Role::Dps && state.Role_ != BotState::Role::Tank && HealSpellFor(classId, bot.level) != 0;
        std::uint8_t const kind = SpreadKindOf(classId, healer);
        std::vector<ground_avoid::Circle> circles;
        bool anyCasting = false;
        std::uint32_t entry = 0;
        for (NearbyUnit const& unit : tick.view.NearbyOf(turn.i))   // turn.nearby is filled by a later step
        {
            if (!unit.is_hostile || unit.health == 0 || !IsLiveMob(unit))
                continue;
            for (raid_rules::EnemyZone const& zone : rules.AvoidEnemyFor(bot.map_id, forever_module::UnitRefEntry(unit.guid)))
                // avoid_enemy_aura: no circle once a bot targeting the unit sees a complete aura list without the aura (unknown keeps the circle)
                if ((zone.Who & kind) != 0 && (zone.Aura == 0 || tick.UnitAuraSeen(unit.guid, zone.Aura) != DecideTick::AuraSeen::kAbsent))
                {
                    circles.push_back({ unit.pos_x, unit.pos_y, zone.Yards - ground_avoid::kInsideMarginYards });   // inside = closer than radius + 1 yard = closer than `yards`
                    anyCasting = anyCasting || unit.casting != 0;
                    entry = forever_module::UnitRefEntry(unit.guid);
                }
        }
        if (!circles.empty() && ground_avoid::InsideAny(bot.pos_x, bot.pos_y, circles) && (!midCast || anyCasting))
        {
            if (!walking || ground_avoid::InsideAny(state.SpacingDest.X, state.SpacingDest.Y, circles))
                if (auto const out = ground_avoid::EscapePoint(bot.pos_x, bot.pos_y, circles))
                {
                    state.SpacingDest = *out;
                    state.SpacingSetMs = nowMs;
                    walking = true;
                    if (_options.OnEvent && nowMs - state.SpacingLoggedMs >= 10000)
                    {
                        state.SpacingLoggedMs = nowMs;
                        _options.OnEvent("raid_avoid_enemy: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " entry=" + std::to_string(entry) +
                                         " to=" + std::to_string(int(out->X)) + "," + std::to_string(int(out->Y)));
                    }
                }
        }
    }

    auto const led = tick.leaderOf.find(bot.guid);      // turn.partyLeader is filled by a later step
    std::uint64_t const leader = led == tick.leaderOf.end() ? 0 : led->second;
    // spread: the party plan names a point for a member that stands too close to another of its group; at most one nudge per kSpreadCooldownMs, never mid-cast.
    if (!walking && _options.RaidSpread && !tank && leader != 0 && bot.in_combat != 0 && !midCast && nowMs >= state.SpacingNextMs)
        if (party_planner::SpreadAssignment const* const nudge = party_planner::SpreadFor(tick.PlanOf(leader), bot.guid))
        {
            state.SpacingDest = ground_avoid::Point2{ nudge->X, nudge->Y };
            state.SpacingSetMs = nowMs;
            state.SpacingNextMs = nowMs + kSpreadCooldownMs;
            walking = true;
            if (_options.OnEvent && nowMs - state.SpacingLoggedMs >= 5000)
            {
                state.SpacingLoggedMs = nowMs;
                _options.OnEvent("raid_spread: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) +
                                 " to=" + std::to_string(int(nudge->X)) + "," + std::to_string(int(nudge->Y)));
            }
        }

    if (!walking)
    {
        state.SpacingSetMs = 0;
        return TurnFlow::kNext;
    }
    Decision const out{ ActionType::kMoveTo, 0, Point{ state.SpacingDest.X, state.SpacingDest.Y, bot.pos_z } };
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

// mindcontrol: nobody attacks a member the plan reads as controlled (it goes on every plan member's do-not-attack list, which the targeting steps honor), and the
// planned mage polymorphs it. purge: the planned priest or shaman removes the listed buff from the enemy. Each is one instant cast; both need the party plan.
BotPolicy::TurnFlow BotPolicy::TurnRaidAuras(DecideTick& tick, BotTurn& turn)
{
    if (!_options.RaidRules || (!_options.RaidMindControl && !_options.RaidPurge) || turn.partyLeader == 0)
        return TurnFlow::kNext;
    party_planner::Order const* const plan = tick.PlanOf(turn.partyLeader);
    if (plan == nullptr)
        return TurnFlow::kNext;
    std::uint64_t const nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    if (_options.RaidMindControl)
        for (std::uint64_t const member : plan->Controlled)
            if (member != bot.guid)
                state.AvoidUntilMs[PlayerRef(member)] = nowMs + kControlAvoidMs;
    if (bot.life_state != 0 || nowMs < state.NextCastMs || IsCasting(bot))
        return TurnFlow::kNext;

    if (_options.RaidMindControl)
        if (party_planner::ControlAssignment const* const job = party_planner::ControlFor(_options.PartyPlanner, plan, bot.guid, nowMs))
        {
            auto const retry = state.RetryAtMs.find(job->Spell);
            auto const victim = tick.indexOf.find(job->Target);
            if (job->Spell != 0 && (retry == state.RetryAtMs.end() || nowMs >= retry->second) && victim != tick.indexOf.end())
            {
                BotSnapshot const& member = tick.view.bots[victim->second];
                if (member.life_state == 0 && member.guid != bot.guid && Distance2d(bot, member) <= kRaidSpellRangeYards)
                {
                    BotAction hold;
                    hold.bot_guid = bot.guid;
                    hold.type = ActionType::kCastSpell;
                    hold.spell_id = job->Spell;
                    hold.target_guid = PlayerRef(member.guid);
                    tick.actions.push_back(hold);
                    state.CastTarget[job->Spell] = hold.target_guid;
                    state.RetryAtMs[job->Spell] = nowMs + kControlRetryMs;
                    state.NextCastMs = nowMs + kGlobalCooldownMs;
                    if (_options.OnEvent)
                        _options.OnEvent("raid_mindcontrol: bot=" + std::to_string(bot.guid) + " spell=" + std::to_string(job->Spell) + " target=" + std::to_string(member.guid));
                    return TurnFlow::kSkip;
                }
            }
        }

    if (_options.RaidPurge)
        if (party_planner::PurgeAssignment const* const job = party_planner::PurgeFor(_options.PartyPlanner, plan, bot.guid, nowMs))
        {
            auto const retry = state.RetryAtMs.find(job->Spell);
            NearbyUnit const* const enemy = FindLiveMob(turn.nearby, job->Target);
            if (job->Spell != 0 && (retry == state.RetryAtMs.end() || nowMs >= retry->second) && enemy != nullptr &&
                std::hypot(enemy->pos_x - bot.pos_x, enemy->pos_y - bot.pos_y) <= kRaidSpellRangeYards)
            {
                BotAction purge;
                purge.bot_guid = bot.guid;
                purge.type = ActionType::kCastSpell;
                purge.spell_id = job->Spell;
                purge.target_guid = enemy->guid;
                tick.actions.push_back(purge);
                state.CastTarget[job->Spell] = enemy->guid;
                state.RetryAtMs[job->Spell] = nowMs + kDispelRetryMs;   // the aura is still listed for a tick or two after the cast
                state.NextCastMs = nowMs + kGlobalCooldownMs;
                if (_options.OnEvent)
                    _options.OnEvent("raid_purge: bot=" + std::to_string(bot.guid) + " spell=" + std::to_string(job->Spell) + " entry=" +
                                     std::to_string(forever_module::UnitRefEntry(enemy->guid)) + " aura=" + std::to_string(job->Aura));
                return TurnFlow::kSkip;
            }
        }
    return TurnFlow::kNext;
}
