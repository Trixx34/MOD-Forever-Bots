#include "bot_policy.h"

#include "combat_rotation.h"

#include <iterator>
#include <limits>
#include <span>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotActionStatus;
using forever_module::BotSnapshot;
using forever_module::NearbyUnit;
using forever_module::WorldTickSnapshotView;

namespace {

constexpr float kMeleeRange = 5.0f;          // yards: approach beyond this, attack within it
constexpr float kDisengagePct = 30.0f;       // below this health the bot stops fighting
constexpr std::uint64_t kGlobalCooldownMs = 1500;
constexpr std::uint64_t kRetryMs = 1000;     // out of range, invalid target, refused, not enough power
constexpr std::uint64_t kCooldownRetryMs = 2000;
constexpr std::uint64_t kNotKnownRetryMs = 60000; // the spell may be learned later

// The closest unit in `units` that satisfies `wanted`, or nullptr.
NearbyUnit const* Closest(BotSnapshot const& bot, std::span<NearbyUnit const> units, bool (*wanted)(NearbyUnit const&))
{
    NearbyUnit const* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    for (NearbyUnit const& unit : units)
    {
        if (!wanted(unit))
            continue;
        float const dist = rotation::DistanceYards(bot, unit);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = &unit;
        }
    }
    return best;
}

bool IsLiveHostile(NearbyUnit const& unit)
{
    return unit.is_hostile && unit.health > 0 &&
           (forever_module::UnitRefType(unit.guid) == 8 || forever_module::UnitRefType(unit.guid) == 2);
}

bool IsPlayer(NearbyUnit const& unit)
{
    return forever_module::UnitRefType(unit.guid) == 2 && unit.health > 0;
}

} // namespace

char const* ActionStatusName(BotActionStatus status)
{
    switch (status)
    {
        case BotActionStatus::kNone: return "none";
        case BotActionStatus::kApplied: return "applied";
        case BotActionStatus::kRefused: return "refused";
        case BotActionStatus::kUnknownSpell: return "unknown spell";
        case BotActionStatus::kNotKnown: return "spell not known";
        case BotActionStatus::kOutOfRange: return "out of range";
        case BotActionStatus::kOnCooldown: return "on cooldown";
        case BotActionStatus::kNotEnoughPower: return "not enough power";
        case BotActionStatus::kNoLineOfSight: return "no line of sight";
        case BotActionStatus::kInvalidTarget: return "invalid target";
        case BotActionStatus::kNotOnline: return "not online";
    }
    return "unknown";
}

// Reads the result of the bot's last action, once per sequence number, and sets when the spell may be tried again.
void BotPolicy::ReadResult(BotState& state, BotSnapshot const& bot, std::uint64_t nowMs)
{
    if (bot.last_action_sequence == 0 || bot.last_action_sequence == state.LastSequence)
        return;
    state.LastSequence = bot.last_action_sequence;

    if (bot.last_action_type != uint8_t(ActionType::kCastSpell) || bot.last_action_spell == 0)
        return;

    std::uint32_t const spell = bot.last_action_spell;
    switch (bot.last_action_status)
    {
        case BotActionStatus::kNotKnown:
        case BotActionStatus::kUnknownSpell:
            state.RetryAtMs[spell] = nowMs + kNotKnownRetryMs;
            break;
        case BotActionStatus::kOnCooldown:
            state.RetryAtMs[spell] = nowMs + kCooldownRetryMs;
            break;
        case BotActionStatus::kOutOfRange:
        case BotActionStatus::kNoLineOfSight:
        case BotActionStatus::kNotEnoughPower:
        case BotActionStatus::kInvalidTarget:
        case BotActionStatus::kRefused:
            state.RetryAtMs[spell] = nowMs + kRetryMs;
            break;
        case BotActionStatus::kApplied:
        case BotActionStatus::kNone:
        case BotActionStatus::kNotOnline:
            break;
    }
}

std::vector<BotAction> BotPolicy::Decide(WorldTickSnapshotView const& view, std::uint64_t nowMs)
{
    std::vector<BotAction> actions;
    std::unordered_set<std::uint64_t> present;

    for (std::size_t i = 0; i < view.bots.size(); ++i)
    {
        BotSnapshot const& bot = view.bots[i];
        present.insert(bot.guid);
        BotState& state = _bots[bot.guid];
        ReadResult(state, bot, nowMs);

        // A dead bot gets no orders; its last decision stays in place until it is alive again.
        if (bot.health == 0)
            continue;

        std::span<NearbyUnit const> nearby = view.NearbyOf(i);
        NearbyUnit const* player = Closest(bot, nearby, IsPlayer);
        bool const lowHealth = bot.max_health > 0 && std::uint64_t(bot.health) * 100 < std::uint64_t(bot.max_health) * kDisengagePct;

        // The target is the threat target (even when it is out of view), otherwise the closest live hostile.
        NearbyUnit const* targetUnit = nullptr;
        std::uint64_t targetRef = bot.threat_target_guid;
        if (targetRef != 0)
        {
            for (NearbyUnit const& unit : nearby)
                if (unit.guid == targetRef)
                    targetUnit = &unit;
        }
        else if (NearbyUnit const* hostile = Closest(bot, nearby, IsLiveHostile))
        {
            targetUnit = hostile;
            targetRef = hostile->guid;
        }
        bool const hasTarget = targetRef != 0 && !lowHealth;
        float const distance = targetUnit ? rotation::DistanceYards(bot, *targetUnit) : bot.target_distance;

        Decision wanted;
        if (lowHealth)
        {
            wanted = player ? Decision{ ActionType::kFollow, player->guid } : Decision{ ActionType::kStop, 0 };
        }
        else if (targetRef != 0)
        {
            if (!state.InFight)
            {
                state.InFight = true;
                state.FightStartMs = nowMs;
            }
            wanted = distance > kMeleeRange ? Decision{ ActionType::kFollow, targetRef } : Decision{ ActionType::kAttackTarget, targetRef };
        }
        else
        {
            state.InFight = false;
            wanted = player ? Decision{ ActionType::kFollow, player->guid } : Decision{ ActionType::kStop, 0 };
        }

        if (!state.Last || state.Last->Type != wanted.Type || state.Last->Target != wanted.Target)
        {
            BotAction action;
            action.bot_guid = bot.guid;
            action.type = wanted.Type;
            action.target_guid = wanted.Target;
            actions.push_back(action);
            state.Last = wanted;
        }

        if (!hasTarget)
            continue;

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
                state.LastTestCastTick = view.tick_number;
            }
            continue;
        }

        // Class rotation: the first row that is off cooldown, in range and allowed by its rule. One cast per global cooldown.
        if (nowMs < state.NextCastMs)
            continue;
        rotation::Facts const facts = rotation::FactsFor(bot, nearby, targetUnit, state.InFight ? nowMs - state.FightStartMs : 0);
        for (rotation::Row const& row : rotation::RowsForClass(_options.ClassId))
        {
            auto const retry = state.RetryAtMs.find(row.Spell);
            if (retry != state.RetryAtMs.end() && nowMs < retry->second)
                continue;
            if (row.OnTarget && (distance < row.MinRange || (row.MaxRange > 0.0f && distance > row.MaxRange)))
                continue;
            if (!rotation::Allowed(row.When, facts))
                continue;

            BotAction cast;
            cast.bot_guid = bot.guid;
            cast.type = ActionType::kCastSpell;
            cast.spell_id = row.Spell;
            cast.target_guid = row.OnTarget ? targetRef : 0;
            actions.push_back(cast);
            state.NextCastMs = nowMs + kGlobalCooldownMs;
            break;
        }
    }

    for (auto it = _bots.begin(); it != _bots.end();)
        it = present.count(it->first) ? std::next(it) : _bots.erase(it);

    return actions;
}
