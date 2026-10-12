#include "bot_policy.h"

#include <iterator>
#include <limits>
#include <span>
#include <unordered_set>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotSnapshot;
using forever_module::NearbyUnit;

namespace
{

// High guid types of the unit references in snapshots (forever_module::MakeUnitRef).
constexpr std::uint8_t kPlayerType = 2;
constexpr std::uint8_t kCreatureType = 8;

// The closest unit in `units` that satisfies `wanted`, or nullptr.
NearbyUnit const* Closest(BotSnapshot const& bot, std::span<NearbyUnit const> units, bool (*wanted)(NearbyUnit const&))
{
    NearbyUnit const* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    for (NearbyUnit const& unit : units)
    {
        if (!wanted(unit))
            continue;
        float const dx = unit.pos_x - bot.pos_x;
        float const dy = unit.pos_y - bot.pos_y;
        float const dist = dx * dx + dy * dy;
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
           (forever_module::UnitRefType(unit.guid) == kCreatureType || forever_module::UnitRefType(unit.guid) == kPlayerType);
}

bool IsPlayer(NearbyUnit const& unit)
{
    return forever_module::UnitRefType(unit.guid) == kPlayerType && unit.health > 0;
}

} // namespace

char const* ActionStatusName(forever_module::BotActionStatus status)
{
    using forever_module::BotActionStatus;
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

std::vector<BotAction> BotPolicy::Decide(forever_module::WorldTickSnapshotView const& view)
{
    std::vector<BotAction> actions;
    std::unordered_set<std::uint64_t> present;

    for (std::size_t i = 0; i < view.bots.size(); ++i)
    {
        BotSnapshot const& bot = view.bots[i];
        present.insert(bot.guid);

        // A dead bot gets no orders; its last decision stays in place until it is alive again.
        if (bot.health == 0)
            continue;

        std::span<NearbyUnit const> nearby = view.NearbyOf(i);
        NearbyUnit const* player = Closest(bot, nearby, IsPlayer);

        // Below 30% health the bot disengages: it follows the closest player, or stands still if none is near.
        bool const lowHealth = static_cast<std::uint64_t>(bot.health) * 10 < static_cast<std::uint64_t>(bot.max_health) * 3;

        Decision wanted;
        if (lowHealth)
        {
            wanted = player ? Decision{ ActionType::kFollow, player->guid } : Decision{ ActionType::kStop, 0 };
        }
        else if (bot.threat_target_guid != 0)
        {
            wanted = { ActionType::kAttackTarget, bot.threat_target_guid };
        }
        else if (NearbyUnit const* hostile = Closest(bot, nearby, IsLiveHostile))
        {
            wanted = { ActionType::kAttackTarget, hostile->guid };
        }
        else if (player)
        {
            wanted = { ActionType::kFollow, player->guid };
        }
        else
        {
            wanted = { ActionType::kStop, 0 };
        }

        auto const last = _last.find(bot.guid);
        if (last == _last.end() || last->second.Type != wanted.Type || last->second.Target != wanted.Target)
        {
            BotAction action;
            action.bot_guid = bot.guid;
            action.type = wanted.Type;
            action.target_guid = wanted.Target;
            actions.push_back(action);
            _last[bot.guid] = wanted;
        }

        // Test cast: at the current attack target, on its own timer, after the attack decision above.
        if (_options.TestSpell != 0 && wanted.Type == ActionType::kAttackTarget && wanted.Target != 0)
        {
            auto const lastCast = _lastCastTick.find(bot.guid);
            if (lastCast == _lastCastTick.end() || view.tick_number >= lastCast->second + _options.CastEveryTicks)
            {
                BotAction cast;
                cast.bot_guid = bot.guid;
                cast.type = ActionType::kCastSpell;
                cast.spell_id = _options.TestSpell;
                cast.target_guid = wanted.Target;
                actions.push_back(cast);
                _lastCastTick[bot.guid] = view.tick_number;
            }
        }
    }

    for (auto it = _last.begin(); it != _last.end();)
        it = present.count(it->first) ? std::next(it) : _last.erase(it);
    for (auto it = _lastCastTick.begin(); it != _lastCastTick.end();)
        it = present.count(it->first) ? std::next(it) : _lastCastTick.erase(it);

    return actions;
}
