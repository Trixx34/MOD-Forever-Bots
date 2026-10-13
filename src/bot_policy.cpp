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
constexpr std::uint64_t kBusyRetryMs = 300;  // a cast refused because the bot was casting or moving: tried again very soon
constexpr std::uint64_t kNotKnownRetryMs = 60000; // the spell may be learned later
constexpr float kPullRadius = 10.0f;         // yards: hostiles this close to a candidate are pulled with it
constexpr std::size_t kMaxDungeonPull = 4;  // a dungeon pack is larger than an outdoor one; the party pulls up to this many together
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
constexpr std::uint32_t kRepairBelowPct = 50;    // worn gear below this durability sends the bot to a repairman, not just any vendor
constexpr float kRoutePointReachYards = 6.0f;    // the leader has arrived at a route waypoint this close to it
constexpr float kRouteWaitYards = 30.0f;         // the leader waits for a member farther behind than this
constexpr std::uint32_t kRouteMinHealthPct = 80; // and for its own health to be at least this
// Dungeon pull control: the tank (the party leader) pulls only when the party is ready, and the others wait for the tank to hold the target.
constexpr std::uint32_t kPartyReadyHealthPct = 80;  // every living member has at least this much health
constexpr std::uint32_t kPartyReadyPowerPct = 60;   // and every mana user at least this much mana
constexpr std::uint32_t kPartyReadySitHealthPct = 95; // a sitting member rests up to this much
constexpr std::uint64_t kPullHoldMaxMs = 120000;    // the party never waits longer than this for itself
constexpr std::uint64_t kTankHeadStartMs = 2500;    // the others join a fight this long after the tank started it, so the tank has the threat
// Healing: a healer class heals the party member with the lowest health below this share, within this range.
constexpr std::uint32_t kHealBelowPct = 60;
constexpr float kHealRangeYards = 30.0f;
constexpr float kBuffRangeYards = 30.0f;            // a party buff reaches members this close
constexpr std::size_t kGrindMemory = 2;           // grind spots remembered as just visited
constexpr std::uint64_t kAcceptInviteRetryMs = 3000; // an invite from a player is answered again after this long if it is still pending
constexpr std::uint64_t kGotoTimeoutMs = 90000;     // a goto order is dropped after this long
constexpr std::uint64_t kPullOrderMs = 30000;      // a pull order is dropped after this long, or when the target is gone
constexpr std::uint64_t kCloserAfterLosMs = 5000;   // after a line-of-sight refusal the bot closes to melee range for this long
constexpr std::uint64_t kTravelTimeoutMs = 180000; // a walk to the vendor longer than this is dropped for now
// Death and recovery. Spell ids and levels checked against the fork's SpellLevels.db2: Resurrection 2006 level 10,
// Redemption 7328 level 12, Ancestral Spirit 2008 level 12, Rebirth 20484 level 20.
constexpr std::uint32_t kResurrection = 2006;
constexpr std::uint32_t kRedemption = 7328;
constexpr std::uint32_t kAncestralSpirit = 2008;
constexpr float kRezRangeYards = 30.0f;              // a rezzer resurrects a corpse within this range
constexpr std::uint32_t kRezManaPct = 35;            // and starts the 10 s cast with at least this much mana
constexpr std::uint64_t kRezCastMs = 11000;          // after a resurrect cast started, the same target is left alone this long
constexpr std::uint64_t kRezRetryMs = 5000;          // after a refusal, this long
constexpr std::uint32_t kRezRefusalsLogged = 3;      // refusals in a row before rez_refused is logged
constexpr std::uint64_t kReleaseRetryMs = 3000;      // a release not yet seen to work is not sent again before this
constexpr std::uint64_t kAcceptRetryMs = 2000;
constexpr std::uint64_t kReclaimRetryMs = 3000;
constexpr float kReclaimYards = 35.0f;               // the core allows 39 yards; the ghost reclaims from this close

// Spells with a cast time (or a channel), which cannot be started while moving. The core's refusals add to the bot's own list, so a
// spell missing here is learned after its first refusal.
bool KnownCastTimeSpell(std::uint32_t spell)
{
    switch (spell)
    {
        case 686: case 585: case 133: case 116: case 5176: case 5185: case 403: case 331: case 2050: case 2054: case 2060: case 635:
        case 8092: case 348: case 707: case 172: case 1120: case 689: case 5676: case 5143: case 8004: case 421: case 1064: case 14914:
        case 15407: case 2948: case 5019: case 8921: case 19750: case 2006: case 7328: case 2008:
            return true;
        default:
            return false;
    }
}

// The spell a healer class heals an ally with (rank 1 id, or the next spell once the bot has the level), 0 for a class that does not heal.
std::uint32_t HealSpellFor(std::uint32_t classId, std::uint32_t level)
{
    switch (classId)
    {
        case 5: return level >= 16 ? 2054 : 2050; // priest: Heal, or Lesser Heal
        case 2: return 635;                       // paladin: Holy Light
        case 7: return 331;                       // shaman: Healing Wave
        case 11: return 5185;                     // druid: Healing Touch
        default: return 0;
    }
}

// The spell a class resurrects a dead ally with, 0 for a class that cannot or whose level is too low. A druid's Rebirth (20484, level 20)
// needs a reagent the bots are not given, so a druid is not a rezzer. A level of 0 is unknown and not judged.
std::uint32_t ResurrectSpellFor(std::uint32_t classId, std::uint32_t level)
{
    switch (classId)
    {
        case 5: return level == 0 || level >= 10 ? kResurrection : 0;    // priest
        case 2: return level == 0 || level >= 12 ? kRedemption : 0;      // paladin
        case 7: return level == 0 || level >= 12 ? kAncestralSpirit : 0; // shaman
        default: return 0;
    }
}

bool IsResurrectSpell(std::uint32_t spell)
{
    return spell == kResurrection || spell == kRedemption || spell == kAncestralSpirit;
}

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
                            std::function<bool(std::uint64_t)> const& avoided, bool defending,
                            std::vector<std::uint32_t> const& onlyEntries = {}, bool dungeon = false)
{
    NearbyUnit const* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    for (NearbyUnit const& candidate : units)
    {
        if (!IsLiveMob(candidate) || avoided(candidate.guid))
            continue;
        // A bot hunting for a quest pulls only the creatures the quest wants (unless it is already being attacked).
        if (!defending && !onlyEntries.empty() &&
            std::find(onlyEntries.begin(), onlyEntries.end(), forever_module::UnitRefEntry(candidate.guid)) == onlyEntries.end())
            continue;
        // Not worth pulling: well above the bot's level, or an elite. (A level of 0 is unknown and not judged.) A bot that is already
        // in combat is being attacked, and the snapshot does not say by what: it fights the closest creature, whatever its level, since
        // standing still until it dies is worse. The same goes for the pack-size rule below.
        // In a dungeon (the route is active) elites, bosses and higher levels are the content: the party fights what it meets.
        if (!defending && !dungeon && bot.level != 0 &&
            ((candidate.level != 0 && candidate.level > bot.level + kMaxLevelAbove) || candidate.elite))
            continue;
        float const dist = rotation::DistanceYards(bot, candidate);
        if (dist >= bestDist)
            continue;

        std::size_t group = 0;
        for (NearbyUnit const& other : units)
            if (IsLiveMob(other) && DistanceBetween(candidate, other) <= kPullRadius)
                ++group;
        if (!defending && group > (dungeon ? kMaxDungeonPull : kMaxPull))
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
        case BotActionStatus::kBusy: return "busy casting";
        case BotActionStatus::kMoving: return "moving";
    }
    return "unknown";
}

// Reads the result of the bot's last action, once per sequence number, and sets when the spell may be tried again.
std::span<rotation::Row const> BotPolicy::RowsFor(std::uint32_t classId) const
{
    if (_options.Rotations)
        if (auto const at = _options.Rotations->ByClass.find(classId); at != _options.Rotations->ByClass.end())
            return at->second;
    return rotation::RowsForClass(classId);
}

std::uint64_t BotPolicy::DurationMsOf(std::uint32_t classId, std::uint32_t spell) const
{
    for (rotation::Row const& row : RowsFor(classId))
        if (row.Spell == spell && row.DurationSec != 0)
            return std::uint64_t(row.DurationSec) * 1000;
    return DotDurationMs(spell);
}

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

    // Auto gear refused (in combat, switched off in the core): tried again after a while.
    if (bot.last_action_type == uint8_t(ActionType::kAutoGear) && bot.last_action_status == BotActionStatus::kRefused)
    {
        state.GearedAtLevel = 0;
        state.GearRetryMs = nowMs + 60000;
        return;
    }

    // A quest action refused (not eligible, bags full, no NPC in reach): the quest is left alone for a while.
    if (bot.last_action_type == uint8_t(ActionType::kQuest) && bot.last_action_status == BotActionStatus::kRefused)
    {
        _quests.NoteRefused(state.Quest, nowMs);
        return;
    }

    // A party action refused (no invite waiting, a full party): the party waits a moment before the next one.
    if (bot.last_action_type == uint8_t(ActionType::kGroup) && bot.last_action_status == BotActionStatus::kRefused)
    {
        state.PartyRetryAtMs = nowMs + kPartyRetryMs;
        return;
    }

    // The death actions: a refusal is logged, and the action waits before it is sent again.
    if (bot.last_action_type == uint8_t(ActionType::kReleaseSpirit) || bot.last_action_type == uint8_t(ActionType::kResurrect) ||
        bot.last_action_type == uint8_t(ActionType::kReclaimCorpse))
    {
        bool const ok = bot.last_action_status == BotActionStatus::kApplied;
        char const* const what = bot.last_action_type == uint8_t(ActionType::kReleaseSpirit) ? "release"
                                 : bot.last_action_type == uint8_t(ActionType::kResurrect) ? "resurrect_accept" : "reclaim";
        if (!ok && _options.OnEvent)
            _options.OnEvent(std::string("death_action_refused: bot=") + std::to_string(bot.guid) + " action=" + what +
                             " status=" + ActionStatusName(bot.last_action_status));
        if (ok && bot.last_action_type == uint8_t(ActionType::kReclaimCorpse))
            state.Reclaimed = true;
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
    if (IsResurrectSpell(spell))
    {
        // A resurrect: the dead member is left alone while the cast runs, or after a refusal for a while; a run of refusals is logged.
        auto const cast = state.CastTarget.find(spell);
        std::uint64_t const target = cast != state.CastTarget.end() ? cast->second : 0;
        if (bot.last_action_status == BotActionStatus::kApplied)
        {
            state.RezRetryAtMs[target] = nowMs + kRezCastMs;
            state.RezRefusals[target] = 0;
        }
        else
        {
            state.RezRetryAtMs[target] = nowMs + kRezRetryMs;
            if (++state.RezRefusals[target] == kRezRefusalsLogged && _options.OnEvent)
                _options.OnEvent("rez_refused: rezzer=" + std::to_string(bot.guid) + " spell=" + std::to_string(spell) + " target=" +
                                 std::to_string(target) + " status=" + ActionStatusName(bot.last_action_status));
        }
        return;
    }
    if (bot.last_action_status == BotActionStatus::kInvalidTarget)
    {
        // The cast had no valid target: that unit is avoided too, so the next cast goes to another one.
        auto const cast = state.CastTarget.find(spell);
        if (cast != state.CastTarget.end() && cast->second != 0)
            AvoidUnit(state.AvoidUntilMs, state.AvoidCount, cast->second, nowMs);
    }
    if (bot.last_action_status == BotActionStatus::kNoLineOfSight)
        state.CloserUntilMs = nowMs + kCloserAfterLosMs; // walls between them: close in rather than cast through them
    if (bot.last_action_status == BotActionStatus::kApplied)
    {
        // The target of the cast as sent: 0 for a spell on the bot itself (a self buff such as a seal).
        auto const cast = state.CastTarget.find(spell);
        if (cast != state.CastTarget.end())
        {
            std::uint64_t const duration = DurationMsOf(ClassFor(_options, bot.guid), spell);
            if (duration != 0)
            {
                state.Dots[spell] = Dot{ cast->second, nowMs + duration };
                state.BuffUntilMs[{ spell, cast->second }] = nowMs + duration;
            }

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
        case BotActionStatus::kBusy:
            state.RetryAtMs[spell] = nowMs + kBusyRetryMs; // another cast is in progress: soon, not after the long retry
            state.NextCastMs = std::max(state.NextCastMs, nowMs + kBusyRetryMs * 2); // and no other spell is tried meanwhile
            break;
        case BotActionStatus::kMoving:
            state.CastTimeSpells.insert(spell);            // remembered: the bot stops before this spell from now on
            state.RetryAtMs[spell] = nowMs + kBusyRetryMs;
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

std::vector<std::string> BotPolicy::ApplyChat(WorldTickSnapshotView const& view, ChatContext const& chat, ChatCommand const& command,
                                               std::uint64_t nowMs)
{
    std::vector<std::string> lines;
    std::uint64_t const issuerRef = PlayerRef(chat.Issuer);
    std::uint32_t matched = 0, refused = 0, index = 0;
    char const* refusal = "";
    for (BotSnapshot const& bot : view.bots)
    {
        // Only the bots of the speaker's own group obey, and a whisper reaches only the bot it names.
        if (bot.group_leader_guid != issuerRef || (chat.Recipient != 0 && bot.guid != chat.Recipient))
            continue;
        std::uint32_t const classId = ClassFor(_options, bot.guid);
        if (!command.Selector.Matches(classId))
            continue;
        ++matched;
        BotState& state = _bots[bot.guid];
        auto const refuse = [&](char const* why)
        {
            ++refused;
            refusal = why;
        };
        switch (command.Verb)
        {
            case ChatVerb::Follow:
                state.ChatStay = false;
                state.ChatFollow = issuerRef;
                state.GotoActive = false;
                state.Last.reset();
                break;
            case ChatVerb::FollowOff:
                state.ChatFollow = 0;
                state.Last.reset();
                break;
            case ChatVerb::Stay:
                state.ChatStay = true;
                state.ChatFollow = 0;
                state.GotoActive = false;
                state.Last.reset();
                break;
            case ChatVerb::StayOff:
                state.ChatStay = false;
                state.Last.reset();
                break;
            case ChatVerb::Stop:
                state.ChatStay = false;
                state.ChatFollow = 0;
                state.GotoActive = false;
                state.ForcedTarget = 0;
                state.ForcedHeal = 0;
                state.Last.reset();
                break;
            case ChatVerb::GotoHere:
            {
                if (bot.map_id != chat.MapId)
                {
                    refuse("on another map");
                    break;
                }
                float dx = 0.0f, dy = 0.0f;
                ChatSpreadOffset(index, dx, dy);
                state.GotoPoint = Point{ chat.Pos.X + dx, chat.Pos.Y + dy, chat.Pos.Z };
                state.GotoActive = true;
                state.GotoUntilMs = nowMs + kGotoTimeoutMs;
                state.ChatFollow = 0;
                state.ChatStay = true; // holds the spot once there
                state.Last.reset();
                break;
            }
            case ChatVerb::Release:
                if (bot.life_state != 1)
                    refuse("not dead (or already released)");
                else
                    state.ReleaseNow = true;
                break;
            case ChatVerb::Status:
            {
                std::string line = "Bot " + std::to_string(bot.guid) + " (class " + std::to_string(classId) + "): ";
                line += state.SeenLife == 1 ? "dead" : state.SeenLife == 2 ? "ghost" : state.SeenInCombat ? "fighting" : "idle";
                line += ", hp " + std::to_string(state.SeenHealthPct) + "%";
                if (bot.max_power > 0)
                    line += ", power " + std::to_string(state.SeenPowerPct) + "%";
                if (state.ChatStay) line += ", stay";
                if (state.ChatFollow != 0) line += ", following you";
                if (state.Stance_ == BotState::Stance::Passive) line += ", passive";
                if (state.Stance_ == BotState::Stance::Defensive) line += ", defensive";
                if (state.Role_ != BotState::Role::Auto)
                    line += state.Role_ == BotState::Role::Tank ? ", role tank" : state.Role_ == BotState::Role::Healer ? ", role healer" : ", role dps";
                if (state.Focus != 0) line += ", focus set";
                if (state.FollowYards > 0.0f) line += ", follow " + std::to_string(int(state.FollowYards)) + "y";
                if (state.GotoActive) line += ", walking to you";
                lines.push_back(std::move(line));
                break;
            }
            case ChatVerb::Pull:
                if (classId != kWarriorClass && state.Role_ != BotState::Role::Tank)
                    refuse("not a tank");
                else if (chat.TargetRef == 0 || forever_module::UnitRefType(chat.TargetRef) != 8)
                    refuse("select a creature first");
                else
                {
                    state.ForcedTarget = chat.TargetRef;
                    state.ForcedUntilMs = nowMs + kPullOrderMs;
                    state.ChatStay = false;
                    state.AvoidUntilMs.erase(chat.TargetRef);
                }
                break;
            case ChatVerb::Heal:
                if (HealSpellFor(classId, bot.level) == 0)
                    refuse("not a healer");
                else if (chat.TargetRef == 0 || forever_module::UnitRefType(chat.TargetRef) != kPlayerType)
                    refuse("select a friend first");
                else
                    state.ForcedHeal = chat.TargetRef;
                break;
            case ChatVerb::Passive:
                state.Stance_ = BotState::Stance::Passive;
                break;
            case ChatVerb::Aggressive:
                state.Stance_ = BotState::Stance::Aggressive;
                break;
            case ChatVerb::Stance:
                state.Stance_ = command.Arg == "passive" ? BotState::Stance::Passive
                                : command.Arg == "defensive" ? BotState::Stance::Defensive : BotState::Stance::Aggressive;
                break;
            case ChatVerb::Role:
                state.Role_ = command.Arg == "tank" ? BotState::Role::Tank : command.Arg == "healer" ? BotState::Role::Healer
                              : command.Arg == "dps" ? BotState::Role::Dps : BotState::Role::Auto;
                break;
            case ChatVerb::Focus:
                if (chat.TargetRef == 0 || forever_module::UnitRefType(chat.TargetRef) != 8)
                    refuse("select a creature first");
                else
                    state.Focus = chat.TargetRef;
                break;
            case ChatVerb::FocusOff:
                state.Focus = 0;
                break;
            case ChatVerb::Distance:
                state.FollowYards = command.Arg == "default" ? 0.0f : command.Value;
                state.FollowWalking = false;
                break;
            case ChatVerb::Summon:
                if (bot.life_state != 0)
                    refuse("dead");
                else
                {
                    forever_module::BotAction summon;
                    summon.bot_guid = bot.guid;
                    summon.type = ActionType::kSummon;
                    summon.target_guid = issuerRef;
                    summon.dest_x = float(index);
                    state.OrderAction = summon;
                    state.GotoActive = false;
                    state.Last.reset();
                }
                break;
            case ChatVerb::Revive:
                if (bot.life_state == 0)
                    refuse("not dead");
                else
                {
                    forever_module::BotAction revive;
                    revive.bot_guid = bot.guid;
                    revive.type = ActionType::kRevive;
                    revive.target_guid = issuerRef;
                    revive.dest_x = float(index);
                    state.OrderAction = revive;
                    state.ReleaseNow = false;
                }
                break;
            case ChatVerb::Mount:
            case ChatVerb::Dismount:
                if (bot.life_state != 0)
                    refuse("dead");
                else
                {
                    forever_module::BotAction mount;
                    mount.bot_guid = bot.guid;
                    mount.type = ActionType::kMount;
                    mount.spell_id = command.Verb == ChatVerb::Mount ? 1u : 0u;
                    state.OrderAction = mount;
                }
                break;
            case ChatVerb::None:
                break;
        }
        ++index;
    }
    if (matched == 0)
        lines.push_back("No bot of your group matched.");
    else if (refused != 0)
        lines.push_back(std::string(ChatVerbName(command.Verb)) + ": " + std::to_string(refused) + " of " + std::to_string(matched) + " bot(s) refused (" + refusal + ").");
    return lines;
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
                auto const known = _bots.find(guids[p]);
                bool const tankRole = known != _bots.end() && known->second.Role_ == BotState::Role::Tank;
                if (tankRole || (ClassFor(_options, guids[p]) == kWarriorClass && (known == _bots.end() || known->second.Role_ == BotState::Role::Auto)))
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

    // A leader that has a target has begun a fight: note it before the members decide, so they see the same start whatever the bot order.
    for (auto const& entry : membersOf)
    {
        auto const at = indexOf.find(entry.first);
        if (at == indexOf.end())
            continue;
        BotSnapshot const& leader = view.bots[at->second];
        BotState& leaderState = _bots[leader.guid];
        if (leader.life_state == 0 && leader.threat_target_guid != 0 && !leaderState.InFight)
        {
            leaderState.InFight = true;
            leaderState.FightStartMs = nowMs;
        }
    }

    // The party of a leader: the leader and its members that are online.
    auto const forParty = [&](std::uint64_t leaderGuid, auto&& fn)
    {
        if (auto const at = indexOf.find(leaderGuid); at != indexOf.end())
            fn(view.bots[at->second]);
        auto const members = membersOf.find(leaderGuid);
        if (members == membersOf.end())
            return;
        for (std::uint64_t member : members->second)
            if (auto const at = indexOf.find(member); at != indexOf.end())
                fn(view.bots[at->second]);
    };
    auto const isRezzer = [&](BotSnapshot const& member)
    {
        return member.life_state == 0 && ResurrectSpellFor(ClassFor(_options, member.guid), member.level) != 0;
    };
    auto const partyFighting = [&](std::uint64_t leaderGuid)
    {
        bool fighting = false;
        forParty(leaderGuid, [&](BotSnapshot const& member) { fighting = fighting || (member.life_state == 0 && member.in_combat != 0); });
        return fighting;
    };
    // Whether the party waits for a dead member: 0 no, 1 for a resurrect, 2 for a ghost on its way back. Never in a fight, never
    // for a ghost whose run failed, and every wait is capped.
    auto const deadWait = [&](std::uint64_t leaderGuid) -> std::uint8_t
    {
        if (partyFighting(leaderGuid))
            return 0;
        bool rezzer = false;
        forParty(leaderGuid, [&](BotSnapshot const& member) { rezzer = rezzer || isRezzer(member); });
        std::uint8_t kind = 0;
        forParty(leaderGuid, [&](BotSnapshot const& member)
        {
            if (member.life_state == 0)
                return;
            static BotState const unseen; // a member the policy has not met yet counts as just down
            auto const known = _bots.find(member.guid);
            BotState const& theirs = known == _bots.end() ? unseen : known->second;
            if (member.life_state == 1)
            {
                if (rezzer && (theirs.Life != 1 || nowMs - theirs.DeathMs < _options.WaitForRezMs))
                    kind = std::max<std::uint8_t>(kind, 1);
            }
            else if (!theirs.RunFailed)
            {
                bool const within = theirs.Inside ? nowMs - theirs.InsideSinceMs < _options.WaitForGhostMs
                                    : !theirs.RunStarted || nowMs - theirs.RunStartMs < _options.CorpseRunTimeoutMs;
                if (within)
                    kind = 2;
            }
        });
        return kind;
    };

    for (std::size_t i = 0; i < view.bots.size(); ++i)
    {
        BotSnapshot const& bot = view.bots[i];
        present.insert(bot.guid);
        BotState& state = _bots[bot.guid];
        ReadResult(state, bot, nowMs);
        if (state.OrderAction)
        {
            actions.push_back(*state.OrderAction);
            state.OrderAction.reset();
        }
        state.SeenHealthPct = bot.max_health ? std::uint32_t(std::uint64_t(bot.health) * 100 / bot.max_health) : 100;
        state.SeenPowerPct = bot.max_power ? std::uint32_t(std::uint64_t(bot.power) * 100 / bot.max_power) : 100;
        state.SeenLife = bot.life_state;
        state.SeenInCombat = bot.in_combat != 0;
        for (auto it = state.AvoidUntilMs.begin(); it != state.AvoidUntilMs.end();)
            it = nowMs >= it->second ? state.AvoidUntilMs.erase(it) : std::next(it);

        std::span<NearbyUnit const> nearby = view.NearbyOf(i);
        auto const ledBy = leaderOf.find(bot.guid);
        std::uint64_t const deadParty = ledBy == leaderOf.end() ? 0 : ledBy->second;

        // A dead bot (a corpse, or a ghost: it has health 1, so only life_state tells) gets its own orders and nothing else. State
        // changes are logged once.
        if (bot.life_state != 0)
        {
            std::uint8_t const was = state.Life;
            auto const event = [&](std::string const& text)
            {
                if (_options.OnEvent)
                    _options.OnEvent(text);
            };
            auto const emit = [&](Decision const& wantedDead)
            {
                if (state.Last && state.Last->Type == wantedDead.Type && state.Last->Target == wantedDead.Target &&
                    state.Last->Dest.X == wantedDead.Dest.X && state.Last->Dest.Y == wantedDead.Dest.Y && state.Last->Dest.Z == wantedDead.Dest.Z)
                    return;
                BotAction action;
                action.bot_guid = bot.guid;
                action.type = wantedDead.Type;
                action.dest_x = wantedDead.Dest.X;
                action.dest_y = wantedDead.Dest.Y;
                action.dest_z = wantedDead.Dest.Z;
                actions.push_back(action);
                state.Last = wantedDead;
            };
            if (was == 0)
            {
                // Just died: the living state is dropped, and how it happened is logged (the killer is the threat target, else the
                // closest live hostile).
                state.DeathMs = nowMs;
                state.ReleaseNow = false;
                state.ReleaseSent = false;
                state.ReleaseHeldLogged = false;
                state.AcceptRetryMs = 0;
                state.AcceptedFrom = 0;
                state.ReclaimRetryMs = 0;
                state.Reclaimed = false;
                state.RunStarted = false;
                state.Inside = false;
                state.RunFailed = false;
                state.RunStartDist = 0.0f;
                state.RunProgress = 0;
                state.InFight = false;
                state.Sitting = false;
                state.Approaching = false;
                state.Traveling = false;
                state.Wandering = false;
                state.Idling = false;
                NearbyUnit const* killer = nullptr;
                for (NearbyUnit const& unit : nearby)
                    if (unit.guid == bot.threat_target_guid)
                        killer = &unit;
                if (!killer)
                    killer = Closest(bot, nearby, IsLiveMob);
                event("died: bot=" + std::to_string(bot.guid) + " life=" + std::to_string(bot.life_state) + " map=" + std::to_string(bot.map_id) +
                      " last_decision=" + (state.Last ? std::to_string(int(state.Last->Type)) : std::string("none")) +
                      " killer_entry=" + (killer ? std::to_string(forever_module::UnitRefEntry(killer->guid)) : std::string("none")) +
                      " in_combat=" + std::to_string(bot.in_combat));
            }
            if (bot.life_state == 2 && was != 2)
            {
                state.RunStarted = false;
                state.Inside = false;
                state.Last.reset();
                event("ghost: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " corpse_map=" + std::to_string(bot.corpse_map) +
                      " corpse_valid=" + std::to_string(bot.corpse_valid) + " reclaim_wait_ms=" + std::to_string(bot.reclaim_wait_ms));
            }
            state.Life = bot.life_state;

            // A resurrect offer is accepted at any time, whatever the bot was doing.
            if (bot.resurrect_from != 0)
            {
                if (nowMs >= state.AcceptRetryMs)
                {
                    BotAction accept;
                    accept.bot_guid = bot.guid;
                    accept.type = ActionType::kResurrect;
                    accept.target_guid = bot.resurrect_from;
                    accept.spell_id = 0;
                    actions.push_back(accept);
                    state.AcceptRetryMs = nowMs + kAcceptRetryMs;
                    if (state.AcceptedFrom != bot.resurrect_from)
                    {
                        state.AcceptedFrom = bot.resurrect_from;
                        event("resurrect_accept: bot=" + std::to_string(bot.guid) + " from=" + std::to_string(bot.resurrect_from) +
                              " life=" + std::to_string(bot.life_state) + " reason=offer_pending");
                    }
                }
                continue;
            }

            if (bot.life_state == 1)
            {
                if (state.ReleaseSent && nowMs < state.ReleaseSentMs + kReleaseRetryMs)
                    continue;
                std::uint64_t const waited = nowMs - state.DeathMs;
                bool anyAlive = false;
                bool rezzer = false;
                bool fighting = bot.in_combat != 0;
                if (deadParty != 0)
                    forParty(deadParty, [&](BotSnapshot const& member)
                    {
                        if (member.guid == bot.guid || member.life_state != 0)
                            return;
                        anyAlive = true;
                        rezzer = rezzer || isRezzer(member);
                        fighting = fighting || member.in_combat != 0;
                    });
                char const* reason = nullptr;
                if (!anyAlive)
                    reason = "release_all_down";
                else if (!rezzer)
                    reason = "release_no_rezzer";
                else if (waited >= _options.ReleaseDelayMs)
                    reason = "release_timeout";
                if (!reason && state.ReleaseNow)
                    reason = "release_ordered";
                if (!reason)
                    continue; // waiting for a resurrect
                if (fighting && waited < _options.ReleaseCombatHoldMs)
                {
                    // The core refuses a release in combat, and the party is still fighting: held back, capped.
                    if (!state.ReleaseHeldLogged)
                    {
                        state.ReleaseHeldLogged = true;
                        event("release_held: bot=" + std::to_string(bot.guid) + " reason=combat would_release=" + reason);
                    }
                    continue;
                }
                BotAction release;
                release.bot_guid = bot.guid;
                release.type = ActionType::kReleaseSpirit;
                actions.push_back(release);
                state.ReleaseSent = true;
                state.ReleaseSentMs = nowMs;
                event(std::string("release: bot=") + std::to_string(bot.guid) + " reason=" + reason + " waited_ms=" + std::to_string(waited) +
                      " rezzer_up=" + (rezzer ? "yes" : "no") + " alive_members=" + (anyAlive ? "yes" : "no"));
                continue;
            }

            // A ghost: back to the corpse. A corpse on another map (an instance, a ghost is released outside) is reached through the portal.
            if (state.RunFailed)
                continue;
            auto const fail = [&](char const* why)
            {
                state.RunFailed = true;
                emit(Decision{ ActionType::kStop, 0 });
                event(std::string("corpse_run_failed: bot=") + std::to_string(bot.guid) + " reason=" + why + " map=" + std::to_string(bot.map_id) +
                      " corpse_map=" + std::to_string(bot.corpse_map) + " run_ms=" + std::to_string(state.RunStarted ? nowMs - state.RunStartMs : 0));
            };
            if (!bot.corpse_valid)
            {
                fail("no_corpse");
                continue;
            }
            if (!state.RunStarted)
            {
                state.RunStarted = true;
                state.RunStartMs = nowMs;
                event("corpse_run_start: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " corpse_map=" +
                      std::to_string(bot.corpse_map) + " dist=" + (bot.map_id == bot.corpse_map ? std::to_string(int(DistanceTo(bot, Point{ bot.corpse_x, bot.corpse_y, bot.corpse_z }))) : std::string("other_map")));
            }
            if (nowMs - state.RunStartMs > _options.CorpseRunTimeoutMs)
            {
                fail("timeout");
                continue;
            }
            Point const corpse{ bot.corpse_x, bot.corpse_y, bot.corpse_z };
            if (bot.map_id == bot.corpse_map)
            {
                float const dist = DistanceTo(bot, corpse);
                if (!state.Inside)
                {
                    state.Inside = true;
                    state.InsideSinceMs = nowMs;
                    state.RunStartDist = std::max(dist, 1.0f);
                    event("corpse_map_reached: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " dist=" + std::to_string(int(dist)));
                }
                std::uint32_t const done = dist >= state.RunStartDist ? 0u : std::uint32_t((1.0f - dist / state.RunStartDist) * 100.0f);
                for (std::uint32_t mark : { 25u, 50u, 75u })
                    if (done >= mark && state.RunProgress < mark)
                    {
                        state.RunProgress = mark;
                        event("corpse_run_progress: bot=" + std::to_string(bot.guid) + " pct=" + std::to_string(mark) + " dist=" + std::to_string(int(dist)));
                    }
                if (dist <= kReclaimYards)
                {
                    emit(Decision{ ActionType::kStop, 0 });
                    if (bot.reclaim_wait_ms == 0 && nowMs >= state.ReclaimRetryMs)
                    {
                        BotAction reclaim;
                        reclaim.bot_guid = bot.guid;
                        reclaim.type = ActionType::kReclaimCorpse;
                        actions.push_back(reclaim);
                        state.ReclaimRetryMs = nowMs + kReclaimRetryMs;
                        event("reclaim: bot=" + std::to_string(bot.guid) + " dist=" + std::to_string(int(dist)) + " run_ms=" + std::to_string(nowMs - state.RunStartMs));
                    }
                }
                else
                    emit(Decision{ ActionType::kMoveTo, 0, corpse });
            }
            else if (_options.HasPortal && bot.map_id == _options.PortalMap && bot.corpse_map == _options.PortalInsideMap)
                emit(Decision{ ActionType::kMoveTo, 0, _options.PortalPoint });
            else
                fail("no_route_to_corpse_map");
            continue;
        }

        // Alive again after being down: the movement orders are sent afresh, and how it happened is logged.
        if (state.Life != 0)
        {
            if (_options.OnEvent)
                _options.OnEvent("revived: bot=" + std::to_string(bot.guid) + " how=" + (state.AcceptedFrom != 0 ? "resurrect" : state.Reclaimed ? "reclaim" : "unknown") +
                                 " down_ms=" + std::to_string(nowMs - state.DeathMs) + " health_pct=" +
                                 std::to_string(bot.max_health ? std::uint64_t(bot.health) * 100 / bot.max_health : 0));
            state.Life = 0;
            state.ReleaseNow = false;
            state.Last.reset();
            state.Sitting = false;
            state.NextCastMs = 0;
        }

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
        bool const atLeader = leaderSnap && Distance2d(bot, *leaderSnap) <= (state.FollowYards > 0.0f ? state.FollowYards : kFollowLeaderYards);

        // Who an idle bot follows: a member its leader (and stays put once close), else the closest player.
        std::uint64_t followRef = player ? player->guid : 0;
        if (partyMember && leaderSnap)
            followRef = atLeader ? 0 : leaderRef;
        if (state.ChatFollow != 0)
        {
            followRef = state.ChatFollow; // told to follow the speaker
            if (state.FollowYards > 0.0f)
            {
                // A follow distance: walk when farther than it, stop within half of it (the speaker has to be in view to tell).
                for (NearbyUnit const& unit : nearby)
                    if (unit.guid == state.ChatFollow)
                    {
                        float const distanceToSpeaker = rotation::DistanceYards(bot, unit);
                        if (distanceToSpeaker <= state.FollowYards * 0.5f)
                            state.FollowWalking = false;
                        else if (distanceToSpeaker > state.FollowYards)
                            state.FollowWalking = true;
                        if (!state.FollowWalking)
                            followRef = 0;
                        break;
                    }
            }
        }
        if (state.ChatStay)
            followRef = 0;                // told to hold the position

        // The party leader waits for a dead member (a corpse that a rezzer can still resurrect, a ghost on its way back), capped.
        std::uint8_t const deadWaitKind = partyLeader != 0 ? deadWait(partyLeader) : std::uint8_t(0);
        if (partyLeaderBot && deadWaitKind != state.DeadWaitKind)
        {
            if (_options.OnEvent)
                _options.OnEvent(std::string("party_wait: leader=") + std::to_string(bot.guid) + " for=" +
                                 (deadWaitKind == 1 ? "rez" : deadWaitKind == 2 ? "ghost" : "none") + " waited_ms=" +
                                 std::to_string(state.DeadWaitKind != 0 ? nowMs - state.DeadWaitSinceMs : 0));
            if (state.DeadWaitKind == 0)
                state.DeadWaitSinceMs = nowMs;
            state.DeadWaitKind = deadWaitKind;
        }

        // Auto gear: out of combat, once when the bot first appears (if AutoGearing) and after it has gained some levels, or when asked.
        if (bot.in_combat == 0 && nowMs >= state.GearRetryMs)
        {
            bool const alt = _options.IsAlt && _options.IsAlt(bot.guid);
            bool const due = !alt && _options.AutoGearing && (state.GearedAtLevel == 0 || bot.level >= state.GearedAtLevel + _options.AutoGearEveryLevels);
            if (due || (state.GearRequested && !alt))
            {
                BotAction gear;
                gear.bot_guid = bot.guid;
                gear.type = ActionType::kAutoGear;
                gear.spell_id = (classId == kWarriorClass && _options.PartySize != 0) ? 1u : 0u; // a warrior in a party tanks
                actions.push_back(gear);
                state.GearedAtLevel = bot.level != 0 ? bot.level : 1;
                state.GearRequested = false;
                state.GearRetryMs = nowMs + 10000;
            }
        }

        // Quests (M5): the planner's step for this bot. A party member follows its leader instead, and a hurt or fighting bot
        // leaves quests alone until it is done.
        QuestStep questStep;
        bool routeBusy = false;
        if (_quests.Enabled() && !partyMember && !lowHealth && bot.in_combat == 0 && !state.ChatStay && state.ChatFollow == 0)
            questStep = _quests.Next(bot, state.Quest, nowMs);
        std::vector<std::uint32_t> const noEntries;
        std::vector<std::uint32_t> const& huntEntries = questStep.Kind == QuestStepKind::Hunt ? questStep.Entries : noEntries;

        // The target is the threat target (even when it is out of view), otherwise the closest live hostile. A member with
        // no threat of its own fights its leader's target, when that is in view.
        // Dungeon pull control (the route is active): the leader pulls only when its party is ready, the others never pull and join a
        // fight once the tank has held it for a moment. A bot already in combat defends itself whatever the rule says.
        bool dungeonHold = false;
        bool tankHeadStart = false;
        if (_routeActive && partyLeader != 0 && bot.in_combat == 0)
        {
            if (partyLeaderBot)
            {
                bool ready = true;
                std::string not_ready_reason;
                auto const checkReady = [&](BotSnapshot const& member)
                {
                    if (member.life_state != 0)
                        return;
                    if (member.max_health > 0 && std::uint64_t(member.health) * 100 < std::uint64_t(member.max_health) * kPartyReadyHealthPct)
                    {
                        ready = false;
                        not_ready_reason = "health bot=" + std::to_string(member.guid) + " pct=" + std::to_string(std::uint64_t(member.health) * 100 / member.max_health);
                    }
                    else if (member.power_type == 0 && member.max_power > 0 &&
                             std::uint64_t(member.power) * 100 < std::uint64_t(member.max_power) * kPartyReadyPowerPct)
                    {
                        ready = false;
                        not_ready_reason = "mana bot=" + std::to_string(member.guid) + " pct=" + std::to_string(std::uint64_t(member.power) * 100 / member.max_power);
                    }
                    else if (Distance2d(bot, member) > kRouteWaitYards)
                    {
                        ready = false;
                        not_ready_reason = "far bot=" + std::to_string(member.guid);
                    }
                };
                checkReady(bot);
                for (std::uint64_t member : membersOf[bot.guid])
                    if (auto const at = indexOf.find(member); at != indexOf.end())
                        checkReady(view.bots[at->second]);
                if (ready && deadWaitKind != 0)
                {
                    ready = false;
                    not_ready_reason = deadWaitKind == 1 ? "dead_member_rez" : "dead_member_ghost";
                }
                if (ready)
                {
                    if (state.HoldSinceMs != 0 && _options.OnEvent)
                        _options.OnEvent("pull hold ended: leader=" + std::to_string(bot.guid) + " waited_ms=" + std::to_string(nowMs - state.HoldSinceMs));
                    state.HoldSinceMs = 0;
                }
                else
                {
                    if (state.HoldSinceMs == 0)
                    {
                        state.HoldSinceMs = nowMs;
                        if (_options.OnEvent)
                            _options.OnEvent("pull hold started: leader=" + std::to_string(bot.guid) + " reason=" + not_ready_reason);
                    }
                    dungeonHold = nowMs - state.HoldSinceMs < kPullHoldMaxMs;
                }
            }
            else
            {
                dungeonHold = true;
                auto const tank = _bots.find(partyLeader);
                tankHeadStart = tank != _bots.end() && tank->second.InFight && nowMs - tank->second.FightStartMs < kTankHeadStartMs;
            }
        }

        NearbyUnit const* targetUnit = nullptr;
        auto const avoided = [&state, nowMs](std::uint64_t ref)
        {
            auto const until = state.AvoidUntilMs.find(ref);
            return until != state.AvoidUntilMs.end() && nowMs < until->second;
        };
        std::uint64_t targetRef = bot.threat_target_guid;
        if (targetRef != 0 && avoided(targetRef))
            targetRef = 0; // it refused us: the bot does not fight it for now
        // Chat orders: a passive or staying bot starts no fight of its own; a pull order points the bot at the speaker's target.
        bool groupFighting = false;
        if (bot.group_leader_guid != 0)
            for (BotSnapshot const& other : view.bots)
                groupFighting = groupFighting || (other.group_leader_guid == bot.group_leader_guid && other.in_combat != 0 && other.life_state == 0);
        if (partyLeader != 0 && partyFighting(partyLeader))
            groupFighting = true;
        bool const noSelfPull = (state.Stance_ == BotState::Stance::Passive || state.ChatStay ||
                                 (state.Stance_ == BotState::Stance::Defensive && !groupFighting)) && bot.in_combat == 0;
        if (state.Focus != 0 && (bot.in_combat != 0 || groupFighting))
        {
            if (FindLiveMob(nearby, state.Focus))
            {
                state.FocusMissingMs = 0;
                if (!avoided(state.Focus))
                    targetRef = state.Focus; // the focus target first
            }
            else if (state.FocusMissingMs == 0)
                state.FocusMissingMs = nowMs;
            else if (nowMs - state.FocusMissingMs > 30000)
            {
                state.Focus = 0; // out of view for a good while: forgotten
                state.FocusMissingMs = 0;
            }
        }
        if (state.ForcedTarget != 0)
        {
            if (nowMs >= state.ForcedUntilMs || !FindLiveMob(nearby, state.ForcedTarget))
                state.ForcedTarget = 0;
            else if (targetRef == 0 && !avoided(state.ForcedTarget))
                targetRef = state.ForcedTarget;
        }
        if (targetRef == 0 && partyMember && leaderSnap && leaderSnap->threat_target_guid != 0 && !tankHeadStart && !noSelfPull &&
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
        else if (NearbyUnit const* hostile = (dungeonHold || noSelfPull) ? nullptr
                     : PullTarget(bot, nearby, avoided, bot.in_combat != 0, huntEntries, _routeActive))
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
            float approach = outOfPower ? kMeleeApproachYards : rotation::ApproachYards(classId);
            if (nowMs < state.CloserUntilMs)
                approach = std::min(approach, kMeleeApproachYards + 1.0f);
            if (distance <= approach)
            {
                // A ranged bot that has walked into casting range stops there: the core's follow keeps running it into the target,
                // and a spell with a cast time fails while the caster moves.
                if (state.Approaching && approach > kMeleeApproachYards + 0.5f)
                {
                    BotAction stop;
                    stop.bot_guid = bot.guid;
                    stop.type = ActionType::kStop;
                    actions.push_back(stop);
                }
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
            // Dungeon route (doc 19): the leader of a party walks the route while it is healthy and its members are close. Otherwise
            // it stands (and so rests), and the party catches up.
            if (_routeActive && partyLeaderBot && !state.ChatStay && !_options.Route.empty() && state.RouteIndex < _options.Route.size())
            {
                bool together = true;
                for (std::uint64_t member : membersOf[bot.guid])
                {
                    auto const at = indexOf.find(member);
                    if (at == indexOf.end() || view.bots[at->second].life_state != 0)
                        continue;
                    together = together && Distance2d(bot, view.bots[at->second]) <= kRouteWaitYards;
                }
                together = together && deadWaitKind == 0;
                bool const strong = bot.max_health == 0 ||
                                    std::uint64_t(bot.health) * 100 >= std::uint64_t(bot.max_health) * kRouteMinHealthPct;
                Point const* waypoint = &_options.Route[state.RouteIndex];
                float const dx = bot.pos_x - waypoint->X;
                float const dy = bot.pos_y - waypoint->Y;
                if (std::sqrt(dx * dx + dy * dy) <= kRoutePointReachYards)
                {
                    ++state.RouteIndex; // arrived: on to the next point in the same tick
                    waypoint = state.RouteIndex < _options.Route.size() ? &_options.Route[state.RouteIndex] : nullptr;
                }
                if (waypoint && together && strong)
                {
                    wanted = Decision{ ActionType::kMoveTo, 0, *waypoint };
                    routeBusy = true;
                }
            }

            switch (questStep.Kind)
            {
                case QuestStepKind::WalkTo:
                case QuestStepKind::Hunt:
                    // Hunt with the target area equal to the bot's own place (no areas known): stand and wait for the creatures.
                    if (questStep.Kind == QuestStepKind::WalkTo || questStep.Dest.X != bot.pos_x || questStep.Dest.Y != bot.pos_y)
                        wanted = Decision{ ActionType::kMoveTo, 0, Point{ questStep.Dest.X, questStep.Dest.Y, questStep.Dest.Z } };
                    else
                        wanted = Decision{ ActionType::kStop, 0 };
                    break;
                case QuestStepKind::Accept:
                case QuestStepKind::HandIn:
                case QuestStepKind::Abandon:
                {
                    BotAction quest;
                    quest.bot_guid = bot.guid;
                    quest.type = ActionType::kQuest;
                    std::uint32_t const operation = questStep.Kind == QuestStepKind::Accept ? kQuestAccept
                                                    : questStep.Kind == QuestStepKind::HandIn ? kQuestHandIn : kQuestAbandon;
                    quest.spell_id = QuestCode(operation, questStep.Quest);
                    actions.push_back(quest);
                    _quests.NoteSent(state.Quest, operation, questStep.Quest, nowMs);
                    wanted = Decision{ ActionType::kStop, 0 };
                    break;
                }
                case QuestStepKind::Wait:
                case QuestStepKind::None:
                    break;
            }

            // A goto order: walk to the point, then hold there (the order also set stay).
            if (state.GotoActive)
            {
                if (nowMs >= state.GotoUntilMs || DistanceTo(bot, state.GotoPoint) <= 3.0f)
                    state.GotoActive = false;
                else
                {
                    wanted = Decision{ ActionType::kMoveTo, 0, state.GotoPoint };
                    routeBusy = true;
                }
            }
        }

        bool enemyNear = false;
        for (NearbyUnit const& unit : nearby)
            enemyNear = enemyNear || IsLiveMob(unit);
        bool idle = !hasTarget && !enemyNear && wanted.Type == ActionType::kStop && questStep.Kind == QuestStepKind::None && !routeBusy;

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
        if (idle && _options.ServiceEveryMs != 0 && nowMs >= state.NextServiceMs && !state.ChatStay)
        {
            // The nearest vendor on the bot's map, found by the core (layout 6); the nearest repairman instead when the gear is worn
            // (layout 9). None on the map, or a party member: no walk.
            bool const needRepair = bot.repair_valid != 0 && bot.durability_pct < kRepairBelowPct;
            Point const vendor = needRepair ? Point{ bot.repair_x, bot.repair_y, bot.repair_z }
                                            : Point{ bot.vendor_x, bot.vendor_y, bot.vendor_z };
            bool const hasVendor = !partyMember && (needRepair || bot.vendor_valid != 0);
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
        bool const grindMode = _options.Grind && !_options.Grind->Spots.empty() && bot.level != 0;
        if (idle && healthy && grindMode && !partyMember && !state.ChatStay && state.ChatFollow == 0)
        {
            // Leveling: after a quiet spell the bot walks to a grind spot that suits its level (nearest first, not the ones it just left), and
            // stays to fight what lives there until it sees nothing for a while again.
            if (state.Wandering)
            {
                bool const arrived = DistanceTo(bot, state.GrindTarget) <= kHubReachYards;
                bool const timedOut = nowMs - state.WanderStartMs > kWanderTimeoutMs;
                if (arrived || timedOut)
                {
                    state.Wandering = false;
                    state.IdleSinceMs = nowMs;
                }
                else
                {
                    wanted = Decision{ ActionType::kMoveTo, 0, state.GrindTarget };
                    idle = false;
                }
            }
            else if (nowMs - state.IdleSinceMs >= kWanderIdleMs)
            {
                std::uint8_t const side = _options.SideOf ? _options.SideOf(bot.guid) : std::uint8_t(0);
                std::vector<std::size_t> const spots = GrindSpotsFor(*_options.Grind, bot.map_id, bot.level, side, bot.pos_x, bot.pos_y, state.GrindVisited);
                if (!spots.empty())
                {
                    GrindSpot const& spot = _options.Grind->Spots[spots.front()];
                    state.GrindTarget = Point{ spot.X, spot.Y, spot.Z };
                    state.GrindVisited.push_back(spots.front());
                    if (state.GrindVisited.size() > kGrindMemory)
                        state.GrindVisited.erase(state.GrindVisited.begin());
                    state.Wandering = true;
                    state.WanderStartMs = nowMs;
                    wanted = Decision{ ActionType::kMoveTo, 0, state.GrindTarget };
                    idle = false;
                    if (_options.OnEvent)
                        _options.OnEvent("grind_spot: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) +
                                         " spot=" + std::to_string(spots.front()) + " levels=" + std::to_string(spot.MinLevel) + "-" + std::to_string(spot.MaxLevel));
                }
                else
                    state.IdleSinceMs = nowMs; // nothing on this map for this level: look again later
            }
        }
        else if (idle && healthy && !partyMember && !state.ChatStay && state.ChatFollow == 0 && hubs != _options.HubsByMap.end() && !hubs->second.empty())
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
        // Waiting for the party (dungeon): sits to recover health, and mana for a mana user, even with creatures in view.
        bool const holdIdle = dungeonHold && !hasTarget && wanted.Type == ActionType::kStop && !routeBusy;
        bool const manaUser = bot.power_type == 0 && bot.max_power > 0;
        bool const manaLow = holdIdle && manaUser &&
                             std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * (state.Sitting ? kPartyReadySitHealthPct : kPartyReadyPowerPct);
        bool const sitWanted = (idle || holdIdle) && (state.Sitting ? (stillHurt || manaLow) : (hurtEnough || manaLow)) &&
                               nowMs >= state.RestRetryAtMs;
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

        // A player's party invite is accepted (AcceptInvites, from the chat command switch): the player then commands the bot in chat.
        if (_options.AcceptInvites && bot.group_invite_from != 0 && bot.in_combat == 0 && nowMs >= state.InviteAcceptAtMs &&
            forever_module::UnitRefType(bot.group_invite_from) == kPlayerType)
        {
            BotAction accept;
            accept.bot_guid = bot.guid;
            accept.type = ActionType::kGroup;
            accept.spell_id = kGroupAccept;
            actions.push_back(accept);
            state.InviteAcceptAtMs = nowMs + kAcceptInviteRetryMs;
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

        if (IsResurrectSpell(bot.casting_spell) && bot.cast_remaining_ms != 0 && state.Last)
            wanted = *state.Last; // a resurrect cast is not broken by a new movement order

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

        // A heal order: the healer casts its heal on the speaker's selected friend, once.
        if (state.ForcedHeal != 0 && nowMs >= state.NextCastMs && !(bot.casting_spell != 0 && bot.cast_remaining_ms != 0))
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
                continue;
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
            continue;
        }

        // Healing: a healer class in a party heals the party member (itself included) with the least health, below kHealBelowPct, in range.
        if (partyLeader != 0 && nowMs >= state.NextCastMs && !(bot.casting_spell != 0 && bot.cast_remaining_ms != 0))
        {
            std::uint32_t const healSpell = state.Role_ == BotState::Role::Dps ? 0u : HealSpellFor(classId, bot.level);
            auto const retry = healSpell != 0 ? state.RetryAtMs.find(healSpell) : state.RetryAtMs.end();
            if (healSpell != 0 && (retry == state.RetryAtMs.end() || nowMs >= retry->second))
            {
                BotSnapshot const* hurt = nullptr;
                std::uint64_t hurtPct = kHealBelowPct;
                auto const consider = [&](BotSnapshot const& member)
                {
                    if (member.life_state != 0 || member.max_health == 0 || Distance2d(bot, member) > kHealRangeYards)
                        return;
                    std::uint64_t const pct = std::uint64_t(member.health) * 100 / member.max_health;
                    if (pct < hurtPct)
                    {
                        hurtPct = pct;
                        hurt = &member;
                    }
                };
                consider(bot);
                auto const lead = leaderOf.find(bot.guid);
                for (BotSnapshot const& other : view.bots)
                    if (other.guid != bot.guid && leaderOf.count(other.guid) && leaderOf[other.guid] == lead->second)
                        consider(other);
                if (hurt)
                {
                    if (bot.moving != 0 && (KnownCastTimeSpell(healSpell) || state.CastTimeSpells.count(healSpell)))
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
                        heal.spell_id = healSpell;
                        heal.target_guid = hurt->guid == bot.guid ? 0 : PlayerRef(hurt->guid);
                        actions.push_back(heal);
                        state.CastTarget[healSpell] = heal.target_guid;
                        if (_options.OnEvent)
                            _options.OnEvent("heal: healer=" + std::to_string(bot.guid) + " spell=" + std::to_string(healSpell) +
                                             " target=" + std::to_string(hurt->guid) + " target_hp_pct=" + std::to_string(hurtPct));
                        state.NextCastMs = nowMs + kGlobalCooldownMs;
                    }
                    continue;
                }
            }

            // Resurrect (after the heals): a priest, paladin or shaman out of combat with the party quiet resurrects the dead member in range
            // that it is closest to (healers first, then the tank), once the 10 s cast is affordable. A refusal retries after a few seconds.
            std::uint32_t const rezSpell = ResurrectSpellFor(classId, bot.level);
            bool const manaOk = bot.power_type != 0 || bot.max_power == 0 ||
                                std::uint64_t(bot.power) * 100 >= std::uint64_t(bot.max_power) * kRezManaPct;
            if (rezSpell != 0 && bot.in_combat == 0 && !hasTarget && manaOk && !partyFighting(partyLeader))
            {
                BotSnapshot const* pick = nullptr;
                int pickRank = 99;
                float pickDist = 0.0f;
                forParty(partyLeader, [&](BotSnapshot const& dead)
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
                    forParty(partyLeader, [&](BotSnapshot const& other)
                    {
                        if (other.guid == bot.guid || !isRezzer(other) || other.in_combat != 0 || other.map_id != bot.map_id)
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
                    continue;
                }
            }
        }

        // Upkeep buffs (rotation file rows 'buff'): out of combat, standing, with nothing to fight. Each is cast on the bot, or on every member of its group
        // (party), when its last cast has run out; a refused cast is tried again after the usual retry.
        if (!hasTarget && bot.in_combat == 0 && bot.moving == 0 && nowMs >= state.NextCastMs && !(bot.casting_spell != 0 && bot.cast_remaining_ms != 0) &&
            !state.Sitting)
        {
            bool cast = false;
            for (rotation::Row const& row : RowsFor(classId))
            {
                if (!row.Buff || (bot.level != 0 && (row.MinLevel > bot.level || (row.MaxLevel != 0 && bot.level > row.MaxLevel))))
                    continue;
                auto const retry = state.RetryAtMs.find(row.Spell);
                if (retry != state.RetryAtMs.end() && nowMs < retry->second)
                    continue;
                // The recipients: the bot itself (reference 0), and for a party buff the members of its group that are alive and close.
                std::vector<std::uint64_t> recipients{ 0 };
                if (row.Party)
                    for (BotSnapshot const& other : view.bots)
                        if (other.guid != bot.guid && other.life_state == 0 && other.group_leader_guid != 0 && other.group_leader_guid == bot.group_leader_guid &&
                            Distance2d(bot, other) <= kBuffRangeYards)
                            recipients.push_back(PlayerRef(other.guid));
                for (std::uint64_t recipient : recipients)
                {
                    auto const until = state.BuffUntilMs.find({ row.Spell, recipient });
                    if (until != state.BuffUntilMs.end() && nowMs < until->second)
                        continue;
                    BotAction buff;
                    buff.bot_guid = bot.guid;
                    buff.type = ActionType::kCastSpell;
                    buff.spell_id = row.Spell;
                    buff.target_guid = recipient;
                    actions.push_back(buff);
                    state.CastTarget[row.Spell] = recipient;
                    state.NextCastMs = nowMs + kGlobalCooldownMs;
                    cast = true;
                    break;
                }
                if (cast)
                    break;
            }
            if (cast)
                continue;
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
        // A cast or channel in progress finishes first: no cast can start meanwhile.
        if (bot.casting_spell != 0 && bot.cast_remaining_ms != 0)
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

        for (rotation::Row const& row : RowsFor(classId))
        {
            if (row.Buff)
                continue; // upkeep spells are cast out of combat, below
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
            if (row.OnTarget && (distance < row.MinRange || (row.MaxRange > 0.0f && distance > row.MaxRange)))
                continue;
            if (!rotation::Allowed(row.When, facts))
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
    }

    for (auto it = _bots.begin(); it != _bots.end();)
        it = present.count(it->first) ? std::next(it) : _bots.erase(it);

    return actions;
}
