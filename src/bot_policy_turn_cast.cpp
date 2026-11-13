// SPDX-License-Identifier: GPL-2.0-or-later
// BotPolicy::Decide, a bot's turn, its casts: heal orders, party healing and resurrects, the hunter pet, upkeep buffs, crafting and the rotation (the steps DecideBot runs in order; bot_policy.cpp).
#include "bot_policy_internal.h"
#include "party_view_adapter.h"

BotPolicy::TurnFlow BotPolicy::TurnForcedHeal(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& classId = turn.classId;

    // A heal order: the healer casts its heal on the speaker's selected friend, once.
    if (state.ForcedHeal != 0 && nowMs >= state.NextCastMs && !(IsCasting(bot)))
    {
        std::uint32_t const orderedSpell = HealSpellFor(classId, bot.level);
        if (orderedSpell != 0 && bot.moving != 0 && (KnownCastTimeSpell(orderedSpell) || state.CastTimeSpells.count(orderedSpell)))
        {
            BotAction stop;
            stop.bot_guid = bot.guid;
            stop.type = ActionType::kStop;
            actions.push_back(stop);
            state.Last = Decision{ ActionType::kStop, 0 };
            state.Approaching = false;
            return TurnFlow::kSkip;
        }
        if (orderedSpell != 0)
        {
            BotAction heal;
            heal.bot_guid = bot.guid;
            heal.type = ActionType::kCastSpell;
            heal.spell_id = orderedSpell;
            heal.target_guid = state.ForcedHeal;
            actions.push_back(heal);
            state.CastTarget[orderedSpell] = heal.target_guid;
            state.NextCastMs = nowMs + kGlobalCooldownMs;
        }
        state.ForcedHeal = 0;
        return TurnFlow::kSkip;
    }
    return TurnFlow::kNext;
}

BotPolicy::TurnFlow BotPolicy::TurnPartySupport(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& partyLeader = turn.partyLeader;

    // Healing: a healer class in a party heals the party member (itself included) with the least health, below kHealBelowPct, in range.
    // Dungeon priorities: emergency Flash Heal (< 30%) > shield preemptive (< 50% and took damage) > normal Heal (< 75%) > tank at 90%.
    // Allow healing in a dungeon run even before the party is fully formed (the first tick sends invites).
    if ((partyLeader != 0 || IsRouteActive(bot.guid)) && nowMs >= state.NextCastMs && !(IsCasting(bot)))
    {
        if (PartyHeal(tick, turn) == TurnFlow::kSkip)
            return TurnFlow::kSkip;
        if (PartyHealerDps(tick, turn) == TurnFlow::kSkip)
            return TurnFlow::kSkip;
        if (PartyResurrect(tick, turn) == TurnFlow::kSkip)
            return TurnFlow::kSkip;
    }
    return TurnFlow::kNext;
}

// TurnPartySupport: the heal of the party member that needs it most (an emergency, the tank, a shield, Renew, Prayer of Healing, a top-up).
BotPolicy::TurnFlow BotPolicy::PartyHeal(DecideTick& tick, BotTurn& turn)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    auto& leaderOf = tick.leaderOf;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& classId = turn.classId;
    auto const& partyLeader = turn.partyLeader;
    // A bot explicitly playing the tank never casts its class's heal spell from here: it has aggro and a target to hold, not
    // a party to triage (a paladin or druid given the tank role used to run PartyHeal like a healer; 2026-11-10 report, "a
    // paladin or druid tank does not auto-attack; it behaves like a healer").
    std::uint32_t const healSpell = (state.Role_ == BotState::Role::Dps || state.Role_ == BotState::Role::Tank) ? 0u : HealSpellFor(classId, bot.level);
    auto const retry = healSpell != 0 ? state.RetryAtMs.find(healSpell) : state.RetryAtMs.end();
    // Payment is checked per spell below, so a cheaper heal is not held back by the one this healer cannot afford yet.
    if (healSpell != 0 && (retry == state.RetryAtMs.end() || nowMs >= retry->second))
    {
        std::uint64_t const healGroup = partyLeader != 0 ? partyLeader : bot.guid;
        if (_healClaims.size() > 64)
            for (auto it = _healClaims.begin(); it != _healClaims.end();)
                it = nowMs >= it->second.UntilMs ? _healClaims.erase(it) : std::next(it);
        BotSnapshot const* hurt = nullptr;
        std::uint64_t hurtPct = (IsRouteActive(bot.guid) && _options.TacticHeal) ? _options.DungeonHealBelowPct : kHealBelowPct;
        BotSnapshot const* leaderHurt = nullptr;   // the tank with a target: healed first, whatever its share
        heal_plan::Plan const* const healPlan = HealPlanOf(partyLeader);
        std::uint64_t leaderHurtPct = HealPlanTankThreshold(healPlan, bot.guid, partyLeader, _options.DungeonTankHealBelowPct, nowMs);   // pre-heal window: topped up higher
        BotSnapshot const* leaderTopUp = nullptr;  // the route leader out of a fight, below kRouteTopUpPct
        // Emergency target: anyone under kEmergencyHealBelowPct gets Flash Heal first.
        BotSnapshot const* emergencyHurt = nullptr;
        // Shield target: a member under kShieldBelowPct that took damage recently (health dropped since last tick).
        BotSnapshot const* shieldHurt = nullptr;
        BotSnapshot const* tankMember = nullptr;   // the party leader, who tanks: Renew keeps a heal over time on it, Binding Heal pairs it with the priest
        std::uint64_t tankPct = 100;
        std::uint32_t lowCount = 0;                // members under kPrayerBelowPct (Prayer of Healing)
        auto const ready = [&](std::uint32_t spell)
        {
            auto const at = state.RetryAtMs.find(spell);
            return at == state.RetryAtMs.end() || nowMs >= at->second;
        };
        // Planned healing: do this healer's own members need a heal now (then it leaves the others alone, emergencies aside)?
        bool assignedNeed = false;
        if (healPlan && heal_plan::Covers(healPlan, bot.guid))
        {
            auto const needs = [&](BotSnapshot const& member)
            {
                if (member.life_state != 0 || member.max_health == 0 || !heal_plan::IsAssigned(healPlan, bot.guid, member.guid) || Distance2d(bot, member) > kHealRangeYards)
                    return;
                std::uint64_t const pct = std::uint64_t(member.health) * 100 / member.max_health;
                if (pct < (member.guid == partyLeader ? std::max<std::uint64_t>(leaderHurtPct, hurtPct) : hurtPct))
                    assignedNeed = true;
            };
            needs(bot);
            auto const lead = leaderOf.find(bot.guid);
            std::uint64_t const leadGuid = lead != leaderOf.end() ? lead->second : bot.guid;
            for (BotSnapshot const& other : view.bots)
            {
                auto const otherLead = leaderOf.find(other.guid);
                if (other.guid != bot.guid && otherLead != leaderOf.end() && otherLead->second == leadGuid)
                    needs(other);
            }
        }
        auto const consider = [&](BotSnapshot const& member)
        {
            if (member.life_state != 0 || member.max_health == 0 || Distance2d(bot, member) > kHealRangeYards)
                return;
            // Another healer of this party already has this member: leave it, someone else needs the heal more.
            if (auto const claim = _healClaims.find(member.guid); claim != _healClaims.end() &&
                nowMs < claim->second.UntilMs && claim->second.Group == healGroup)
                return;
            std::uint64_t const pct = std::uint64_t(member.health) * 100 / member.max_health;
            if (pct < kPrayerBelowPct)
                ++lowCount;   // counted before the assignment check: Prayer of Healing looks at the whole party, whoever else heals a member
            // The party planner gave this member to another healer (party_planner.h): leave it, the same as a claim.
            // With a standing heal plan (heal_plan.h) the plan decides whose member this is and the reactive planner's nearest-healer pick is not used.
            if (!healPlan && party_planner::HealAssignedElsewhere(tick.PlanOf(partyLeader), bot.guid, member.guid))
                return;
            // Another healer's member waits while this healer's own need heals, or while its healer is free to take it.
            if (healPlan && heal_plan::Judge(healPlan, bot.guid, member.guid, assignedNeed, OwnerCanHeal(tick, *healPlan, member), static_cast<std::uint32_t>(pct), kEmergencyHealBelowPct) == heal_plan::Verdict::Skip)
                return;
            if (member.guid == partyLeader)
            {
                tankMember = &member;
                tankPct = pct;
            }
            // Out of a fight the route leader is healed up: it walks on only at kRouteMinHealthPct, and with creatures in view it cannot sit to get there.
            // Ragefire Chasm, 2026-10-26: leaders stood between 50 and 79% for half an hour, below the 80% to walk and above the healer's 40%.
            if (member.guid == partyLeader && member.threat_target_guid == 0 && member.in_combat == 0 && bot.in_combat == 0 && IsRouteActive(bot.guid) &&
                pct < kRouteTopUpPct && pct >= kEmergencyHealBelowPct)
            {
                leaderTopUp = &member;
                return;
            }
            if (member.guid == partyLeader && member.threat_target_guid != 0)
            {
                if (pct < leaderHurtPct)
                {
                    leaderHurtPct = pct;
                    leaderHurt = &member;
                }
                return;
            }
            if (pct < kEmergencyHealBelowPct)
            {
                // Emergency: Flash Heal is instant-ish (1.5 s) and we can afford it.
                if (!emergencyHurt || pct < std::uint64_t(emergencyHurt->health) * 100 / emergencyHurt->max_health)
                    emergencyHurt = &member;
            }
            else
            {
                // Power Word: Shield is a priest spell; the other healers have no shield to cast.
                // Not on a warrior or a druid: the absorb starves a rage user of rage (the guide).
                // Not while Weakened Soul keeps a new shield off it (kShieldLockMs after the last one was ordered): the cast is refused, and with the shield first in the
                // priority chain the member below 55 % was never healed (Stockade, 2026-10-28: a mage held at 44 % for over 30 s while the priest ordered 15 shields).
                auto const shielded = state.BuffUntilMs.find({ kPowerWordShield, member.guid == bot.guid ? 0 : PlayerRef(member.guid) });
                bool const shieldLocked = shielded != state.BuffUntilMs.end() && nowMs < shielded->second;
                if (pct < kShieldBelowPct && !shieldLocked && classId == 5 && ClassFor(_options, member.guid) != kWarriorClass_ && ClassFor(_options, member.guid) != kDruidClass_)
                {
                    // Shield preemptive: check if this member's health dropped since the last tick (took damage).
                    // The module keeps a short history via state.StallBotHealth etc; here we approximate by checking
                    // if the member is in combat (taking hits) or was the recent target of a hostile.
                    bool tookDamage = member.in_combat != 0 || (state.FoughtAtMs.size() && state.FoughtAtMs.count(member.threat_target_guid));
                    if (tookDamage && (!shieldHurt || pct < std::uint64_t(shieldHurt->health) * 100 / shieldHurt->max_health))
                        shieldHurt = &member;
                }
                // A member between the emergency and the shield threshold is still a normal heal target when no shield
                // applies (it has not been hit yet): the old else-if chain left it with no heal at all, which the
                // 2026-10-23 run showed as a priest at 34 % never receiving a direct heal.
                if (pct < hurtPct)
                {
                    hurtPct = pct;
                    hurt = &member;
                }
            }
        };
        consider(bot);
        // A bot's party leader, from the tick's shared map; a route-active leader with no module party of its own (a
        // "tank lead" or level-dungeon run, which only needs IsRouteActive to reach this function, not partyLeader != 0) has no
        // entry there, so it is its own group, the same fallback healGroup above uses. Previously dereferenced leaderOf.find's
        // result unchecked, which was undefined behaviour for exactly that bot (2026-11-10 report, item 17); and separately read
        // every other bot's entry with leaderOf[other.guid], which silently inserted a zero entry for every bot guid this ever
        // ran across that was not already in the map, since leaderOf is a live reference into the tick's shared state, not a copy.
        auto const lead = leaderOf.find(bot.guid);
        std::uint64_t const leadGuid = lead != leaderOf.end() ? lead->second : bot.guid;
        for (BotSnapshot const& other : view.bots)
        {
            auto const otherLead = leaderOf.find(other.guid);
            if (other.guid != bot.guid && otherLead != leaderOf.end() && otherLead->second == leadGuid)
                consider(other);
        }
        // Priority: emergency > tank holding aggro > shield > normal
        std::uint32_t spellToCast = 0;
        BotSnapshot const* target = nullptr;
        std::string note;
        bool const priest = classId == 5;
        bool const prayer = priest && lowCount >= kPrayerMinLow && bot.level >= kPrayerOfHealingLevel && ready(kPrayerOfHealing);
        // Renew on the tank: while it fights and the last Renew has run out. The priest sees only its own auras, so the timer is the module's.
        bool const renewDue = priest && tankMember && tankMember->in_combat != 0 && bot.level >= kRenewLevel && ready(kRenew) &&
                              !(state.BuffUntilMs.count({ kRenew, PlayerRef(tankMember->guid) }) && nowMs < state.BuffUntilMs[{ kRenew, PlayerRef(tankMember->guid) }]);
        if (prayer)
        {
            spellToCast = kPrayerOfHealing; // a party heal around the priest: most of the party is low
            target = &bot;
            hurtPct = std::uint64_t(bot.health) * 100 / std::max<std::uint32_t>(bot.max_health, 1);
            note = "party";
        }
        else if (emergencyHurt)
        {
            // Flash Heal is a priest spell; the other healers answer an emergency with their own heal.
            spellToCast = classId == 5 ? kFlashHeal : healSpell;
            target = emergencyHurt;
            note = "emergency";
        }
        else if (leaderHurt)
        {
            spellToCast = healSpell;
            target = leaderHurt;
            hurtPct = leaderHurtPct;
            note = "tank";
        }
        else if (shieldHurt)
        {
            spellToCast = kPowerWordShield;
            target = shieldHurt;
            hurtPct = std::uint64_t(shieldHurt->health) * 100 / shieldHurt->max_health;
            note = "shield";
        }
        else if (renewDue)
        {
            spellToCast = kRenew;
            target = tankMember;
            hurtPct = tankPct;
            note = "renew";
        }
        else if (hurt)
        {
            spellToCast = healSpell;
            target = hurt;
            note = "";
        }
        else if (leaderTopUp)
        {
            spellToCast = healSpell;
            target = leaderTopUp;
            hurtPct = std::uint64_t(leaderTopUp->health) * 100 / leaderTopUp->max_health;
            note = "top_up";
        }
        // Binding Heal heals the priest and the tank together: when both are hurt and the chosen heal is for the tank.
        if (priest && target && target != &bot && target == tankMember && spellToCast != kPrayerOfHealing && spellToCast != kRenew && bot.level >= kBindingHealLevel &&
            ready(kBindingHeal) && std::uint64_t(bot.health) * 100 / std::max<std::uint32_t>(bot.max_health, 1) < kBindingBelowPct && tankPct < kBindingBelowPct)
        {
            spellToCast = kBindingHeal;
            note = note.empty() ? "binding" : note + " binding";
        }
        if (target)
        {
            // Check we can pay for the chosen spell, so a cheaper heal is used when the healer is low on mana.
            auto const spellRetry = state.RetryAtMs.find(spellToCast);
            if ((spellRetry == state.RetryAtMs.end() || nowMs >= spellRetry->second) && CanPay(_options.SpellCosts.get(), spellToCast, bot))
            {
                // If we recently lost LoS to the target and are still moving to regain it, wait for movement instead of retrying the cast.
                if (state.CloserUntilMs > nowMs)
                {
                    state.NextCastMs = nowMs + 1000;
                    return TurnFlow::kSkip;
                }
                if (bot.moving != 0 && (KnownCastTimeSpell(spellToCast) || state.CastTimeSpells.count(spellToCast)))
                {
                    BotAction stop;
                    stop.bot_guid = bot.guid;
                    stop.type = ActionType::kStop;
                    actions.push_back(stop);
                    state.Last = Decision{ ActionType::kStop, 0 };
                    state.Approaching = false;
                }
                else
                {
                    BotAction heal;
                    heal.bot_guid = bot.guid;
                    heal.type = ActionType::kCastSpell;
                    heal.spell_id = spellToCast;
                    heal.target_guid = target->guid == bot.guid ? 0 : PlayerRef(target->guid);
                    // Protocol 28 (HealDownrank): the direct heal is cast at the rank that covers the target's deficit, named exactly (dest_x = 1); the core answers kNotKnown for a
                    // rank the bot has not learned, and the downrank then pauses (ReadResult).
                    std::uint32_t downrank = 0;
                    if (_options.HealDownrank && spellToCast == healSpell && nowMs >= state.HealDownrankPausedUntilMs && target->max_health > target->health)
                    {
                        std::uint32_t const mana = bot.power_type == 0 ? bot.power : std::numeric_limits<std::uint32_t>::max();
                        downrank = heal_ranks::PickRank(classId, bot.level, target->max_health - target->health, mana);
                        if (downrank != 0)
                        {
                            heal.spell_id = downrank;
                            heal.dest_x = 1.0f;
                        }
                    }
                    actions.push_back(heal);
                    state.CastTarget[heal.spell_id] = heal.target_guid;
                    state.CastTarget[spellToCast] = heal.target_guid;
                    if (spellToCast == kRenew)
                        state.BuffUntilMs[{ kRenew, PlayerRef(target->guid) }] = nowMs + kRenewMs;
                    if (spellToCast == kPowerWordShield)
                        state.BuffUntilMs[{ kPowerWordShield, heal.target_guid }] = nowMs + kShieldLockMs;
                    _healClaims[target->guid] = HealClaim{ healGroup, nowMs + kHealClaimMs };
                    if (healPlan && heal_plan::Covers(healPlan, bot.guid) && !heal_plan::IsAssigned(healPlan, bot.guid, target->guid) && !assignedNeed && hurtPct >= kEmergencyHealBelowPct &&
                        healPlan->HealerOf.count(target->guid) && _options.OnEvent)
                        _options.OnEvent("heal_assign_switch: healer=" + std::to_string(bot.guid) + " target=" + std::to_string(target->guid) +
                                         " assigned_healer=" + std::to_string(healPlan->HealerOf.at(target->guid)) + " target_hp_pct=" + std::to_string(hurtPct));
                    if (_options.OnEvent)
                        _options.OnEvent("heal: healer=" + std::to_string(bot.guid) + " spell=" + std::to_string(spellToCast) +
                                         " target=" + std::to_string(target->guid) + " target_hp_pct=" + std::to_string(hurtPct) +
                                         (downrank != 0 ? " rank=" + std::to_string(downrank) : std::string()) + (note.empty() ? "" : " " + note));
                    state.NextCastMs = nowMs + kGlobalCooldownMs;
                }
                return TurnFlow::kSkip;
            }
        }
    }
    return TurnFlow::kNext;
}

BotPolicy::TurnFlow BotPolicy::PartyHealerDps(DecideTick& tick, BotTurn& turn)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    auto const& indexOf = tick.indexOf;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& nearby = turn.nearby;
    auto const& classId = turn.classId;
    auto const& partyLeader = turn.partyLeader;
    // Healer DPS window: when nobody needs healing and healer has mana, cast offensive spells.
    // Only for priest (class 5): Smite, Mind Blast, Shadow Word: Pain. Only in a dungeon party,
    // only out of combat or when the party is not taking damage, and only when healer mana > 40%.
    if (partyLeader != 0 && classId == 5 && bot.in_combat == 0 && PartyFighting(tick, partyLeader) &&
        (state.LosRepositionTarget != 0 ||
         std::uint64_t(bot.power) * 100 >= std::uint64_t(bot.max_power) * _options.HealerDpsManaPct))
    {
        if (state.LosRepositionTarget != 0 && _options.OnEvent)
            _options.OnEvent("healer_dps_los_pending: healer=" + std::to_string(bot.guid) +
                             " target=" + std::to_string(state.LosRepositionTarget));
        // Check if anyone in party needs healing
        bool anyoneHurt = false;
        tick.ForParty(partyLeader, [&](BotSnapshot const& member)
        {
            if (member.life_state != 0 || member.max_health == 0)
                return;
            std::uint64_t const pct = std::uint64_t(member.health) * 100 / member.max_health;
            if (pct < (member.guid == partyLeader ? 80 : 60))
                anyoneHurt = true;
        });
        if (!anyoneHurt && nowMs >= state.NextCastMs && !(IsCasting(bot)))
        {
            // Try Smite first (30 yd range, no cooldown), then SWP (DoT), then Mind Blast (high damage, cooldown, needs LoS).
            // Smite and SWP are safer when LoS is uncertain.
            static constexpr std::uint32_t kSmite = 585;
            static constexpr std::uint32_t kSWP = 589;
            static constexpr std::uint32_t kMindBlast = 8092;
            std::uint32_t dpsSpell = 0;
            // Prefer Smite (always works at 30 yd), then SWP, then Mind Blast (needs clear LoS).
            // If a previous priest DPS cast lost LoS, reposition to its target before retrying any spell.
            bool const losBlocked = state.CloserUntilMs > nowMs || state.LosRepositionTarget != 0;
            if (CanPay(_options.SpellCosts.get(), kSmite, bot) &&
                (state.RetryAtMs.find(kSmite) == state.RetryAtMs.end() || nowMs >= state.RetryAtMs[kSmite]))
                dpsSpell = kSmite;
            else if (CanPay(_options.SpellCosts.get(), kSWP, bot) &&
                     (state.RetryAtMs.find(kSWP) == state.RetryAtMs.end() || nowMs >= state.RetryAtMs[kSWP]))
                dpsSpell = kSWP;
            else if (!losBlocked && CanPay(_options.SpellCosts.get(), kMindBlast, bot) &&
                     (state.RetryAtMs.find(kMindBlast) == state.RetryAtMs.end() || nowMs >= state.RetryAtMs[kMindBlast]))
                dpsSpell = kMindBlast;
            if (dpsSpell != 0)
            {
                // Healer DPS is only on the tank's engaged target; never self-pull a nearby pack.
                std::uint64_t targetGuid = 0;
                auto const lead = indexOf.find(partyLeader);
                if (lead != indexOf.end() && view.bots[lead->second].threat_target_guid != 0)
                {
                    for (NearbyUnit const& unit : nearby)
                        if (unit.guid == view.bots[lead->second].threat_target_guid && unit.is_hostile && unit.health != 0)
                        {
                            targetGuid = unit.guid;
                            break;
                        }
                }
                if (targetGuid != 0)
                {
                    BotAction dps;
                    dps.bot_guid = bot.guid;
                    dps.type = ActionType::kCastSpell;
                    dps.spell_id = dpsSpell;
                    dps.target_guid = targetGuid;
                    actions.push_back(dps);
                    state.CastTarget[dpsSpell] = targetGuid;
                    state.NextCastMs = nowMs + kGlobalCooldownMs;
                    if (_options.OnEvent)
                        _options.OnEvent("healer_dps: healer=" + std::to_string(bot.guid) + " spell=" + std::to_string(dpsSpell) +
                                         " target=" + std::to_string(forever_module::UnitRefEntry(targetGuid)));
                    return TurnFlow::kSkip;
                }
            }
        }
    }
    return TurnFlow::kNext;
}

BotPolicy::TurnFlow BotPolicy::PartyResurrect(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& classId = turn.classId;
    auto const& partyLeader = turn.partyLeader;
    auto& hasTarget = turn.hasTarget;
    // Resurrect (after the heals): a priest, paladin or shaman out of combat with the party quiet resurrects the dead member in range
    // that it is closest to (healers first, then the tank), once the 10 s cast is affordable. A refusal retries after a few seconds.
    std::uint32_t const rezSpell = ResurrectSpellFor(classId, bot.level);
    bool const manaOk = bot.power_type != 0 || bot.max_power == 0 ||
                        std::uint64_t(bot.power) * 100 >= std::uint64_t(bot.max_power) * kRezManaPct;
    if (rezSpell != 0 && bot.in_combat == 0 && !hasTarget && manaOk && !PartyFighting(tick, partyLeader))
    {
        BotSnapshot const* pick = nullptr;
        int pickRank = 99;
        float pickDist = 0.0f;
        tick.ForParty(partyLeader, [&](BotSnapshot const& dead)
        {
            if (dead.life_state == 0 || dead.map_id != bot.map_id || dead.resurrect_from != 0)
                return;
            float const dist = Distance2d(bot, dead);
            if (dist > kRezRangeYards)
                return;
            auto const retry = state.RezRetryAtMs.find(PlayerRef(dead.guid));
            if (retry != state.RezRetryAtMs.end() && nowMs < retry->second)
                return;
            // Only the closest eligible rezzer casts on a target (a tie: the lowest guid).
            bool otherCloser = false;
            tick.ForParty(partyLeader, [&](BotSnapshot const& other)
            {
                if (other.guid == bot.guid || !IsRezzer(other) || other.in_combat != 0 || other.map_id != bot.map_id)
                    return;
                float const otherDist = Distance2d(other, dead);
                if (otherDist <= kRezRangeYards && (otherDist < dist || (otherDist == dist && other.guid < bot.guid)))
                    otherCloser = true;
            });
            if (otherCloser)
                return;
            std::uint32_t const deadClass = ClassFor(_options, dead.guid);
            int const rank = HealSpellFor(deadClass, 99) != 0 ? 0 : deadClass == kWarriorClass ? 1 : 2;
            if (!pick || rank < pickRank || (rank == pickRank && dist < pickDist))
            {
                pick = &dead;
                pickRank = rank;
                pickDist = dist;
            }
        });
        if (pick)
        {
            if (bot.moving != 0)
            {
                BotAction stop;
                stop.bot_guid = bot.guid;
                stop.type = ActionType::kStop;
                actions.push_back(stop);
                state.Last = Decision{ ActionType::kStop, 0 };
                state.Approaching = false;
            }
            else
            {
                BotAction rez;
                rez.bot_guid = bot.guid;
                rez.type = ActionType::kCastSpell;
                rez.spell_id = rezSpell;
                rez.target_guid = PlayerRef(pick->guid);
                actions.push_back(rez);
                state.CastTarget[rezSpell] = rez.target_guid;
                state.NextCastMs = nowMs + kGlobalCooldownMs;
                if (_options.OnEvent)
                    _options.OnEvent("rez_cast: rezzer=" + std::to_string(bot.guid) + " spell=" + std::to_string(rezSpell) + " target=" +
                                     std::to_string(pick->guid) + " dist=" + std::to_string(int(pickDist)) + " life=" + std::to_string(pick->life_state));
            }
            return TurnFlow::kSkip;
        }
    }
    return TurnFlow::kNext;
}

BotPolicy::TurnFlow BotPolicy::TurnHunterPet(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& classId = turn.classId;
    auto& hasTarget = turn.hasTarget;
    auto& wanted = turn.wanted;

    // Hunter pet (out of combat, standing, nothing to fight). Layout 13 puts the pet in the snapshot (pet_state 0 none,
    // 1 alive, 2 dead, with pet_health and pet_max_health), so its state is fact rather than what refused casts imply;
    // the inference below is the fallback for a core that does not report pets.
    if (classId == kHunterClass && !hasTarget && bot.in_combat == 0 && bot.moving == 0 && wanted.Type == ActionType::kStop && nowMs >= state.NextCastMs &&
        !(bot.casting_spell != 0 && bot.cast_remaining_ms != 0) && !state.Sitting && nowMs >= state.PetRetryAtMs)
    {
        if (state.PetFought)
        {
            state.PetFought = false;
            state.PetProbe = true;
        }
        PetKnown const known = PetOf(bot);
        bool const mendable = bot.level == 0 || bot.level >= rotation::LearnLevel(kMendPet);
        bool const petHurt = bot.pet_max_health != 0 &&
                             std::uint64_t(bot.pet_health) * 100 < std::uint64_t(bot.pet_max_health) * kPetMendHealthPct;
        std::uint32_t petSpell = 0;
        if (known == PetKnown::kDead)
            petSpell = kRevivePet;                                    // fact: it is dead
        else if (known == PetKnown::kNone && state.PetMissing)
            petSpell = state.PetDead ? kRevivePet : kCallPet;          // inferred: no pet in the snapshot
        else if (known == PetKnown::kOut && petHurt && mendable)
            petSpell = kMendPet;                                      // fact: it is out and hurt
        else if (state.PetProbe)
        {
            if (mendable)
                petSpell = kMendPet;
            else if (nowMs >= state.PetProbeAtMs)
                petSpell = kCallPet;
            else
                state.PetProbe = false;
        }
        if (petSpell != 0)
        {
            auto const retry = state.RetryAtMs.find(petSpell);
            bool const learned = bot.level == 0 || bot.level >= rotation::LearnLevel(petSpell);
            if (learned && (retry == state.RetryAtMs.end() || nowMs >= retry->second))
            {
                BotAction pet;
                pet.bot_guid = bot.guid;
                pet.type = ActionType::kCastSpell;
                pet.spell_id = petSpell;
                pet.target_guid = 0; // the pet is the spell's own target
                actions.push_back(pet);
                state.CastTarget[petSpell] = 0;
                state.NextCastMs = nowMs + kGlobalCooldownMs;
                if (petSpell == kCallPet && !state.PetMissing)
                {
                    state.PetProbeAtMs = nowMs + kPetProbeMs;
                    state.PetProbe = false;
                }
                return TurnFlow::kSkip;
            }
        }
    }
    return TurnFlow::kNext;
}

void BotPolicy::TurnUpkeepBuffs(DecideTick& tick, BotTurn& turn)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& nearby = turn.nearby;
    auto const& classId = turn.classId;
    auto const& spec = turn.spec;
    auto& hasTarget = turn.hasTarget;

    // Upkeep buffs (rotation file rows 'buff'): out of combat, standing, with nothing to fight. Each is cast on the bot, or on every member of its group
    // (party), when its last cast has run out; a refused cast is tried again after the usual retry.
    if (!hasTarget && bot.in_combat == 0 && bot.moving == 0 && nowMs >= state.NextCastMs && !(IsCasting(bot)) &&
        !state.Sitting)
    {
        bool cast = false;
        // Claims that have run out are dropped; the map is bounded so a long run cannot grow it.
        if (_buffClaims.size() > 256)
            for (auto it = _buffClaims.begin(); it != _buffClaims.end();)
                it = nowMs >= it->second.UntilMs ? _buffClaims.erase(it) : std::next(it);
        std::uint64_t const buffGroup = bot.group_leader_guid != 0 ? bot.group_leader_guid : bot.guid;
        rotation::Facts const buffFacts = rotation::FactsFor(bot, nearby, nullptr, 0);
        for (rotation::Row const& row : RowsFor(classId, spec))
        {
            if (!row.Buff || !RowInLevelRange(row, bot.level))
                continue;
            // A buff row may carry a condition: a stance only in a party (in_party), a pet summon only with no pet (no_pet).
            if (!rotation::Allowed(row, buffFacts))
                continue;
            auto const retry = state.RetryAtMs.find(row.Spell);
            if (retry != state.RetryAtMs.end() && nowMs < retry->second)
                continue;
            if (!CanPay(_options.SpellCosts.get(), row.Spell, bot))
                continue; // a warrior has no rage out of combat: Battle Shout waits until it has some, instead of a cast the core refuses
            // A pet summon (no_pet) is due whenever the snapshot shows no pet: the party claim and the buff timer below would hold it off for the
            // spell's duration (an hour), long after the demon died. The cast in progress and the global cooldown stop it being sent twice.
            if (rotation::RowHasCondition(row, rotation::Cond::NoPet))
            {
                BotAction summon;
                summon.bot_guid = bot.guid;
                summon.type = ActionType::kCastSpell;
                summon.spell_id = row.Spell;
                summon.target_guid = 0;
                actions.push_back(summon);
                state.CastTarget[row.Spell] = 0;
                state.NextCastMs = nowMs + kGlobalCooldownMs;
                cast = true;
                break;
            }
            // The recipients: the bot itself (reference 0), and for a party buff the members of its group that are alive and close.
            std::vector<std::pair<std::uint64_t, std::uint64_t>> recipients{ { 0, bot.guid } };   // (the cast's target reference, the member's guid)
            if (row.Party)
                for (BotSnapshot const& other : view.bots)
                    if (other.guid != bot.guid && other.life_state == 0 && other.group_leader_guid != 0 && other.group_leader_guid == bot.group_leader_guid &&
                        Distance2d(bot, other) <= kBuffRangeYards)
                        recipients.push_back({ PlayerRef(other.guid), other.guid });
            for (auto const& [recipient, recipientGuid] : recipients)
            {
                // The party planner gave this member's buff to a closer caster: leave it (the claims below still guard a pair the plan does not cover).
                if (recipient != 0 && party_planner::BuffAssignedElsewhere(tick.PlanOf(turn.partyLeader), bot.guid, row.Spell, recipientGuid))
                    continue;
                // The bot's own buff is read from the snapshot's auras (layout 13): a buff another player cast over it,
                // or one it lost to a death, is now seen as the fact it is. A party member's buff is not in this bot's
                // snapshot, so its timer and the party claim carry it.
                if (recipient == 0)
                {
                    if (AuraRunning(bot, row.Spell, row.Ranks))
                        continue;
                }
                else
                {
                    auto const mine = state.BuffUntilMs.find({ row.Spell, recipient });
                    if (mine != state.BuffUntilMs.end() && nowMs < mine->second)
                        continue;
                }
                // A party member refreshed this buff moments ago: someone else has it, leave it to them.
                auto const claimed = _buffClaims.find({ row.Spell, recipient });
                if (claimed != _buffClaims.end() && nowMs < claimed->second.UntilMs && claimed->second.Group == buffGroup)
                    continue;
                BotAction buff;
                buff.bot_guid = bot.guid;
                buff.type = ActionType::kCastSpell;
                buff.spell_id = row.Spell;
                buff.target_guid = recipient;
                actions.push_back(buff);
                state.CastTarget[row.Spell] = recipient;
                state.NextCastMs = nowMs + kGlobalCooldownMs;
                std::uint64_t const untilMs = nowMs + std::uint64_t(row.DurationSec) * 1000;
                _buffClaims[{ row.Spell, recipient }] = BuffClaim{ buffGroup, untilMs };
                // A buff this bot casts on itself is claimed under its own player reference as well. Without that, a party mate
                // deciding a moment later sees { spell, PlayerRef(this bot) } unclaimed and buffs it a second time: the self claim is
                // keyed 0, the members' by reference, and the snapshot that would show the buff does not exist yet.
                if (recipient == 0)
                    _buffClaims[{ row.Spell, PlayerRef(bot.guid) }] = BuffClaim{ buffGroup, untilMs };
                cast = true;
                break;
            }
            if (cast)
                break;
        }

        // Passing-player buff (PassingBuff, off by default): out of combat, a low chance to buff a friendly player within 15 yards. Only when the bot has the mana
        // (a share of its pool, and always the mana of its own next casts of the fight rotation on top of the buff), after its per-bot cooldown, and within the
        // map's rate. It sends one cast and touches nothing of the fight.
        if (!cast && _options.PassingBuff.Enabled && !state.InFight && !turn.enemyNear && bot.life_state == 0 && bot.power_type == 0 && bot.max_power > 0 &&
            nowMs >= state.PassingBuffNextMs &&
            bot_random::Below(bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltIdle, 0x600000000ull + nowMs / 5000), 100) < _options.PassingBuff.ChancePct)
        {
            NearbyUnit const* passer = nullptr;
            float passerDistance = 1e9f;
            bool hostileNear = false;
            for (NearbyUnit const& unit : nearby)
            {
                if (unit.is_hostile && unit.health != 0)
                    hostileNear = true;
                else if (!unit.is_hostile && forever_module::UnitRefType(unit.guid) == kPlayerType && !tick.IsOurBot(unit.guid) && unit.health != 0)
                {
                    float const distance = std::hypot(unit.pos_x - bot.pos_x, unit.pos_y - bot.pos_y);
                    if (distance < passerDistance)
                    {
                        passer = &unit;
                        passerDistance = distance;
                    }
                }
            }
            if (passer && !hostileNear && _options.SpellCosts)
            {
                std::vector<std::uint32_t> fightCosts;
                for (rotation::Row const& row : RowsFor(classId, spec))
                    if (!row.Buff && (bot.level == 0 || row.MinLevel <= bot.level))
                        if (auto const payment = _options.SpellCosts->CostAtLevel(row.Spell, bot.level); payment && payment->PowerType == 0)
                            fightCosts.push_back(payment->Cost);
                for (rotation::Row const& row : RowsFor(classId, spec))
                {
                    if (!row.Buff || !row.Party || !RowInLevelRange(row, bot.level))
                        continue;
                    if (row.When.Condition != rotation::Cond::Always || row.When2.Condition != rotation::Cond::Always)
                        continue;
                    auto const retry = state.RetryAtMs.find(row.Spell);
                    if (retry != state.RetryAtMs.end() && nowMs < retry->second)
                        continue;
                    auto const payment = _options.SpellCosts->CostAtLevel(row.Spell, bot.level);
                    if (!payment || payment->PowerType != 0)
                        continue;
                    realism::BuffFacts facts;
                    facts.InCombat = bot.in_combat != 0;
                    facts.HasTarget = hasTarget;
                    facts.Alive = bot.life_state == 0;
                    facts.Moving = bot.moving != 0;
                    facts.Casting = IsCasting(bot);
                    facts.ManaUser = true;
                    facts.Power = bot.power;
                    facts.MaxPower = bot.max_power;
                    facts.BuffCost = payment->Cost;
                    facts.RotationReserve = realism::ReserveMana(fightCosts, _options.PassingBuff.ReserveCasts);
                    facts.TargetDistance = passerDistance;
                    if (!realism::BuffAllowed(_options.PassingBuff, facts, nowMs, state.PassingBuffNextMs))
                        continue;
                    if (!_passingBuffRate.TryGrant(bot.map_id, nowMs, _options.PassingBuff.ZonePerMin))
                        break;
                    BotAction buff;
                    buff.bot_guid = bot.guid;
                    buff.type = ActionType::kCastSpell;
                    buff.spell_id = row.Spell;
                    buff.target_guid = passer->guid;
                    actions.push_back(buff);
                    state.CastTarget[row.Spell] = passer->guid;
                    state.NextCastMs = nowMs + kGlobalCooldownMs;
                    state.PassingBuffNextMs = nowMs + std::uint64_t(_options.PassingBuff.CooldownSec) * 1000;
                    break;
                }
            }
        }
    }
}

void BotPolicy::TurnCraftGather(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& hasTarget = turn.hasTarget;

    // Crafting (SDK 0.17): a bot with nothing to fight, standing still, makes bandages from its cloth and cooks its meat. That raises First Aid and Cooking, and uses up
    // what fills its bags. SDK 0.18: and the things of its crafting profession (Blacksmithing, Leatherworking, Alchemy, Tailoring, Engineering, Smelting). The core picks the
    // recipe; when there is nothing to make it answers kRefused and the skill waits (ReadResult); when a recipe needs an anvil, forge or fire it answers kNeedsFocus and the
    // bot walks to one. The lines take turns.
    bool const calm = !hasTarget && bot.in_combat == 0 && bot.moving == 0 && bot.life_state == 0 && nowMs >= state.NextCastMs && !(IsCasting(bot)) && !state.Sitting &&
                      !state.CraftWalking;
    if (_options.Crafting && calm)
    {
        bool crafted = false;
        for (std::size_t turn = 0; turn < kCraftLineCount && !crafted; ++turn)
        {
            std::size_t const index = (state.CraftTurn + turn) % kCraftLineCount;
            if (nowMs < state.CraftRetryAtMs[index] || !HasLine(bot, kCraftLines[index]))
                continue;
            BotAction craft;
            craft.bot_guid = bot.guid;
            craft.type = ActionType::kSkillUp;
            craft.spell_id = kCraftLines[index];
            actions.push_back(craft);
            state.CraftTurn = std::uint32_t(index + 1);
            state.CraftSent = std::uint32_t(index);
            state.CraftRetryAtMs[index] = nowMs + kSkillUpCastMs; // the answer decides the next try
            state.NextCastMs = nowMs + kGlobalCooldownMs;
            crafted = true;
        }
    }

    // Gathering (SDK 0.18, off by default): a bot with Mining, Herbalism or Skinning asks the core to gather what is within 30 yards of it: a vein or herb its skill allows, or a
    // corpse it can skin. The core walks the bot there and casts; the next ask takes the loot.
    if (_options.Gathering && calm && !state.GatherWalking)
    {
        bool gathered = false;
        for (std::size_t index = 0; index < kGatherLineCount && !gathered; ++index)
        {
            if (nowMs < state.GatherRetryAtMs[index] || !HasLine(bot, kGatherLines[index]))
                continue;
            BotAction gather;
            gather.bot_guid = bot.guid;
            gather.type = ActionType::kGather;
            gather.spell_id = kGatherLines[index];
            actions.push_back(gather);
            state.GatherSent = std::uint32_t(index);
            state.GatherRetryAtMs[index] = nowMs + kSkillUpCastMs; // the answer decides the next try
            state.NextCastMs = nowMs + kGlobalCooldownMs;
            gathered = true;
        }
    }
}

// Whether a healer in a party fight is below its combat mana floor (HealerCombatManaPct): a healer class (a role that heals, a spell to heal with) in a group, in combat, with a
// mana pool. Out of combat the old window (HealerDpsManaPct) still applies; this is the in-combat counterpart.
bool BotPolicy::HealerBelowManaFloor(BotSnapshot const& bot, BotState const& state, std::uint32_t classId, std::uint64_t partyLeader) const
{
    if (_options.HealerCombatManaPct == 0 || partyLeader == 0 || bot.in_combat == 0 || bot.max_power == 0)
        return false;
    if (state.Role_ == BotState::Role::Dps || state.Role_ == BotState::Role::Tank || HealSpellFor(classId, bot.level) == 0)
        return false;
    return std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * _options.HealerCombatManaPct;
}

BotPolicy::TurnFlow BotPolicy::TurnRotation(DecideTick& tick, BotTurn& turn)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& nearby = turn.nearby;
    auto const& classId = turn.classId;
    auto const& spec = turn.spec;
    auto const& partyLeader = turn.partyLeader;
    auto& targetUnit = turn.targetUnit;
    auto& targetRef = turn.targetRef;
    auto const& distance = turn.distance;
    auto& wanted = turn.wanted;

    // Nothing to cast at: stop here. Not "!hasTarget" (which also folds in the below-30%-health disengage, bot_policy_turn_target.cpp's
    // lowHealth): that used to stop the whole rotation at low health, including a bot's own defensive and self-heal rows (Shield Block,
    // Evasion, Holy Light on self...) at exactly the point they matter most (2026-11-10 report, "a bot below 30% health stops all
    // ability use"). A solo bot that low still disengages (TurnMoveChoice's flee branch); this only lets it also use what it has while
    // it does.
    if (targetRef == 0)
    {
        state.AutoShotTarget = 0; // the fight is over: Auto Shot stopped with it
        return TurnFlow::kSkip;
    }

    if (_options.TestSpell != 0)
    {
        // Test cast: on the snapshot tick timer, after the attack decision above.
        if (wanted.Type == ActionType::kAttackTarget &&
            (!state.LastTestCastTick || view.tick_number >= *state.LastTestCastTick + _options.CastEveryTicks))
        {
            BotAction cast;
            cast.bot_guid = bot.guid;
            cast.type = ActionType::kCastSpell;
            cast.spell_id = _options.TestSpell;
            cast.target_guid = targetRef;
            actions.push_back(cast);
            state.CastTarget[_options.TestSpell] = targetRef;
            state.LastTestCastTick = view.tick_number;
        }
        return TurnFlow::kSkip;
    }

    float const castDistance = targetUnit ? rotation::DistanceYards3d(bot, *targetUnit) : distance;

    // Class rotation: the first row that is off cooldown, in range and allowed by its rule. One cast per global cooldown.
    if (nowMs < state.NextCastMs)
        return TurnFlow::kSkip;
    // A cast or channel in progress finishes first: no cast can start meanwhile.
    if (IsCasting(bot))
        return TurnFlow::kSkip;
    rotation::Facts facts = rotation::FactsFor(bot, nearby, targetUnit, state.InFight ? nowMs - state.FightStartMs : 0);
    if (state.ComboTarget != targetRef)
    {
        state.ComboTarget = targetRef;
        state.ComboPoints = 0;
    }
    facts.ComboPoints = int32_t(state.ComboPoints);
    state.PetFought = true;
    // Auto Shot runs until the target changes or the bot moves; casting it again would not help (and may toggle it off).
    if (state.AutoShotTarget != 0 && (state.AutoShotTarget != targetRef || bot.moving != 0))
        state.AutoShotTarget = 0;

    // Spells the core reports as cooling down right now.
    std::unordered_set<std::uint32_t> cooling;
    for (forever_module::SpellCooldown const& cooldown : bot.spell_cooldowns)
        if (cooldown.spell_id != 0 && cooldown.remaining_ms != 0)
            cooling.insert(cooldown.spell_id);

    // Hunter: Mend Pet now and then in a long fight, when the pet is believed to be out.
    if (classId == kHunterClass && !state.PetMissing && bot.moving == 0 && nowMs >= state.PetMendAtMs && facts.FightMs >= kPetMendAfterMs &&
        facts.SelfPowerPct >= std::int32_t(kPetMendManaPct) && facts.SelfHpPct >= 50 &&
        (bot.level == 0 || bot.level >= rotation::LearnLevel(kMendPet)) && !cooling.count(kMendPet))
    {
        auto const retry = state.RetryAtMs.find(kMendPet);
        if (retry == state.RetryAtMs.end() || nowMs >= retry->second)
        {
            BotAction mend;
            mend.bot_guid = bot.guid;
            mend.type = ActionType::kCastSpell;
            mend.spell_id = kMendPet;
            mend.target_guid = 0;
            actions.push_back(mend);
            state.CastTarget[kMendPet] = 0;
            state.NextCastMs = nowMs + kGlobalCooldownMs;
            state.PetMendAtMs = nowMs + kPetMendEveryMs;
            return TurnFlow::kSkip;
        }
    }

    bool const noAoe = party_planner::NoAoeNow(tick.PlanOf(partyLeader));
    // Healer mana floor (HealerCombatManaPct, bug 11): a healer in a party fight does not spend its last mana on attacks; the heals (PartyHeal) are not rows and are not held.
    bool const manaFloor = HealerBelowManaFloor(bot, state, classId, partyLeader);
    for (rotation::Row const& row : RowsFor(classId, spec))
    {
        if (row.Buff)
            continue; // upkeep spells are cast out of combat, below
        if (manaFloor && row.OnTarget)
            continue; // below the healer's combat mana floor: attacks wait, self and party spells stay
        if (row.Spell == kAutoShot && state.AutoShotTarget == targetRef)
            continue; // already shooting this target
        if (row.OnTarget && row.MaxRange > kMeleeApproachYards + 1.0f && NoAmmo(bot, classId))
            continue; // a ranged attack of a hunter with no ammunition: refused by the core, so only the melee rows are used
        if (row.MaxLevel != 0 && bot.level > row.MaxLevel)
            continue; // a better spell has replaced it
        if (noAoe && rotation::IsAoeRow(row))
            continue; // the raid rules' noaoe fight: no area damage now
        // Not learned yet: below the level a trainer teaches it (the rotation file's level, else the built-in table). (A level of 0 is unknown.)
        if (bot.level != 0 && (row.MinLevel != 0 ? row.MinLevel : rotation::LearnLevel(row.Spell)) > bot.level)
            continue;
        auto const retry = state.RetryAtMs.find(row.Spell);
        if (retry != state.RetryAtMs.end() && nowMs < retry->second)
            continue;
        if (cooling.count(row.Spell))
            continue;
        if (!CanPay(_options.SpellCosts.get(), row.Spell, bot))
            continue; // not enough mana, rage or energy for it now: the next row, not a refused cast
        if (row.OnTarget && castDistance < row.MinRange)
            continue;
        if (row.OnTarget && row.MaxRange > 0.0f && castDistance > (row.MaxRange > kRangeSlackYards * 2.0f ? row.MaxRange - kRangeSlackYards : row.MaxRange))
            continue;
        if (!rotation::Allowed(row, facts))
            continue;
        // A cast that ends before the kick lands is left alone (the field is 0 when the core does not report it: then no gate).
        if (rotation::RowHasCondition(row, rotation::Cond::TargetCasting) && targetUnit && targetUnit->cast_remaining_ms != 0 &&
            targetUnit->cast_remaining_ms < kInterruptMinRemainingMs)
            continue;
        // Only one member of the party stops a cast: when a party member already claimed this target's interrupt, this bot uses its next row.
        if (rotation::RowHasCondition(row, rotation::Cond::TargetCasting) &&
            (InterruptTaken(targetRef, nowMs) || !party_planner::MayInterrupt(_options.PartyPlanner, tick.PlanOf(partyLeader), bot.guid, targetRef, nowMs)))
            continue;
        // The effect is still up (on the target, or on the bot for a self buff): recasting only refreshes it.
        auto const dot = state.Dots.find(row.Spell);
        bool const sameTarget = dot != state.Dots.end() && dot->second.Target == (row.OnTarget ? targetRef : 0);
        if (sameTarget && nowMs < dot->second.UntilMs)
            continue;

        // A spell with a cast time cannot start while the bot moves: stop first (the cast follows on the next tick). The stop is
        // remembered as the bot's last movement order, so a later follow or walk is sent again.
        if (bot.moving != 0 && (KnownCastTimeSpell(row.Spell) || state.CastTimeSpells.count(row.Spell)))
        {
            BotAction stop;
            stop.bot_guid = bot.guid;
            stop.type = ActionType::kStop;
            actions.push_back(stop);
            state.Last = Decision{ ActionType::kStop, 0 };
            state.Approaching = false;
            break;
        }

        BotAction cast;
        cast.bot_guid = bot.guid;
        cast.type = ActionType::kCastSpell;
        cast.spell_id = row.Spell;
        cast.target_guid = row.OnTarget ? targetRef : 0;
        actions.push_back(cast);
        state.CastTarget[row.Spell] = cast.target_guid;
        state.NextCastMs = nowMs + kGlobalCooldownMs;
        // Claim this target's interrupt so another party member's rotation reaches past it for one still free (read by
        // InterruptTaken; see _interruptClaims). Bounded the same way as the heal and buff claim maps: expired entries are
        // dropped once there are enough of them to be worth the scan, so a long-running server does not grow this forever.
        if (rotation::RowHasCondition(row, rotation::Cond::TargetCasting) && cast.target_guid != 0)
        {
            if (_interruptClaims.size() > 256)
                for (auto it = _interruptClaims.begin(); it != _interruptClaims.end();)
                    it = nowMs >= it->second.UntilMs ? _interruptClaims.erase(it) : std::next(it);
            _interruptClaims[cast.target_guid] = InterruptClaim{ partyLeader, nowMs + kInterruptClaimMs };
        }
        break;
    }
    return TurnFlow::kNext;
}

// Tank pickup of loose mobs (plan item 2). The core's party view lists the creatures that are on a member who is not a tank (needs Bot.Snapshot.PartyThreat); this turn lets
// the tank take them: the single taunt (warrior Taunt, druid Growl) on the first loose mob in reach, the area taunt (Challenging Shout, Challenging Roar) when two or more are
// loose. A paladin has no taunt in this client's data, so it only follows its own target logic. Acts only with PartyViewPlanner and TankPickup, in combat, one cast per global
// cooldown; the cooldowns of the spells are read from the snapshot and the retry map, so a taunt that is still cooling is not sent again.
BotPolicy::TurnFlow BotPolicy::TurnTankPickup(DecideTick& tick, BotTurn& turn)
{
    if (!_options.PartyViewPlanner || !_options.TankPickup)
        return TurnFlow::kNext;
    std::uint64_t const nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    std::uint32_t const classId = turn.classId;
    bool const tankRole = state.Role_ == BotState::Role::Tank || (state.Role_ == BotState::Role::Auto && classId == kWarriorClass && _options.PartySize != 0);
    if (!tankRole || bot.in_combat == 0 || bot.life_state != 0 || nowMs < state.NextCastMs || IsCasting(bot))
        return TurnFlow::kNext;
    std::uint32_t single = 0, area = 0;
    if (classId == kWarriorClass)
        single = kTauntSpell, area = kChallengingShoutSpell;
    else if (classId == kDruidClass_)
        single = kGrowlSpell, area = kChallengingRoarSpell;
    if (single == 0)
        return TurnFlow::kNext;
    auto const at = tick.indexOf.find(bot.guid);
    forever_module::PartyView const* const party = at == tick.indexOf.end() ? nullptr : tick.view.PartyViewOf(at->second);
    if (party == nullptr)
        return TurnFlow::kNext;
    std::vector<party_view::LooseMob> const loose = party_view::LooseMobsOf(tick.view, *party);
    if (loose.empty())
        return TurnFlow::kNext;
    auto const usable = [&](std::uint32_t spell)
    {
        if (bot.level != 0 && rotation::LearnLevel(spell) > bot.level)
            return false;
        auto const retry = state.RetryAtMs.find(spell);
        if (retry != state.RetryAtMs.end() && nowMs < retry->second)
            return false;
        for (forever_module::SpellCooldown const& cooldown : bot.spell_cooldowns)
            if (cooldown.spell_id == spell && cooldown.remaining_ms != 0)
                return false;
        return CanPay(_options.SpellCosts.get(), spell, bot);
    };
    auto const near = [&](std::uint64_t ref, float yards)
    {
        for (NearbyUnit const& unit : turn.nearby)
            if (unit.guid == ref)
                return unit.health != 0 && std::hypot(unit.pos_x - bot.pos_x, unit.pos_y - bot.pos_y) <= yards;
        return false;
    };
    // TankLoosePickup (route, raid and tank-led parties): one elected tank per loose mob instead of every tank taunting the same one (the 2026-11-13 sim: bots 222 and 224 both
    // taunted the same mob in the same second while the casters died). Mobs on healers come first, then on casters, then the ones that are casting, then the newest; a mob
    // whose elected tank is another one is left to it and the next is tried. The elected tank is the nearest with a ready taunt (a claim keeps it for the cooldown).
    bool electedForTop = true;
    if (_options.TankLoosePickup && turn.routeOn)
    {
        auto const unitOf = [&](std::uint64_t ref) -> NearbyUnit const*
        {
            for (NearbyUnit const& unit : turn.nearby)
                if (unit.guid == ref)
                    return &unit;
            for (NearbyUnit const& unit : tick.LeaderNearby(turn.partyLeader, bot.map_id))
                if (unit.guid == ref)
                    return &unit;
            return nullptr;
        };
        struct Candidate
        {
            party_view::LooseMob Mob;
            NearbyUnit Unit;
            int Rank = 3;
        };
        std::vector<Candidate> candidates;
        for (party_view::LooseMob const& mob : loose)
        {
            NearbyUnit const* unit = unitOf(mob.Unit);
            if (unit == nullptr || unit->health == 0 || !unit->is_hostile ||
                (unit->state_flags & (forever_module::kNearbyTauntImmune | forever_module::kNearbyCcLocked | forever_module::kNearbyEvading)) != 0 ||
                std::hypot(unit->pos_x - bot.pos_x, unit->pos_y - bot.pos_y) > float(_options.TankLooseRangeYards))
                continue;
            Candidate candidate{ mob, *unit, 3 };
            std::uint32_t victimClass = 0;
            for (BotSnapshot const& other : tick.view.bots)
                if (PlayerRef(other.guid) == mob.Victim)
                {
                    victimClass = ClassFor(_options, other.guid);
                    break;
                }
            if (mob.VictimIsHealer)
                candidate.Rank = 0;
            else if (victimClass == 5 || victimClass == 8 || victimClass == 9)   // priest, mage, warlock: casters die first
                candidate.Rank = 1;
            else if (unit->casting != 0)
                candidate.Rank = 2;
            candidates.push_back(candidate);
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](Candidate const& a, Candidate const& b) { return a.Rank < b.Rank; });
        if (_looseClaims.size() > 256)
            for (auto it = _looseClaims.begin(); it != _looseClaims.end();)
                it = nowMs >= it->second.UntilMs ? _looseClaims.erase(it) : std::next(it);
        // The tanks of the party that could take a mob: this bot, and the members with the tank role that are bots in this fight with their taunt ready.
        struct Tank
        {
            std::uint64_t Guid = 0;
            float X = 0, Y = 0;
        };
        std::vector<Tank> tanks;
        bool const iAmReady = nowMs >= state.PickupAtMs && usable(single);
        if (iAmReady)
            tanks.push_back(Tank{ bot.guid, bot.pos_x, bot.pos_y });
        for (forever_module::PartyMember const& member : tick.view.MembersOf(*party))
        {
            if (member.is_bot == 0 || member.online == 0 || (member.role & forever_module::kPartyRoleTank) == 0 || member.guid == PlayerRef(bot.guid))
                continue;
            for (BotSnapshot const& other : tick.view.bots)
            {
                if (PlayerRef(other.guid) != member.guid)
                    continue;
                std::uint32_t const otherClass = ClassFor(_options, other.guid);
                std::uint32_t const otherSpell = otherClass == kWarriorClass ? kTauntSpell : otherClass == kDruidClass_ ? kGrowlSpell : 0;
                bool ready = otherSpell != 0 && other.life_state == 0 && other.in_combat != 0 && other.map_id == bot.map_id &&
                             (other.level == 0 || rotation::LearnLevel(otherSpell) <= other.level) && CanPay(_options.SpellCosts.get(), otherSpell, other);
                for (forever_module::SpellCooldown const& cooldown : other.spell_cooldowns)
                    if (cooldown.spell_id == otherSpell && cooldown.remaining_ms != 0)
                        ready = false;
                if (ready)
                    tanks.push_back(Tank{ other.guid, other.pos_x, other.pos_y });
                break;
            }
        }
        bool first = true;
        for (Candidate const& candidate : candidates)
        {
            std::uint64_t elected = 0;
            auto const claim = _looseClaims.find(candidate.Mob.Unit);
            if (claim != _looseClaims.end() && nowMs < claim->second.UntilMs)
                elected = claim->second.Tank;   // a tank that took it a moment ago keeps it, even if its taunt is cooling now
            else
            {
                float bestKey = 1e9f;
                for (Tank const& tank : tanks)
                {
                    float const dist = std::hypot(tank.X - candidate.Unit.pos_x, tank.Y - candidate.Unit.pos_y);
                    float const key = (dist <= kOnyxiaTauntYards ? 0.0f : 1000.0f) + dist;
                    if (key < bestKey || (key == bestKey && tank.Guid < elected))
                    {
                        bestKey = key;
                        elected = tank.Guid;
                    }
                }
            }
            if (first)
                electedForTop = elected == bot.guid;
            first = false;
            if (elected != bot.guid || !iAmReady)
                continue;
            NearbyUnit const& unit = candidate.Unit;
            float const dist = std::hypot(unit.pos_x - bot.pos_x, unit.pos_y - bot.pos_y);
            bool const losBlocked = (unit.state_flags & forever_module::kNearbyLosKnown) != 0 && (unit.state_flags & forever_module::kNearbyInLosOfBot) == 0;
            char const* const why = candidate.Rank == 0 ? "healer" : candidate.Rank == 1 ? "caster" : candidate.Rank == 2 ? "casting" : "newest";
            auto const describe = [&](char const* action)
            {
                return std::string("tank_loose: tank=") + std::to_string(bot.guid) + " target=" + std::to_string(forever_module::UnitRefEntry(candidate.Mob.Unit)) +
                       " victim=" + std::to_string(candidate.Mob.Victim) + " reason=" + why + " act=" + action + " dist=" + std::to_string(int(dist)) +
                       " loose=" + std::to_string(loose.size());
            };
            std::uint64_t const holdMs = std::max<std::uint64_t>(_options.TankLooseCooldownMs, 500);
            if (dist <= kOnyxiaTauntYards && !losBlocked)
            {
                BotAction cast;
                cast.bot_guid = bot.guid;
                cast.type = ActionType::kCastSpell;
                cast.spell_id = single;
                cast.target_guid = candidate.Mob.Unit;
                tick.actions.push_back(cast);
                state.CastTarget[single] = candidate.Mob.Unit;
                state.PickupAtMs = nowMs + holdMs;
                state.NextCastMs = nowMs + kGlobalCooldownMs;
                _looseClaims[candidate.Mob.Unit] = LooseClaim{ bot.guid, nowMs + holdMs };
                if (_options.OnEvent)
                    _options.OnEvent(describe("taunt"));
                return TurnFlow::kSkip;
            }
            // Out of reach or out of sight: a short walk toward it, last resort, never off the chase leash.
            if (_options.TankLooseChaseYards != 0 && dist > 0.5f &&
                !OffPartyLeash(tick, turn.partyLeader, bot.map_id, unit.pos_x, unit.pos_y, _options.DungeonChaseLeashYards, true, false))
            {
                float const step = std::min(float(_options.TankLooseChaseYards), losBlocked ? dist : std::max(dist - (kOnyxiaTauntYards - 5.0f), 2.0f));
                Point const dest{ bot.pos_x + (unit.pos_x - bot.pos_x) / dist * step, bot.pos_y + (unit.pos_y - bot.pos_y) / dist * step, bot.pos_z };
                BotAction move;
                move.bot_guid = bot.guid;
                move.type = ActionType::kMoveTo;
                move.dest_x = dest.X;
                move.dest_y = dest.Y;
                move.dest_z = dest.Z;
                tick.actions.push_back(move);
                state.Last = Decision{ ActionType::kMoveTo, candidate.Mob.Unit, dest };
                state.LastMove = state.Last;
                state.PickupAtMs = nowMs + holdMs;
                _looseClaims[candidate.Mob.Unit] = LooseClaim{ bot.guid, nowMs + holdMs };
                if (_options.OnEvent)
                    _options.OnEvent(describe(losBlocked ? "approach_los" : "approach"));
                return TurnFlow::kSkip;
            }
        }
        // The single taunt is decided here; the area taunt below only runs for the tank elected for the top mob.
        if (!electedForTop || candidates.empty())
            return TurnFlow::kNext;
    }
    // The area taunt: two or more loose mobs, most of them around the tank.
    if (loose.size() >= 2 && nowMs >= state.PickupAoeAtMs && usable(area))
    {
        std::size_t inReach = 0;
        for (party_view::LooseMob const& mob : loose)
            inReach += near(mob.Unit, kAreaTauntYards) ? 1 : 0;
        if (inReach >= 2)
        {
            BotAction cast;
            cast.bot_guid = bot.guid;
            cast.type = ActionType::kCastSpell;
            cast.spell_id = area;
            cast.target_guid = 0;
            tick.actions.push_back(cast);
            state.CastTarget[area] = 0;
            state.PickupAoeAtMs = nowMs + kPickupAoeRetryMs;
            state.NextCastMs = nowMs + kGlobalCooldownMs;
            if (_options.OnEvent)
                _options.OnEvent("tank_pickup: bot=" + std::to_string(bot.guid) + " spell=" + std::to_string(area) + " loose=" + std::to_string(loose.size()));
            return TurnFlow::kSkip;
        }
    }
    if (!(_options.TankLoosePickup && turn.routeOn) && nowMs >= state.PickupAtMs && usable(single))
    {
        std::uint64_t const target = party_view::TauntTargetFor(tick.view, *party, PlayerRef(bot.guid));
        if (target != 0 && near(target, kOnyxiaTauntYards))
        {
            BotAction cast;
            cast.bot_guid = bot.guid;
            cast.type = ActionType::kCastSpell;
            cast.spell_id = single;
            cast.target_guid = target;
            tick.actions.push_back(cast);
            state.CastTarget[single] = target;
            state.PickupAtMs = nowMs + kPickupRetryMs;
            state.NextCastMs = nowMs + kGlobalCooldownMs;
            if (_options.OnEvent)
                _options.OnEvent("tank_pickup: bot=" + std::to_string(bot.guid) + " spell=" + std::to_string(single) + " target=" + std::to_string(target) +
                                 " loose=" + std::to_string(loose.size()));
            return TurnFlow::kSkip;
        }
    }
    return TurnFlow::kNext;
}

// The raid rules' two jobs outside the interrupt and kill focus: a dispel the party planner gave this bot (the debuff kinds of data/raid_rules.txt), and the taunt of the
// new main tank after the planner swapped the tanks on the rule table's debuff (Pierce Armor on Jeklik). Both are a single instant cast, and neither runs without the
// RaidRulesFile and a plan.
BotPolicy::TurnFlow BotPolicy::TurnRaidRules(DecideTick& tick, BotTurn& turn)
{
    if (!_options.RaidRules)
        return TurnFlow::kNext;
    std::uint64_t const nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    if (turn.partyLeader == 0 || bot.life_state != 0 || nowMs < state.NextCastMs || IsCasting(bot))
        return TurnFlow::kNext;
    party_planner::Order const* const plan = tick.PlanOf(turn.partyLeader);
    if (plan == nullptr)
        return TurnFlow::kNext;

    if (plan->TankSwapped && plan->MainTank == bot.guid && turn.classId == kWarriorClass && bot.in_combat != 0 && nowMs >= state.SwapTauntAtMs)
    {
        for (NearbyUnit const& unit : turn.nearby)
            if (unit.is_hostile && unit.health != 0 && unit.guid != bot.threat_target_guid &&
                _options.RaidRules->IsBoss(bot.map_id, forever_module::UnitRefEntry(unit.guid)) &&
                std::hypot(unit.pos_x - bot.pos_x, unit.pos_y - bot.pos_y) <= kOnyxiaTauntYards)
            {
                BotAction taunt;
                taunt.bot_guid = bot.guid;
                taunt.type = ActionType::kCastSpell;
                taunt.spell_id = kTauntSpell;
                taunt.target_guid = unit.guid;
                tick.actions.push_back(taunt);
                state.CastTarget[kTauntSpell] = unit.guid;
                state.SwapTauntAtMs = nowMs + kOnyxiaTauntCdMs;
                state.NextCastMs = nowMs + kGlobalCooldownMs;
                if (_options.OnEvent)
                    _options.OnEvent("raid_swap_taunt: bot=" + std::to_string(bot.guid) + " boss=" + std::to_string(forever_module::UnitRefEntry(unit.guid)));
                return TurnFlow::kSkip;
            }
    }

    if (party_planner::DispelAssignment const* const job = party_planner::DispelFor(plan, bot.guid))
    {
        std::uint32_t const spell = DispelSpellFor(turn.classId, bot.level, job->Kind);
        auto const retry = state.RetryAtMs.find(spell);
        auto const patient = tick.indexOf.find(job->Target);
        if (spell == 0 || (retry != state.RetryAtMs.end() && nowMs < retry->second) || patient == tick.indexOf.end())
            return TurnFlow::kNext;
        BotSnapshot const& member = tick.view.bots[patient->second];
        if (member.life_state != 0 || Distance2d(bot, member) > kHealRangeYards)
            return TurnFlow::kNext;
        BotAction dispel;
        dispel.bot_guid = bot.guid;
        dispel.type = ActionType::kCastSpell;
        dispel.spell_id = spell;
        dispel.target_guid = member.guid == bot.guid ? 0 : PlayerRef(member.guid);
        tick.actions.push_back(dispel);
        state.CastTarget[spell] = dispel.target_guid;
        state.RetryAtMs[spell] = nowMs + kDispelRetryMs;   // the aura is still listed for a tick or two after the cast
        state.NextCastMs = nowMs + kGlobalCooldownMs;
        if (_options.OnEvent)
            _options.OnEvent("raid_dispel: bot=" + std::to_string(bot.guid) + " spell=" + std::to_string(spell) + " target=" + std::to_string(member.guid) +
                             " aura=" + std::to_string(job->Aura));
        return TurnFlow::kSkip;
    }
    return TurnFlow::kNext;
}
