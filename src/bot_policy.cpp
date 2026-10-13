#include "bot_policy.h"

#include "combat_rotation.h"

#include <algorithm>
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
// Combo points: the rogue's Sinister Strike spell and the spend, and the points a finisher needs.
constexpr std::uint32_t kSinisterStrike = 1752;
constexpr std::uint32_t kEviscerate = 2098;
constexpr std::uint32_t kMaxComboPoints = 5;
// A ranged bot below this share of its power walks to melee range and fights there, since its spells are spent.
constexpr std::uint32_t kOutOfPowerPct = 15;
constexpr float kMeleeApproachYards = 5.0f;
constexpr std::uint32_t kRestBelowPct = 90;  // an idle bot below this health sits down to rest
// Parties (M8). kGroup's spell_id (the core's ModuleBotActions): invite the bot named by target_guid, accept the bot's
// pending invite, leave the party.
constexpr std::uint32_t kGroupInvite = 1;
constexpr std::uint32_t kGroupAccept = 2;
constexpr std::uint32_t kGroupLeave = 3;
constexpr std::uint32_t kWarriorClass = 1;
constexpr std::uint32_t kPlayerType = 2;     // unit reference type of a player (MakeUnitRef's highType)
constexpr std::uint64_t kInviteRetryMs = 15000; // a member is invited again this long after the last invite
constexpr std::uint64_t kPartyRetryMs = 2000;   // a party action refused, or just sent, is not repeated before this
constexpr float kFollowLeaderYards = 8.0f;      // a member this close to its leader stays where it is
constexpr std::uint64_t kApproachTimeoutMs = 30000; // walking to a target this long without reaching it: avoid the target
constexpr std::uint32_t kMaxLevelAbove = 2;      // a bot does not pull a creature more than this many levels above it
constexpr float kLootRangeYards = 25.0f;         // the core loots a corpse within 30 yards; the bot asks within 25
constexpr std::uint64_t kLootWindowMs = 120000;  // a corpse is looted within this long after the fight, or forgotten
constexpr std::uint64_t kAvoidMs = 30000;        // a target that refused an attack or a cast is skipped for this long
constexpr std::uint32_t kMaxAvoidDoublings = 2;  // each repeat avoid doubles the time: 30 s, 60 s, then 120 s at most
constexpr std::uint64_t kWanderIdleMs = 30000;   // idle for this long, seeing nothing, before walking to a hub
constexpr std::uint64_t kWanderTimeoutMs = 180000; // a walk to a hub longer than this is dropped
constexpr float kHubReachYards = 10.0f;          // a bot this close to its hub has arrived
constexpr float kVendorReachYards = 4.0f;       // a bot this close to its vendor can use it (the core allows 5)
constexpr std::uint64_t kTravelTimeoutMs = 180000; // a walk to the vendor longer than this is dropped for now

// Duration of the damage-over-time effect a spell leaves on its target, or 0 for spells the policy does not track. Rank 1
// durations: while the effect runs, the policy does not recast the spell on the same target.
std::uint64_t DotDurationMs(std::uint32_t spell)
{
    switch (spell)
    {
        case kRend: return kRendDurationMs;
        case 172: return 18000;  // Corruption
        case 707: return 15000;  // Immolate
        case 589: return 18000;  // Shadow Word: Pain
        case 1978: return 15000; // Serpent Sting
        case 8050: return 12000; // Flame Shock
        case 8921: return 12000; // Moonfire
        case 21084: return 30000; // Seal of Righteousness (self)
        case 17: return 30000;    // Power Word: Shield (self)
        default: return 0;
    }
}

float DistanceBetween(NearbyUnit const& a, NearbyUnit const& b)
{
    float const dx = a.pos_x - b.pos_x;
    float const dy = a.pos_y - b.pos_y;
    return std::sqrt(dx * dx + dy * dy);
}

bool IsLiveMob(NearbyUnit const& unit);

// The closest live hostile that is not part of a group larger than kMaxPull, or nullptr. A group is the hostiles within
// kPullRadius of the candidate, the candidate included. Units already attacking the bot are not chosen here; the policy
// fights its threat target whatever the group size.
NearbyUnit const* PullTarget(BotSnapshot const& bot, std::span<NearbyUnit const> units,
                            std::function<bool(std::uint64_t)> const& avoided)
{
    NearbyUnit const* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    for (NearbyUnit const& candidate : units)
    {
        if (!IsLiveMob(candidate) || avoided(candidate.guid))
            continue;
        // Not worth the fight: well above the bot's level, or an elite. (A level of 0 is unknown and not judged.) A creature that
        // attacks the bot is still fought, since it comes in as the threat target.
        if (bot.level != 0 && ((candidate.level != 0 && candidate.level > bot.level + kMaxLevelAbove) || candidate.elite))
            continue;
        float const dist = rotation::DistanceYards(bot, candidate);
        if (dist >= bestDist)
            continue;

        std::size_t group = 0;
        for (NearbyUnit const& other : units)
            if (IsLiveMob(other) && DistanceBetween(candidate, other) <= kPullRadius)
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

// A live creature the bot may pick as its own target. Players are never picked: bots of the other faction would otherwise
// fight each other, and a player that attacks the bot still comes in as the threat target.
bool IsLiveMob(NearbyUnit const& unit)
{
    return unit.is_hostile && unit.health > 0 && forever_module::UnitRefType(unit.guid) == 8;
}

// A friendly player the bot may follow. A hostile player is not followed: it would lead the bot into a fight.
bool IsPlayer(NearbyUnit const& unit)
{
    return forever_module::UnitRefType(unit.guid) == 2 && unit.health > 0 && !unit.is_hostile;
}

// Avoids a unit for kAvoidMs, doubled for each earlier time it was avoided, so a unit that keeps coming back (a trainer the
// bot cannot attack) is left alone for longer each time.
void AvoidUnit(std::unordered_map<std::uint64_t, std::uint64_t>& avoidUntil, std::unordered_map<std::uint64_t, std::uint32_t>& count,
               std::uint64_t ref, std::uint64_t nowMs)
{
    std::uint32_t const times = ++count[ref];
    std::uint32_t const doublings = std::min<std::uint32_t>(times - 1, kMaxAvoidDoublings);
    avoidUntil[ref] = nowMs + (kAvoidMs << doublings);
}

// The live creature in `units` with this reference, or nullptr.
NearbyUnit const* FindLiveMob(std::span<NearbyUnit const> units, std::uint64_t guid)
{
    for (NearbyUnit const& unit : units)
        if (unit.guid == guid && IsLiveMob(unit))
            return &unit;
    return nullptr;
}

// A player's unit reference, from its character guid counter (the same reference the core's party actions take).
std::uint64_t PlayerRef(std::uint64_t counter)
{
    return forever_module::MakeUnitRef(static_cast<std::uint8_t>(kPlayerType), 0, counter);
}

float Distance2d(BotSnapshot const& a, BotSnapshot const& b)
{
    float const dx = a.pos_x - b.pos_x;
    float const dy = a.pos_y - b.pos_y;
    return std::sqrt(dx * dx + dy * dy);
}

float DistanceTo(BotSnapshot const& bot, Point const& point)
{
    float const dx = bot.pos_x - point.X;
    float const dy = bot.pos_y - point.Y;
    return std::sqrt(dx * dx + dy * dy);
}

// The class of a bot: its own from the class table, else the configured class (0 = none).
std::uint32_t ClassFor(PolicyOptions const& options, std::uint64_t guid)
{
    if (options.ClassOf)
        if (std::uint32_t const own = options.ClassOf(guid))
            return own;
    return options.ClassId;
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

    // A party action refused (no invite waiting, a full party): the party waits a moment before the next one.
    if (bot.last_action_type == uint8_t(ActionType::kGroup) && bot.last_action_status == BotActionStatus::kRefused)
    {
        state.PartyRetryAtMs = nowMs + kPartyRetryMs;
        return;
    }

    // A melee attack refused or with no valid target: that unit is avoided for kAvoidMs, so the bot picks another.
    if (bot.last_action_type == uint8_t(ActionType::kAttackTarget) &&
        (bot.last_action_status == BotActionStatus::kRefused || bot.last_action_status == BotActionStatus::kInvalidTarget))
    {
        if (state.Last && state.Last->Type == ActionType::kAttackTarget && state.Last->Target != 0)
            AvoidUnit(state.AvoidUntilMs, state.AvoidCount, state.Last->Target, nowMs);
        return;
    }

    if (bot.last_action_type != uint8_t(ActionType::kCastSpell) || bot.last_action_spell == 0)
        return;

    std::uint32_t const spell = bot.last_action_spell;
    if (bot.last_action_status == BotActionStatus::kInvalidTarget)
    {
        // The cast had no valid target: that unit is avoided too, so the next cast goes to another one.
        auto const cast = state.CastTarget.find(spell);
        if (cast != state.CastTarget.end() && cast->second != 0)
            AvoidUnit(state.AvoidUntilMs, state.AvoidCount, cast->second, nowMs);
    }
    if (bot.last_action_status == BotActionStatus::kApplied)
    {
        // The target of the cast as sent: 0 for a spell on the bot itself (a self buff such as a seal).
        auto const cast = state.CastTarget.find(spell);
        if (cast != state.CastTarget.end())
        {
            if (DotDurationMs(spell) != 0)
                state.Dots[spell] = Dot{ cast->second, nowMs + DotDurationMs(spell) };

            if (spell == kSinisterStrike)
            {
                // A Sinister Strike on a new target starts the count again.
                state.ComboPoints = (state.ComboTarget == cast->second) ? std::min(state.ComboPoints + 1, kMaxComboPoints) : 1;
                state.ComboTarget = cast->second;
            }
        }
        if (spell == kEviscerate)
            state.ComboPoints = 0;
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

    // Parties (M8): the lowest-guid bots, in guid order, cut into parties of PartySize. Computed once per tick.
    std::unordered_map<std::uint64_t, std::size_t> indexOf; // bot guid -> index in view.bots
    for (std::size_t i = 0; i < view.bots.size(); ++i)
        indexOf[view.bots[i].guid] = i;

    std::unordered_map<std::uint64_t, std::uint64_t> leaderOf;                // bot guid -> its party's leader
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> membersOf;  // leader guid -> the other members
    if (_options.PartySize != 0)
    {
        std::vector<std::uint64_t> guids;
        guids.reserve(view.bots.size());
        for (BotSnapshot const& bot : view.bots)
            guids.push_back(bot.guid);
        std::sort(guids.begin(), guids.end());

        std::size_t const partied = std::min<std::size_t>(guids.size(), std::size_t(_options.PartySize) * _options.PartyCount);
        for (std::size_t start = 0; start < partied; start += _options.PartySize)
        {
            std::size_t const end = std::min<std::size_t>(start + _options.PartySize, partied);
            std::size_t leader = start;
            for (std::size_t p = start; p < end; ++p)
            {
                if (ClassFor(_options, guids[p]) == kWarriorClass)
                {
                    leader = p;
                    break;
                }
            }
            std::uint64_t const leaderGuid = guids[leader];
            for (std::size_t p = start; p < end; ++p)
            {
                leaderOf[guids[p]] = leaderGuid;
                if (p != leader)
                    membersOf[leaderGuid].push_back(guids[p]);
            }
        }
    }

    for (std::size_t i = 0; i < view.bots.size(); ++i)
    {
        BotSnapshot const& bot = view.bots[i];
        present.insert(bot.guid);
        BotState& state = _bots[bot.guid];
        ReadResult(state, bot, nowMs);
        for (auto it = state.AvoidUntilMs.begin(); it != state.AvoidUntilMs.end();)
            it = nowMs >= it->second ? state.AvoidUntilMs.erase(it) : std::next(it);

        // A dead bot gets no orders; its last decision stays in place until it is alive again.
        if (bot.health == 0)
            continue;

        std::span<NearbyUnit const> nearby = view.NearbyOf(i);
        NearbyUnit const* player = Closest(bot, nearby, IsPlayer);
        bool const lowHealth = bot.max_health > 0 && std::uint64_t(bot.health) * 100 < std::uint64_t(bot.max_health) * kDisengagePct;

        // The bot's class: its own (from the class table) or the configured one. Ranged classes approach to range.
        std::uint32_t const classId = ClassFor(_options, bot.guid);

        // The bot's party: a leader is the bot that invites, a member follows its leader. The leader's snapshot is the one
        // this tick's view holds, when the leader is online.
        auto const led = leaderOf.find(bot.guid);
        std::uint64_t const partyLeader = led == leaderOf.end() ? 0 : led->second;
        bool const partyLeaderBot = partyLeader != 0 && partyLeader == bot.guid;
        bool const partyMember = partyLeader != 0 && !partyLeaderBot;
        std::uint64_t const leaderRef = partyLeader != 0 ? PlayerRef(partyLeader) : 0;
        BotSnapshot const* leaderSnap = nullptr;
        if (partyMember)
        {
            if (auto const at = indexOf.find(partyLeader); at != indexOf.end())
                leaderSnap = &view.bots[at->second];
        }
        bool const atLeader = leaderSnap && Distance2d(bot, *leaderSnap) <= kFollowLeaderYards;

        // Who an idle bot follows: a member its leader (and stays put once close), else the closest player.
        std::uint64_t followRef = player ? player->guid : 0;
        if (partyMember && leaderSnap)
            followRef = atLeader ? 0 : leaderRef;

        // The target is the threat target (even when it is out of view), otherwise the closest live hostile. A member with
        // no threat of its own fights its leader's target, when that is in view.
        NearbyUnit const* targetUnit = nullptr;
        auto const avoided = [&state, nowMs](std::uint64_t ref)
        {
            auto const until = state.AvoidUntilMs.find(ref);
            return until != state.AvoidUntilMs.end() && nowMs < until->second;
        };
        std::uint64_t targetRef = bot.threat_target_guid;
        if (targetRef != 0 && avoided(targetRef))
            targetRef = 0; // it refused us: the bot does not fight it for now
        if (targetRef == 0 && partyMember && leaderSnap && leaderSnap->threat_target_guid != 0 &&
            !avoided(leaderSnap->threat_target_guid))
        {
            if (NearbyUnit const* assist = FindLiveMob(nearby, leaderSnap->threat_target_guid))
                targetRef = assist->guid;
        }
        if (targetRef != 0)
        {
            for (NearbyUnit const& unit : nearby)
                if (unit.guid == targetRef)
                    targetUnit = &unit;
        }
        else if (NearbyUnit const* hostile = PullTarget(bot, nearby, avoided))
        {
            targetUnit = hostile;
            targetRef = hostile->guid;
        }
        bool const hasTarget = targetRef != 0 && !lowHealth;
        // The target of this fight is looted once it is dead: remember it, and forget the ones too old to still be on the ground.
        if (hasTarget && forever_module::UnitRefType(targetRef) == 8)
            state.FoughtAtMs[targetRef] = nowMs;
        for (auto it = state.FoughtAtMs.begin(); it != state.FoughtAtMs.end();)
            it = nowMs - it->second > kLootWindowMs ? state.FoughtAtMs.erase(it) : std::next(it);
        for (auto it = state.LootedUntilMs.begin(); it != state.LootedUntilMs.end();)
            it = nowMs >= it->second ? state.LootedUntilMs.erase(it) : std::next(it);
        float const distance = targetUnit ? rotation::DistanceYards(bot, *targetUnit) : bot.target_distance;

        Decision wanted;
        if (lowHealth)
        {
            wanted = followRef ? Decision{ ActionType::kFollow, followRef } : Decision{ ActionType::kStop, 0 };
        }
        else if (targetRef != 0)
        {
            if (!state.InFight)
            {
                state.InFight = true;
                state.FightStartMs = nowMs;
            }
            // A ranged bot that has run out of power closes to melee range: its spells are spent.
            bool const outOfPower = bot.max_power > 0 && std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * kOutOfPowerPct;
            float const approach = outOfPower ? kMeleeApproachYards : rotation::ApproachYards(classId);
            if (distance <= approach)
            {
                state.Approaching = false;
                wanted = Decision{ ActionType::kAttackTarget, targetRef };
            }
            else
            {
                // Walking to a target that never gets in range (behind a wall, or unreachable) is given up after a while:
                // the target is avoided, and the bot picks another one next tick.
                if (!state.Approaching || state.ApproachTarget != targetRef)
                {
                    state.Approaching = true;
                    state.ApproachTarget = targetRef;
                    state.ApproachSinceMs = nowMs;
                    wanted = Decision{ ActionType::kFollow, targetRef };
                }
                else if (nowMs - state.ApproachSinceMs > kApproachTimeoutMs)
                {
                    state.Approaching = false;
                    AvoidUnit(state.AvoidUntilMs, state.AvoidCount, targetRef, nowMs);
                    wanted = Decision{ ActionType::kStop, 0 };
                }
                else
                {
                    wanted = Decision{ ActionType::kFollow, targetRef };
                }
            }
        }
        else
        {
            state.InFight = false;
            wanted = followRef ? Decision{ ActionType::kFollow, followRef } : Decision{ ActionType::kStop, 0 };
        }

        bool enemyNear = false;
        for (NearbyUnit const& unit : nearby)
            enemyNear = enemyNear || IsLiveMob(unit);
        bool idle = !hasTarget && !enemyNear && wanted.Type == ActionType::kStop;

        // Seeing nothing and doing nothing, for hubs (M7): the idle time starts when the bot first is idle, and any activity
        // resets it.
        if (!idle)
        {
            state.Idling = false;
            state.Wandering = false;
        }
        else if (!state.Idling)
        {
            state.Idling = true;
            state.IdleSinceMs = nowMs;
        }

        // Services and travel (M6). An idle bot due a vendor visit walks to the nearest vendor on its map, unless it is a
        // party member (it follows its leader). The visit is sent once the bot is in reach. A walk longer than
        // kTravelTimeoutMs is dropped and the visit waits for its next interval. A walking bot is not idle, so it neither
        // sits nor trains on the way.
        if (idle && _options.ServiceEveryMs != 0 && nowMs >= state.NextServiceMs)
        {
            // The nearest vendor on the bot's map, found by the core (layout 6). None on the map, or a party member: no walk.
            Point const vendor{ bot.vendor_x, bot.vendor_y, bot.vendor_z };
            bool const hasVendor = !partyMember && bot.vendor_valid != 0;
            if (hasVendor && DistanceTo(bot, vendor) > kVendorReachYards)
            {
                if (!state.Traveling)
                {
                    state.Traveling = true;
                    state.TravelStartMs = nowMs;
                }
                if (nowMs - state.TravelStartMs <= kTravelTimeoutMs)
                {
                    wanted = Decision{ ActionType::kMoveTo, 0, vendor };
                    idle = false;
                }
                else
                {
                    state.Traveling = false;
                    state.NextServiceMs = nowMs + _options.ServiceEveryMs;
                }
            }
            else
            {
                state.Traveling = false;
                BotAction vendor;
                vendor.bot_guid = bot.guid;
                vendor.type = ActionType::kVendor;
                vendor.spell_id = kServiceSell | kServiceRepair | kServiceEquip;
                actions.push_back(vendor);
                state.NextServiceMs = nowMs + _options.ServiceEveryMs;
            }
        }

        // Hubs (M7): a bot that has been idle for kWanderIdleMs walks to the next hub of its map, and waits there until the
        // next walk. A hurt bot rests instead, and a party member follows its leader. Anything that comes near stops the walk.
        bool const healthy = bot.max_health == 0 || std::uint64_t(bot.health) * 100 >= std::uint64_t(bot.max_health) * kRestBelowPct;
        auto const hubs = _options.HubsByMap.find(bot.map_id);
        if (idle && healthy && !partyMember && hubs != _options.HubsByMap.end() && !hubs->second.empty())
        {
            std::vector<Point> const& list = hubs->second;
            if (state.Wandering)
            {
                Point const& hub = list[state.NextHub % list.size()];
                bool const arrived = DistanceTo(bot, hub) <= kHubReachYards;
                bool const timedOut = nowMs - state.WanderStartMs > kWanderTimeoutMs;
                if (arrived || timedOut)
                {
                    // Next time, the next hub; the bot waits a full interval before it walks again.
                    state.Wandering = false;
                    state.NextHub = (state.NextHub + 1) % list.size();
                    state.IdleSinceMs = nowMs;
                }
                else
                {
                    wanted = Decision{ ActionType::kMoveTo, 0, hub };
                    idle = false;
                }
            }
            else if (nowMs - state.IdleSinceMs >= kWanderIdleMs)
            {
                state.Wandering = true;
                state.WanderStartMs = nowMs;
                wanted = Decision{ ActionType::kMoveTo, 0, list[state.NextHub % list.size()] };
                idle = false;
            }
        }

        // Rest: sit down while idle and hurt, and stand before anything moves or fights. The stand goes out before the
        // movement below, in the same action set, so the bot never moves sitting.
        // Idle means no fight and no player to follow, with no live hostile anywhere near: a sitting bot is easy prey.
        // Creatures only: a hostile player that is not attacking the bot does not stop it from resting (its threat target
        // already does, through hasTarget).
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

        // Party actions (M8), out of combat. The leader invites each member that is not in its party yet, and invites a
        // member again only after kInviteRetryMs. A member accepts its leader's invite, and leaves a party that is not its
        // leader's. Each is sent once; a refused one is retried after kPartyRetryMs.
        if (_options.PartySize != 0 && bot.in_combat == 0 && nowMs >= state.PartyRetryAtMs)
        {
            if (partyLeaderBot)
            {
                for (std::uint64_t member : membersOf[bot.guid])
                {
                    auto const at = indexOf.find(member);
                    if (at == indexOf.end())
                        continue; // not online
                    BotSnapshot const& other = view.bots[at->second];
                    if (other.group_leader_guid == leaderRef || other.group_invite_from == leaderRef)
                        continue; // in this party already, or invited and waiting for the answer
                    auto const invited = state.InvitedAtMs.find(member);
                    if (invited != state.InvitedAtMs.end() && nowMs < invited->second + kInviteRetryMs)
                        continue;

                    BotAction invite;
                    invite.bot_guid = bot.guid;
                    invite.type = ActionType::kGroup;
                    invite.spell_id = kGroupInvite;
                    invite.target_guid = PlayerRef(member);
                    actions.push_back(invite);
                    state.InvitedAtMs[member] = nowMs;
                }
            }
            else if (partyMember)
            {
                BotAction party;
                party.bot_guid = bot.guid;
                party.type = ActionType::kGroup;
                if (bot.group_role == 0 && bot.group_invite_from == leaderRef)
                    party.spell_id = kGroupAccept;
                else if (bot.group_role != 0 && bot.group_leader_guid != leaderRef)
                    party.spell_id = kGroupLeave;
                if (party.spell_id != 0)
                {
                    actions.push_back(party);
                    state.PartyRetryAtMs = nowMs + kPartyRetryMs;
                }
            }
        }

        // Loot (M7): a creature the bot fought, now dead and close, once the bot is out of combat. One loot per corpse; the core
        // decides whether the bot may take anything from it.
        if (bot.in_combat == 0 && !lowHealth)
        {
            for (NearbyUnit const& unit : nearby)
            {
                if (unit.health != 0 || forever_module::UnitRefType(unit.guid) != 8 || state.LootedUntilMs.count(unit.guid))
                    continue;
                if (!state.FoughtAtMs.count(unit.guid) || rotation::DistanceYards(bot, unit) > kLootRangeYards)
                    continue;

                BotAction loot;
                loot.bot_guid = bot.guid;
                loot.type = ActionType::kLoot;
                loot.target_guid = unit.guid;
                actions.push_back(loot);
                state.LootedUntilMs[unit.guid] = nowMs + kLootWindowMs;
                state.FoughtAtMs.erase(unit.guid);
                break; // one corpse per tick
            }
        }

        if (!state.Last || state.Last->Type != wanted.Type || state.Last->Target != wanted.Target ||
            state.Last->Dest.X != wanted.Dest.X || state.Last->Dest.Y != wanted.Dest.Y || state.Last->Dest.Z != wanted.Dest.Z)
        {
            BotAction action;
            action.bot_guid = bot.guid;
            action.type = wanted.Type;
            action.target_guid = wanted.Target;
            action.dest_x = wanted.Dest.X;
            action.dest_y = wanted.Dest.Y;
            action.dest_z = wanted.Dest.Z;
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
                state.CastTarget[_options.TestSpell] = targetRef;
                state.LastTestCastTick = view.tick_number;
            }
            continue;
        }

        // Class rotation: the first row that is off cooldown, in range and allowed by its rule. One cast per global cooldown.
        if (nowMs < state.NextCastMs)
            continue;
        rotation::Facts facts = rotation::FactsFor(bot, nearby, targetUnit, state.InFight ? nowMs - state.FightStartMs : 0);
        if (state.ComboTarget != targetRef)
        {
            state.ComboTarget = targetRef;
            state.ComboPoints = 0;
        }
        facts.ComboPoints = int32_t(state.ComboPoints);

        // Spells the core reports as cooling down right now.
        std::unordered_set<std::uint32_t> cooling;
        for (forever_module::SpellCooldown const& cooldown : bot.spell_cooldowns)
            if (cooldown.spell_id != 0 && cooldown.remaining_ms != 0)
                cooling.insert(cooldown.spell_id);

        for (rotation::Row const& row : rotation::RowsForClass(classId))
        {
            // Not learned yet: below the level a trainer teaches it. (A level of 0 is unknown and not judged.)
            if (bot.level != 0 && rotation::LearnLevel(row.Spell) > bot.level)
                continue;
            auto const retry = state.RetryAtMs.find(row.Spell);
            if (retry != state.RetryAtMs.end() && nowMs < retry->second)
                continue;
            if (cooling.count(row.Spell))
                continue;
            if (row.OnTarget && (distance < row.MinRange || (row.MaxRange > 0.0f && distance > row.MaxRange)))
                continue;
            if (!rotation::Allowed(row.When, facts))
                continue;
            // The effect is still up (on the target, or on the bot for a self buff): recasting only refreshes it.
            auto const dot = state.Dots.find(row.Spell);
            bool const sameTarget = dot != state.Dots.end() && dot->second.Target == (row.OnTarget ? targetRef : 0);
            if (sameTarget && nowMs < dot->second.UntilMs)
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
