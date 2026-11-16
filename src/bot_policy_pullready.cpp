// SPDX-License-Identifier: GPL-2.0-or-later
// BotPolicy, the ready gate before a pull for bots that are not in a route party (leveling, questing): wait for mana and for the upkeep buffs, for a bounded time. The route
// parties' gate is in bot_policy_turn_move.cpp (pull_hold); this one logs pull_wait. Keys SoloPullManaPct, PullBuffGate, PullWaitMaxMs (doc/ai-plan-next.md).
#include "bot_policy_internal.h"

using namespace forever_module;

// Whether `who` has an upkeep buff (a buff row of its rotation) to cast: not on the bot, not refused a moment ago, affordable, allowed by the row's condition. Pets (no_pet rows) are
// not a buff here. Looked at most once per kBuffDueEveryMs per bot: the answer is kept in its state.
bool BotPolicy::UpkeepBuffDue(BotSnapshot const& who, BotState& state, std::uint64_t nowMs)
{
    if (state.BuffDueCheckedMs != 0 && nowMs - state.BuffDueCheckedMs < kBuffDueEveryMs)
        return state.BuffDue;
    state.BuffDueCheckedMs = nowMs;
    state.BuffDue = false;
    if (who.life_state != 0 || who.in_combat != 0 || who.level == 0)
        return false;
    std::uint32_t const classId = ClassFor(_options, who.guid);
    rotation::Facts const facts = rotation::FactsFor(who, std::span<NearbyUnit const>{}, nullptr, 0);
    for (rotation::Row const& row : RowsFor(classId, SpecFor(who.guid, classId)))
    {
        if (!row.Buff || !RowInLevelRange(row, who.level) || rotation::RowHasCondition(row, rotation::Cond::NoPet) || !rotation::Allowed(row, facts))
            continue;
        if (auto const retry = state.RetryAtMs.find(row.Spell); retry != state.RetryAtMs.end() && nowMs < retry->second)
            continue;
        if (!CanPay(_options.SpellCosts.get(), row.Spell, who) || AuraRunning(who, row.Spell, row.Ranks))
            continue;
        state.BuffDue = true;
        break;
    }
    return state.BuffDue;
}

// A solo bot out of combat with no target of its own waits before it starts a fight when it is short of mana (SoloPullManaPct: it sits and drinks, up to the mark plus
// pull_ready::kManaReleaseGapPct) or has an upkeep buff to cast (PullBuffGate: it stands still and TurnUpkeepBuffs casts it), at most PullWaitMaxMs each; the wait then ends and
// does not start again until the need has been over once (pull_wait_timeout). It uses the solo hurt-rest path (no target, no quest walk, away from a close creature).
void BotPolicy::TurnSoloReady(DecideTick& tick, BotTurn& turn)
{
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    std::uint64_t const nowMs = tick.nowMs;
    turn.pullWaitMana = false;
    turn.pullWait = false;
    bool const solo = turn.partyLeader == 0 && !turn.routeOn && bot.in_combat == 0 && bot.threat_target_guid == 0 && bot.life_state == 0 && state.ForcedTarget == 0 &&
                      !state.ChatStay && state.ChatFollow == 0 && bot.group_leader_guid == 0;
    if (!solo || (_options.SoloPullManaPct == 0 && !_options.PullBuffGate))
    {
        state.SoloManaGate = pull_ready::Gate{};
        state.SoloBuffGate = pull_ready::Gate{};
        return;
    }
    bool timedOut = false;
    auto const note = [&](pull_ready::Gate& gate, char const* reason, bool timeout)
    {
        if (!_options.OnEvent)
            return;
        if (timeout)
            _options.OnEvent("pull_wait_timeout: bot=" + std::to_string(bot.guid) + " reason=" + reason + " level=" + std::to_string(bot.level) + " wait_ms=" + std::to_string(nowMs - gate.SinceMs));
        else if (pull_ready::LogDue(gate, nowMs))
            _options.OnEvent("pull_wait: bot=" + std::to_string(bot.guid) + " reason=" + reason + " level=" + std::to_string(bot.level) + " hp_pct=" + std::to_string(HealthPct(bot, 100)) +
                             " mana_pct=" + std::to_string(bot.max_power > 0 ? bot.power * 100 / bot.max_power : 100) + " wait_ms=" + std::to_string(nowMs - gate.SinceMs));
    };
    bool const manaUser = bot.power_type == 0 && bot.max_power > 0;
    bool const manaNeeded = manaUser && pull_ready::ManaNeeded(bot.power, bot.max_power, _options.SoloPullManaPct, state.SoloManaGate.SinceMs != 0);
    if (pull_ready::Hold(state.SoloManaGate, manaNeeded, nowMs, _options.PullWaitMaxMs, timedOut))
    {
        turn.pullWaitMana = true;
        turn.pullWait = true;
        turn.hurtRest = true;
        turn.lowHealth = true;
        note(state.SoloManaGate, "mana", false);
    }
    else if (timedOut)
        note(state.SoloManaGate, "mana", true);
    bool const buffNeeded = _options.PullBuffGate && !turn.hurtRest && UpkeepBuffDue(bot, state, nowMs);
    if (pull_ready::Hold(state.SoloBuffGate, buffNeeded, nowMs, _options.PullWaitMaxMs, timedOut))
    {
        turn.pullWait = true;
        turn.hurtRest = true;
        turn.lowHealth = true;
        note(state.SoloBuffGate, "buff", false);
    }
    else if (timedOut)
        note(state.SoloBuffGate, "buff", true);
}

// The avoid function of a solo bot's own pulls: the turn's AvoidedFn, plus the creature entries that killed the bot (KillerAvoidMs) while it is out of combat. A bot in a group or on a
// route keeps the plain one (a boss that killed it is fought again). A creature already on the bot is not touched: the core's threat target is not asked through this.
std::function<bool(std::uint64_t)> BotPolicy::PullAvoidFn(BotTurn& turn)
{
    std::function<bool(std::uint64_t)> base = turn.AvoidedFn();
    BotState& state = turn.state;
    if (state.KillerAvoidUntilMs.empty() || turn.partyLeader != 0 || turn.routeOn || turn.bot.in_combat != 0)
        return base;
    std::uint64_t const nowMs = turn.nowMs;
    return [base, &avoid = state.KillerAvoidUntilMs, nowMs](std::uint64_t ref)
    {
        if (base(ref))
            return true;
        auto const it = avoid.find(forever_module::UnitRefEntry(ref));
        return it != avoid.end() && nowMs < it->second;
    };
}

// AoeCcProtect: whether a creature one of the bot's party holds (its Dots entry of Polymorph, Sap or Banish still running; the cc tactic records them) is in view within
// kAoeCcProtectYards of the bot's target (of the bot, with no target). The party is the bot's own (its leader's members and itself).
bool BotPolicy::CcHeldNearTarget(DecideTick& tick, BotTurn& turn)
{
    if (turn.partyLeader == 0)
        return false;
    std::uint64_t const nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    float const cx = turn.targetUnit ? turn.targetUnit->pos_x : bot.pos_x;
    float const cy = turn.targetUnit ? turn.targetUnit->pos_y : bot.pos_y;
    bool held = false;
    auto const check = [&](BotSnapshot const& member)
    {
        if (held)
            return;
        auto const known = _bots.find(member.guid);
        if (known == _bots.end())
            return;
        for (std::uint32_t const spell : { kSapSpell, kPolymorphSpell, kBanishSpell })
        {
            auto const dot = known->second.Dots.find(spell);
            if (dot == known->second.Dots.end() || nowMs >= dot->second.UntilMs || dot->second.Target == 0)
                continue;
            for (NearbyUnit const& unit : turn.nearby)
                if (unit.guid == dot->second.Target && IsLiveMob(unit) && std::hypot(unit.pos_x - cx, unit.pos_y - cy) <= kAoeCcProtectYards)
                    held = true;
        }
    };
    check(bot);
    tick.ForParty(turn.partyLeader, check);
    return held;
}
