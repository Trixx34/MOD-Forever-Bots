#include "bot_policy.h"

#include "combat_rotation.h"

#include <cmath>
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
constexpr float kPullRadius = 10.0f;         // yards: hostiles this close to a candidate are pulled with it
constexpr std::size_t kMaxPull = 3;          // a group larger than this is not pulled (over-pull control)
constexpr std::uint32_t kRend = 772;         // warrior rank 1; the bleed lasts 21 seconds
constexpr std::uint64_t kRendDurationMs = 21000;
// kVendor's spell_id bits (the core's ModuleBotActions): sell grey items, repair, equip the best gear.
constexpr std::uint32_t kServiceSell = 1;
constexpr std::uint32_t kServiceRepair = 2;
constexpr std::uint32_t kServiceEquip = 4;
constexpr std::uint32_t kRestBelowPct = 90;  // an idle bot below this health sits down to rest

// Duration of the damage-over-time effect a spell leaves on its target, or 0 for spells the policy does not track.
std::uint64_t DotDurationMs(std::uint32_t spell)
{
    return spell == kRend ? kRendDurationMs : 0;
}

float DistanceBetween(NearbyUnit const& a, NearbyUnit const& b)
{
    float const dx = a.pos_x - b.pos_x;
    float const dy = a.pos_y - b.pos_y;
    return std::sqrt(dx * dx + dy * dy);
}

bool IsLiveHostile(NearbyUnit const& unit);

// The closest live hostile that is not part of a group larger than kMaxPull, or nullptr. A group is the hostiles within
// kPullRadius of the candidate, the candidate included. Units already attacking the bot are not chosen here; the policy
// fights its threat target whatever the group size.
NearbyUnit const* PullTarget(BotSnapshot const& bot, std::span<NearbyUnit const> units)
{
    NearbyUnit const* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    for (NearbyUnit const& candidate : units)
    {
        if (!IsLiveHostile(candidate))
            continue;
        float const dist = rotation::DistanceYards(bot, candidate);
        if (dist >= bestDist)
            continue;

        std::size_t group = 0;
        for (NearbyUnit const& other : units)
            if (IsLiveHostile(other) && DistanceBetween(candidate, other) <= kPullRadius)
                ++group;
        if (group > kMaxPull)
            continue;

        bestDist = dist;
        best = &candidate;
    }
    return best;
}

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

    // The core refuses a sit while in combat: the bot is not sitting, and the sit waits a second.
    if (bot.last_action_type == uint8_t(ActionType::kRest) && bot.last_action_status == BotActionStatus::kRefused)
    {
        state.Sitting = false;
        state.RestRetryAtMs = nowMs + kRetryMs;
        return;
    }

    if (bot.last_action_type != uint8_t(ActionType::kCastSpell) || bot.last_action_spell == 0)
        return;

    std::uint32_t const spell = bot.last_action_spell;
    if (bot.last_action_status == BotActionStatus::kApplied && DotDurationMs(spell) != 0)
    {
        auto const cast = state.CastTarget.find(spell);
        if (cast != state.CastTarget.end() && cast->second != 0)
            state.Dots[spell] = Dot{ cast->second, nowMs + DotDurationMs(spell) };
    }

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
        else if (NearbyUnit const* hostile = PullTarget(bot, nearby))
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

        // Rest: sit down while idle and hurt, and stand before anything moves or fights. The stand goes out before the
        // movement below, in the same action set, so the bot never moves sitting.
        // Idle means no fight and no player to follow, with no live hostile anywhere near: a sitting bot is easy prey.
        bool enemyNear = false;
        for (NearbyUnit const& unit : nearby)
            enemyNear = enemyNear || IsLiveHostile(unit);
        bool const idle = !hasTarget && !enemyNear && wanted.Type == ActionType::kStop;
        bool const hurtEnough = bot.max_health > 0 && std::uint64_t(bot.health) * 100 < std::uint64_t(bot.max_health) * kRestBelowPct;
        bool const stillHurt = bot.max_health > 0 && bot.health < bot.max_health;
        bool const sitWanted = idle && (state.Sitting ? stillHurt : hurtEnough) && nowMs >= state.RestRetryAtMs;
        if (sitWanted != state.Sitting)
        {
            BotAction rest;
            rest.bot_guid = bot.guid;
            rest.type = ActionType::kRest;
            rest.spell_id = sitWanted ? 1 : 0;
            actions.push_back(rest);
            state.Sitting = sitWanted;
        }

        // Services (M3, M4) for an idle bot: a vendor visit on a timer (sell grey, repair, equip), and a trainer check on a
        // timer or at once after a level-up. The core refuses both when no NPC is in interaction range, which is expected
        // until travel (M6) takes bots to them.
        if (idle && _options.ServiceEveryMs != 0 && nowMs >= state.NextServiceMs)
        {
            BotAction vendor;
            vendor.bot_guid = bot.guid;
            vendor.type = ActionType::kVendor;
            vendor.spell_id = kServiceSell | kServiceRepair | kServiceEquip;
            actions.push_back(vendor);
            state.NextServiceMs = nowMs + _options.ServiceEveryMs;
        }
        bool const levelUp = bot.level != 0 && bot.level != state.LastTrainLevel;
        if (idle && _options.TrainEveryMs != 0 && (levelUp || nowMs >= state.NextTrainMs))
        {
            BotAction train;
            train.bot_guid = bot.guid;
            train.type = ActionType::kTrain;
            actions.push_back(train);
            state.LastTrainLevel = bot.level;
            state.NextTrainMs = nowMs + _options.TrainEveryMs;
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

        // Spells the core reports as cooling down right now.
        std::unordered_set<std::uint32_t> cooling;
        for (forever_module::SpellCooldown const& cooldown : bot.spell_cooldowns)
            if (cooldown.spell_id != 0 && cooldown.remaining_ms != 0)
                cooling.insert(cooldown.spell_id);

        std::uint32_t classId = _options.ClassId;
        if (_options.ClassOf)
            if (std::uint32_t const own = _options.ClassOf(bot.guid))
                classId = own;
        for (rotation::Row const& row : rotation::RowsForClass(classId))
        {
            auto const retry = state.RetryAtMs.find(row.Spell);
            if (retry != state.RetryAtMs.end() && nowMs < retry->second)
                continue;
            if (cooling.count(row.Spell))
                continue;
            if (row.OnTarget && (distance < row.MinRange || (row.MaxRange > 0.0f && distance > row.MaxRange)))
                continue;
            if (!rotation::Allowed(row.When, facts))
                continue;
            // The effect is still on the target: recasting only refreshes it.
            auto const dot = state.Dots.find(row.Spell);
            if (row.OnTarget && dot != state.Dots.end() && dot->second.Target == targetRef && nowMs < dot->second.UntilMs)
                continue;

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
    }

    for (auto it = _bots.begin(); it != _bots.end();)
        it = present.count(it->first) ? std::next(it) : _bots.erase(it);

    return actions;
}
