// SPDX-License-Identifier: GPL-2.0-or-later
// BotPolicy::Decide, a bot's turn, its casts: heal orders, party healing and resurrects, the hunter pet, upkeep buffs, crafting and the rotation (the steps DecideBot runs in order; bot_policy.cpp).
#include "bot_policy_internal.h"

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
    std::uint32_t const healSpell = state.Role_ == BotState::Role::Dps ? 0u : HealSpellFor(classId, bot.level);
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
        std::uint64_t leaderHurtPct = _options.DungeonTankHealBelowPct;
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
                ++lowCount;
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
        auto const lead = leaderOf.find(bot.guid);
        for (BotSnapshot const& other : view.bots)
            if (other.guid != bot.guid && leaderOf.count(other.guid) && leaderOf[other.guid] == lead->second)
                consider(other);
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
                    actions.push_back(heal);
                    state.CastTarget[spellToCast] = heal.target_guid;
                    if (spellToCast == kRenew)
                        state.BuffUntilMs[{ kRenew, PlayerRef(target->guid) }] = nowMs + kRenewMs;
                    if (spellToCast == kPowerWordShield)
                        state.BuffUntilMs[{ kPowerWordShield, heal.target_guid }] = nowMs + kShieldLockMs;
                    _healClaims[target->guid] = HealClaim{ healGroup, nowMs + kHealClaimMs };
                    if (_options.OnEvent)
                        _options.OnEvent("heal: healer=" + std::to_string(bot.guid) + " spell=" + std::to_string(spellToCast) +
                                         " target=" + std::to_string(target->guid) + " target_hp_pct=" + std::to_string(hurtPct) +
                                         (note.empty() ? "" : " " + note));
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
            if (!row.Buff || (bot.level != 0 && (row.MinLevel > bot.level || (row.MaxLevel != 0 && bot.level > row.MaxLevel))))
                continue;
            // A buff row may carry a condition: a stance only in a party (in_party), a pet summon only with no pet (no_pet).
            if (row.When.Condition != rotation::Cond::Always && !rotation::Allowed(row.When, buffFacts))
                continue;
            auto const retry = state.RetryAtMs.find(row.Spell);
            if (retry != state.RetryAtMs.end() && nowMs < retry->second)
                continue;
            if (!CanPay(_options.SpellCosts.get(), row.Spell, bot))
                continue; // a warrior has no rage out of combat: Battle Shout waits until it has some, instead of a cast the core refuses
            // A pet summon (no_pet) is due whenever the snapshot shows no pet: the party claim and the buff timer below would hold it off for the
            // spell's duration (an hour), long after the demon died. The cast in progress and the global cooldown stop it being sent twice.
            if (row.When.Condition == rotation::Cond::NoPet)
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
            std::vector<std::uint64_t> recipients{ 0 };
            if (row.Party)
                for (BotSnapshot const& other : view.bots)
                    if (other.guid != bot.guid && other.life_state == 0 && other.group_leader_guid != 0 && other.group_leader_guid == bot.group_leader_guid &&
                        Distance2d(bot, other) <= kBuffRangeYards)
                        recipients.push_back(PlayerRef(other.guid));
            for (std::uint64_t recipient : recipients)
            {
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
                    if (!row.Buff || !row.Party || (bot.level != 0 && (row.MinLevel > bot.level || (row.MaxLevel != 0 && bot.level > row.MaxLevel))))
                        continue;
                    if (row.When.Condition != rotation::Cond::Always)
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
    auto& hasTarget = turn.hasTarget;
    auto const& distance = turn.distance;
    auto& wanted = turn.wanted;

    if (!hasTarget)
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

    for (rotation::Row const& row : RowsFor(classId, spec))
    {
        if (row.Buff)
            continue; // upkeep spells are cast out of combat, below
        if (row.Spell == kAutoShot && state.AutoShotTarget == targetRef)
            continue; // already shooting this target
        if (row.OnTarget && row.MaxRange > kMeleeApproachYards + 1.0f && NoAmmo(bot, classId))
            continue; // a ranged attack of a hunter with no ammunition: refused by the core, so only the melee rows are used
        if (row.MaxLevel != 0 && bot.level > row.MaxLevel)
            continue; // a better spell has replaced it
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
        if (!rotation::Allowed(row.When, facts))
            continue;
        // Only one member of the party stops a cast: when a party member is already casting an interrupt, this bot uses its next row.
        if (row.When.Condition == rotation::Cond::TargetCasting && InterruptTaken(tick, bot, partyLeader))
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
        break;
    }
    return TurnFlow::kNext;
}
