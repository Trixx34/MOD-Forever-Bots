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
constexpr float kRangedKiteYards = 12.0f;    // a ranged bot keeps a melee target this far away
constexpr float kFleeYards = 15.0f;          // below 30% health: flees while a threat is this close
constexpr std::uint64_t kGlobalCooldownMs = 1500;
constexpr std::uint64_t kRetryMs = 1000;     // out of range, invalid target, refused, not enough power
constexpr std::uint64_t kCooldownRetryMs = 2000;
constexpr float kRangeSlackYards = 2.0f;        // a ranged row is used this far inside its range: the snapshot is a tick old and the units move
constexpr std::uint64_t kPowerRetryMs = 2000;    // a cast refused for lack of mana, rage or energy: power comes back slowly
constexpr std::uint64_t kHeldCastMs = 500;       // after a cooldown or generic refusal no other spell is tried for this long (often the global cooldown)
constexpr std::uint64_t kCloserAfterRangeMs = 5000; // after an out-of-range refusal the bot stands closer for this long
constexpr float kCasterCloserYards = 15.0f;      // the closer stand-off of a ranged bot after an out-of-range refusal
constexpr float kMeleeCloserYards = 3.0f;        // and of a melee bot
constexpr std::uint32_t kLosRefusalsAvoid = 3;   // this many line-of-sight refusals in a row and the target is avoided
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
constexpr std::uint32_t kServiceBuyHealthPotion = 64; // layout 16: the core's settings give the counts
constexpr std::uint32_t kServiceBuyManaPotion = 128;
constexpr std::uint32_t kServiceBuyBandage = 256;
constexpr std::uint32_t kServiceBuyReagent = 512;  // the item and the count are the core's
constexpr std::uint32_t kServiceBuyAmmo = 8;     // layout 15: buy ammunition (dest_x = the count to carry), food (dest_y), drink (dest_z)
constexpr std::uint32_t kServiceBuyFood = 16;
constexpr std::uint32_t kServiceBuyDrink = 32;
constexpr std::uint64_t kSupplyJudgeAfterMs = 7000; // a supply trip is judged this long after the buy: the core counts the bags every 2 seconds
// Combo points: the rogue's Sinister Strike spell and the spend, and the points a finisher needs.
constexpr std::uint32_t kSinisterStrike = 1752;
constexpr std::uint32_t kEviscerate = 2098;
constexpr std::uint32_t kAutoShot = 75;
constexpr std::uint32_t kCallPet = 883;
constexpr std::uint32_t kRevivePet = 982;
constexpr std::uint32_t kMendPet = 136;
constexpr std::uint32_t kHunterClass = 3;
constexpr std::uint64_t kPetBackoffMs = 30000;       // first wait after a refused revive; doubles per failure
constexpr std::uint64_t kPetBackoffMaxMs = 300000;
constexpr std::uint64_t kPetProbeMs = 120000;        // below level 12 a Call Pet check after a fight is sent at most this often
constexpr std::uint64_t kPetMendEveryMs = 30000;     // Mend Pet in a fight at most this often
constexpr std::uint64_t kPetMendAfterMs = 15000;     // and only once the fight has lasted this long
constexpr std::uint32_t kPetMendManaPct = 50;         // and with at least this much mana
constexpr std::uint32_t kPetMendHealthPct = 85;        // layout 13: a pet below this health is healed
// Layout 13: ActionType::kPetCommand spell_id (the pet is in the snapshot now, so it can be commanded, not just called).
constexpr std::uint32_t kPetAttack = 1;               // attack target_guid
constexpr std::uint32_t kPetFollow = 2;
constexpr std::uint32_t kPetStay = 3;                 // hold position
constexpr std::uint32_t kPetStance = 4;               // passive, defensive or aggressive, in dest_x
constexpr std::uint32_t kMaxComboPoints = 5;
// Saber Slash (Forever): like Sinister Strike it gives one combo point (SpellEffect 30 energize, power type 4, 1 point, in the client data).
constexpr std::uint32_t kSaberSlash = 424785;

// Combo points a rogue spell adds on its target when it lands, 0 for any other spell.
std::uint32_t ComboPointsGained(std::uint32_t spell)
{
    return spell == kSinisterStrike || spell == kSaberSlash ? 1 : 0;
}
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
// After a hub walk that timed out (a hub in rock, or past what the pathing allows), the next hub is tried this much sooner than a
// full idle interval: going round the list must not cost the bot 30 s of standing for every unreachable hub.
constexpr std::uint64_t kHubRetryIdleMs = 5000;
constexpr float kHubReachYards = 10.0f;          // a bot this close to its hub has arrived
constexpr float kVendorReachYards = 4.0f;       // a bot this close to its vendor can use it (the core allows 5)
constexpr std::uint64_t kBossHoldMs = 180000;     // how long the leader waits at a boss point for the boss to die before it goes on
constexpr std::uint32_t kRouteNoRouteSkip = 3;     // refusals in a row (no route) after which a dungeon waypoint is skipped
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
// The tank (the party leader) is kept alive first while it holds a target, even a little below this.
constexpr std::uint32_t kTankHealBelowPct = 90;
// How long a heal claim on a member lasts: one global cooldown and the cast, so a second healer looks elsewhere meanwhile.
constexpr std::uint64_t kHealClaimMs = 2500;
constexpr float kHealRangeYards = 30.0f;
constexpr float kBuffRangeYards = 30.0f;            // a party buff reaches members this close
constexpr std::size_t kGrindMemory = 2;           // grind spots remembered as just visited
constexpr std::uint32_t kAcceptRefusalsMax = 3;       // resurrect answers refused this often in a row: the offer is left alone for a while
constexpr std::uint64_t kAcceptIgnoreMs = 60000;
constexpr std::uint64_t kFightStallMs = 45000;       // a fight in which neither side's health went down this long is given up (the telemetry calls 60 s "stuck")
constexpr std::uint64_t kStallAvoidMs = 300000;      // a target the fight stalled against is left alone 5 minutes, 10, 20, then 40 at most
constexpr std::uint32_t kMaxStallDoublings = 3;
constexpr std::uint32_t kRestManaBelowPct = 40;      // an idle mana user below this sits down to drink in the open
constexpr std::uint32_t kRestManaUntilPct = 90;      // and stays sat until it has this much
constexpr std::uint64_t kManaRestMaxMs = 120000;     // but never longer than this at a time
constexpr std::uint64_t kManaRestBlockMs = 300000;   // and not again for this long after a rest that did not refill the mana
constexpr std::uint32_t kHubFailuresMax = 2;         // hub walks in a row that failed before the bot explores on foot instead
constexpr std::uint64_t kStuckCheckMs = 300000;       // every 5 minutes a bot that has not moved reports why it is not doing anything
constexpr std::uint64_t kGrindRetryMs = 60000;        // a bot that found no grind spot explores on foot this long before it looks again
constexpr std::uint64_t kExploreIdleMs = 10000;      // an idle bot with nowhere to go takes a short walk after this long
constexpr std::uint64_t kExploreTimeoutMs = 60000;
constexpr float kExploreReachYards = 6.0f;
constexpr std::uint64_t kAcceptInviteRetryMs = 3000; // an invite from a player is answered again after this long if it is still pending
constexpr std::uint64_t kGotoTimeoutMs = 90000;     // a goto order is dropped after this long
constexpr std::uint64_t kPullOrderMs = 30000;      // a pull order is dropped after this long, or when the target is gone
constexpr std::uint64_t kCloserAfterLosMs = 5000;   // after a line-of-sight refusal the bot closes to melee range for this long
// Gear a bot carries (traded, looted, a quest reward) is worn when it is better. The core compares item levels; asking is cheap, so every
// bot asks this often, spread by guid so a few hundred bots do not all ask in one tick.
constexpr std::uint64_t kBagGearEveryMs = 300000;
constexpr std::uint64_t kBagGearSpreadMs = 60000;
constexpr std::uint64_t kTravelTimeoutMs = 180000; // a walk to the vendor longer than this is dropped for now
constexpr float kMountMinDistanceYards = 100.0f;   // a walk this long: mount up before it
constexpr float kMountDismountDistanceYards = 15.0f; // arrived or close: dismount
// A quest walk that keeps the bot within kQuestWalkStuckYards for kQuestWalkStuckMs has no route to its giver (seen on the sim: 11
// level 1 bots idle for hours, 110 to 8000 yards from the giver). With QuestTeleport the bot is sent there once; a refusal, or a second
// stuck period, drops the start (kNoRouteBlockMs) so the bot grinds. With QuestTeleport off the start is dropped at once: the core
// refuses the teleport while Bot.Actions.Teleport is 0, and retrying it never ends the walk.
constexpr std::uint64_t kQuestWalkStuckMs = 120000;
constexpr float kQuestWalkStuckYards = 5.0f;
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

// A cast or channel is in progress. cast_remaining_ms is no test: it is 0 while a spell is in flight (a Fireball after the cast, state
// "delayed"), which the core still counts as casting and refuses every other cast for ("bot is casting spell N").
bool IsCasting(BotSnapshot const& bot)
{
    return bot.casting_spell != 0;
}

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

// The spells that stop a cast in progress (rotation rows with Cond::TargetCasting). When several party members know one, only
// one may use it on the same cast: a second kick lands on nothing and both casts are wasted.
bool IsInterruptSpell(std::uint32_t spell)
{
    return spell == 853 || spell == 1766 || spell == 8042; // Hammer of Justice, Kick, Earth Shock
}

// Layout 13: whether the bot carries this aura right now. The aura list replaces the module's own buff timers, so a buff
// another player cast over the bot, or one it lost to a death, is the fact it is. An aura with no duration reports a large
// remaining_ms and runs for ever; a reported aura with 0 left is one that just expired.
bool AuraRunning(BotSnapshot const& bot, std::uint32_t spell)
{
    for (std::uint32_t i = 0; i < bot.aura_count && i < forever_module::kMaxAuras; ++i)
        if (bot.auras[i].spell_id == spell)
            return bot.auras[i].remaining_ms != 0;
    return false;
}

// Layout 13: the bot's pet, from the snapshot, instead of what refused casts imply. pet_state is 0 none, 1 alive, 2 dead.
// A core that does not report pets leaves it 0, and the inference from refused casts below is the fallback.
enum class PetKnown : std::uint8_t { kNone, kOut, kDead };

PetKnown PetOf(BotSnapshot const& bot)
{
    switch (bot.pet_state)
    {
        case 1: return PetKnown::kOut;
        case 2: return PetKnown::kDead;
        default: return PetKnown::kNone;
    }
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

// The level difference at which a creature gives no experience at all (the classic "grey" rule: GetZeroDifference). Whether this fork
// changes it is not known; the vanilla table is used.
std::uint32_t ZeroExpDifference(std::uint32_t level)
{
    if (level < 8)
        return 5;
    if (level < 10)
        return 6;
    if (level < 12)
        return 7;
    if (level < 16)
        return 8;
    if (level < 20)
        return 9;
    if (level < 30)
        return 11;
    if (level < 40)
        return 12;
    if (level < 45)
        return 13;
    if (level < 50)
        return 14;
    if (level < 55)
        return 15;
    if (level < 60)
        return 16;
    return 17;
}

// A creature far enough below the bot that killing it gives no experience: not worth pulling. (A level of 0 is unknown and not judged.)
bool IsGrey(BotSnapshot const& bot, NearbyUnit const& unit)
{
    return bot.level != 0 && unit.level != 0 && unit.level < bot.level && bot.level - unit.level >= ZeroExpDifference(bot.level);
}

// A unit's share of its health (0..1), or 1 when its maximum is unknown. A pull prefers the weakest, so a kill comes sooner.
float HealthShare(NearbyUnit const& unit)
{
    return unit.max_health ? float(unit.health) / float(unit.max_health) : 1.0f;
}

// The live hostile this bot should pull, or nullptr. A group is the hostiles within kPullRadius of the candidate, the candidate
// included. Units already attacking the bot are not chosen here; the policy fights its threat target whatever the group size.
// The weakest target wins and the closest breaks the tie: finishing one creature before the next is more experience per hour
// than walking to whichever one happens to stand nearest first.
NearbyUnit const* PullTarget(BotSnapshot const& bot, std::span<NearbyUnit const> units,
                            std::function<bool(std::uint64_t)> const& avoided, bool defending,
                            std::vector<std::uint32_t> const& onlyEntries = {}, bool dungeon = false)
{
    NearbyUnit const* best = nullptr;
    float bestHealth = 0.0f;
    float bestDist = 0.0f;
    for (NearbyUnit const& candidate : units)
    {
        if (!IsLiveMob(candidate) || avoided(candidate.guid))
            continue;
        // A bot hunting for a quest pulls only the creatures the quest wants (unless it is already being attacked).
        if (!defending && !onlyEntries.empty() &&
            std::find(onlyEntries.begin(), onlyEntries.end(), forever_module::UnitRefEntry(candidate.guid)) == onlyEntries.end())
            continue;
        // Nothing to gain from a grey creature (no experience): left alone, so the bot goes looking for something that levels it. Not when
        // hunting for a quest (the quest wants it), nor in a dungeon, nor when the bot is already being attacked.
        if (!defending && !dungeon && onlyEntries.empty() && IsGrey(bot, candidate))
            continue;
        // Not worth pulling: well above the bot's level, or an elite. (A level of 0 is unknown and not judged.) A bot that is already
        // in combat is being attacked, and the snapshot does not say by what: it fights the closest creature, whatever its level, since
        // standing still until it dies is worse. The same goes for the pack-size rule below.
        // In a dungeon (the route is active) elites, bosses and higher levels are the content: the party fights what it meets.
        if (!defending && !dungeon && bot.level != 0 &&
            ((candidate.level != 0 && candidate.level > bot.level + kMaxLevelAbove) || candidate.elite))
            continue;
        float const health = HealthShare(candidate);
        float const dist = rotation::DistanceYards(bot, candidate);
        if (best && !(health < bestHealth || (health == bestHealth && dist < bestDist)))
            continue;

        std::size_t group = 0;
        for (NearbyUnit const& other : units)
            if (IsLiveMob(other) && DistanceBetween(candidate, other) <= kPullRadius)
                ++group;
        if (!defending && group > (dungeon ? kMaxDungeonPull : kMaxPull))
            continue;

        best = &candidate;
        bestHealth = health;
        bestDist = dist;
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

// A stalled fight (nothing lost on either side): the target is left alone much longer than a refused one, more each time, because it is
// out of reach or bugged, and the bot has nothing to gain by coming back to it every two minutes (a mage did that for an hour and a half).
void AvoidStalledUnit(std::unordered_map<std::uint64_t, std::uint64_t>& avoidUntil, std::unordered_map<std::uint64_t, std::uint32_t>& count,
                      std::uint64_t ref, std::uint64_t nowMs)
{
    std::uint32_t const times = ++count[ref];
    std::uint32_t const doublings = std::min<std::uint32_t>(times - 1, kMaxStallDoublings);
    avoidUntil[ref] = nowMs + (kStallAvoidMs << doublings);
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

// A hunter whose ranged weapon uses ammunition and who carries none (layout 15: the core fills ammo_entry and ammo_count).
bool NoAmmo(BotSnapshot const& bot, std::uint32_t classId)
{
    return classId == kHunterClass && bot.ammo_entry != 0 && bot.ammo_count == 0;
}

// The snapshot facts the supply logic reads (supply_policy.h).
supply::Facts FactsOf(BotSnapshot const& bot)
{
    supply::Facts facts;
    facts.AmmoEntry = bot.ammo_entry;
    facts.AmmoCount = bot.ammo_count;
    facts.FoodCount = bot.food_count;
    facts.DrinkCount = bot.drink_count;
    facts.ManaUser = bot.power_type == 0 && bot.max_power > 0;
    facts.Copper = bot.copper;
    facts.FreeSlots = bot.bag_free_slots;
    facts.AmmoVendor = bot.ammo_vendor_valid != 0;
    facts.FoodVendor = bot.food_vendor_valid != 0;
    facts.HealthPotions = bot.health_potion_count;
    facts.ManaPotions = bot.mana_potion_count;
    facts.Bandages = bot.bandage_count;
    facts.GoodsVendor = bot.goods_vendor_valid != 0;
    facts.ReagentEntry = bot.reagent_entry;
    facts.ReagentCount = bot.reagent_count;
    facts.ReagentVendor = bot.reagent_vendor_valid != 0;
    return facts;
}

} // namespace

bool BotPolicy::SafeToLogOut(BotSnapshot const& bot) const
{
    if (bot.in_combat != 0 || bot.life_state != 0 || bot.group_role != 0 || bot.group_invite_from != 0)
        return false;
    auto const found = _bots.find(bot.guid);
    if (found == _bots.end())
        return true;
    BotState const& state = found->second;
    return !state.ChatStay && state.ChatFollow == 0 && !state.Traveling && !state.SupplyTrip && state.SupplyCheckAtMs == 0 && !state.ServiceUrgent;
}

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
        case BotActionStatus::kNoRoute: return "no route";
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

    // A flight refused (no flight master in reach, no path, not worth it, in combat): the bot walks for a while.
    if (bot.last_action_type == uint8_t(ActionType::kTaxi) && bot.last_action_status != BotActionStatus::kApplied &&
        bot.last_action_status != BotActionStatus::kNone)
    {
        state.TaxiBlockedUntilMs = nowMs + _options.Taxi.BlockedMs;
        state.TaxiWaitUntilMs = 0;
        return;
    }

    // Eating or drinking refused: busy or moving is a short wait, anything else (nothing to eat, supplies off in the core) a long one.
    if (bot.last_action_type == uint8_t(ActionType::kConsume) && bot.last_action_status != BotActionStatus::kApplied &&
        bot.last_action_status != BotActionStatus::kNone)
    {
        bool const brief = bot.last_action_status == BotActionStatus::kBusy || bot.last_action_status == BotActionStatus::kMoving;
        state.NextConsumeMs = nowMs + (brief ? 8000 : 120000);
        return;
    }

    // Auto gear refused (in combat, switched off in the core): tried again after a while.
    if (bot.last_action_type == uint8_t(ActionType::kAutoGear) && bot.last_action_status == BotActionStatus::kRefused)
    {
        state.GearedAtLevel = 0;
        state.GearRetryMs = nowMs + 60000;
        return;
    }

    // A move the core found no route for, or a quest-walk teleport the core refused: that start's giver is left alone for a while
    // (the planner picks the next start). Each result is read once, by its sequence. A refused teleport must not be sent again.
    bool const noRoute = bot.last_action_type == uint8_t(ActionType::kMoveTo) && bot.last_action_status == BotActionStatus::kNoRoute;
    bool const teleportRefused = bot.last_action_type == uint8_t(ActionType::kTeleport) &&
                                 bot.last_action_status != BotActionStatus::kApplied && bot.last_action_status != BotActionStatus::kNone;
    if (noRoute && bot.last_action_sequence != state.NoRouteSequence)
        ++state.RouteNoRoute; // a dungeon leader counts these against its current waypoint (kRouteNoRouteSkip)
    if ((noRoute || teleportRefused) && bot.last_action_sequence != state.NoRouteSequence)
    {
        state.NoRouteSequence = bot.last_action_sequence;
        state.QuestTeleportSent = false;
        _quests.NoteNoRoute(state.Quest, nowMs);
        if (teleportRefused && _options.OnEvent)
            _options.OnEvent("quest_walk_blocked: bot=" + std::to_string(bot.guid) + " reason=teleport_refused status=" +
                             ActionStatusName(bot.last_action_status));
    }

    // A quest action refused (not eligible, bags full, no NPC in reach): the quest is left alone for a while.
    if (bot.last_action_type == uint8_t(ActionType::kQuest) && bot.last_action_status == BotActionStatus::kRefused)
    {
        if (_quests.NoteRefused(state.Quest, nowMs))
        {
            // A hand-in the core refused: the reward did not fit in the bags. A bot with quest work is never idle, so the vendor visit that
            // empties the bags (sell, equip) is not left to the idle bot's turn.
            state.ServiceUrgent = true;
            state.NextServiceMs = 0;
        }
        return;
    }

    // A party action refused (no invite waiting, a full party): the party waits a moment before the next one.
    if (bot.last_action_type == uint8_t(ActionType::kGroup) && bot.last_action_status == BotActionStatus::kRefused)
    {
        state.PartyRetryAtMs = nowMs + kPartyRetryMs;
        return;
    }

    // Mount / dismount: record whether the request succeeded so the policy does not keep repeating it.
    if (bot.last_action_type == uint8_t(ActionType::kMount))
    {
        if (bot.last_action_status == BotActionStatus::kApplied)
            state.Mounted = (bot.last_action_spell == 1);
        else
        {
            state.Mounted = false;
            state.MountRetryMs = nowMs + 15000;
        }
        return;
    }

    // The death actions: a refusal is logged, and the action waits before it is sent again.
    if (bot.last_action_type == uint8_t(ActionType::kReleaseSpirit) || bot.last_action_type == uint8_t(ActionType::kResurrect) ||
        bot.last_action_type == uint8_t(ActionType::kReclaimCorpse))
    {
        bool const ok = bot.last_action_status == BotActionStatus::kApplied;
        char const* const what = bot.last_action_type == uint8_t(ActionType::kReleaseSpirit) ? "release"
                                 : bot.last_action_type == uint8_t(ActionType::kResurrect) ? "resurrect_accept" : "reclaim";
        bool logIt = !ok;
        if (bot.last_action_type == uint8_t(ActionType::kResurrect))
        {
            // An answer to a resurrect offer that is refused again and again must not hold the bot: after a few it is left alone for a minute.
            if (ok)
                state.AcceptRefusals = 0;
            else
            {
                logIt = state.AcceptRefusals == 0; // one line per run of refusals
                if (++state.AcceptRefusals >= kAcceptRefusalsMax)
                {
                    state.AcceptIgnoreUntilMs = nowMs + kAcceptIgnoreMs;
                    state.AcceptIgnoreFrom = bot.resurrect_from; // a different resurrector is still answered
                    state.AcceptRefusals = 0;
                }
            }
        }
        if (logIt && _options.OnEvent)
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
    if (spell == kCallPet || spell == kRevivePet || spell == kMendPet)
    {
        // The snapshot has no pet, so the results of the pet casts are the only evidence of its state.
        switch (bot.last_action_status)
        {
            case BotActionStatus::kApplied:
                state.PetMissing = false;
                state.PetDead = false;
                state.PetProbe = false;
                state.PetFailures = 0;
                state.PetRetryAtMs = 0;
                if (spell == kMendPet)
                    state.AutoShotTarget = 0; // the channel stops Auto Shot: it is cast again afterwards
                break;
            case BotActionStatus::kRefused:
            case BotActionStatus::kInvalidTarget:
            case BotActionStatus::kNoLineOfSight:
                if (spell == kCallPet)
                {
                    // Call Pet is refused for a dead pet (or for no pet at all, which Revive Pet's refusal then shows).
                    state.PetDead = true;
                    state.PetMissing = true;
                }
                else if (spell == kRevivePet)
                {
                    state.PetDead = false; // maybe it was alive after all, or there is no pet: Call Pet is tried again after the backoff
                    state.PetMissing = true;
                    ++state.PetFailures;
                    std::uint64_t wait = kPetBackoffMs;
                    for (std::uint32_t i = 1; i < state.PetFailures && wait < kPetBackoffMaxMs; ++i)
                        wait *= 2;
                    state.PetRetryAtMs = nowMs + std::min(wait, kPetBackoffMaxMs);
                }
                else
                {
                    state.PetMissing = true; // Mend Pet has no pet to heal
                    state.PetProbe = false;
                }
                break;
            default:
                break; // busy, moving, cooldown, no power, out of range, not known: the usual retry below, the pet state is unchanged
        }
    }
    if (spell == kAutoShot && bot.last_action_status == BotActionStatus::kApplied)
    {
        auto const shot = state.CastTarget.find(spell);
        if (shot != state.CastTarget.end())
            state.AutoShotTarget = shot->second;
    }
    if (bot.last_action_status == BotActionStatus::kInvalidTarget)
    {
        // The cast had no valid target: that unit is avoided too, so the next cast goes to another one.
        auto const cast = state.CastTarget.find(spell);
        if (cast != state.CastTarget.end() && cast->second != 0)
            AvoidUnit(state.AvoidUntilMs, state.AvoidCount, cast->second, nowMs);
    }
    if (bot.last_action_status == BotActionStatus::kNoLineOfSight)
    {
        state.CloserUntilMs = nowMs + kCloserAfterLosMs; // walls between them: close in rather than cast through them
        auto const cast = state.CastTarget.find(spell);
        if (++state.LosRefusals >= kLosRefusalsAvoid && cast != state.CastTarget.end() && cast->second != 0)
        {
            AvoidUnit(state.AvoidUntilMs, state.AvoidCount, cast->second, nowMs); // still no sight after closing in: another target
            state.LosRefusals = 0;
        }
    }
    else if (bot.last_action_status != BotActionStatus::kBusy && bot.last_action_status != BotActionStatus::kMoving)
        state.LosRefusals = 0;
    if (bot.last_action_status == BotActionStatus::kOutOfRange)
        state.RangeCloserUntilMs = nowMs + kCloserAfterRangeMs; // the snapshot's distance was short of the core's: stand closer for a while
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

            if (std::uint32_t const gained = ComboPointsGained(spell); gained != 0)
            {
                // A builder on a new target starts the count again.
                state.ComboPoints = (state.ComboTarget == cast->second) ? std::min(state.ComboPoints + gained, kMaxComboPoints) : gained;
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
            state.NextCastMs = std::max(state.NextCastMs, nowMs + kHeldCastMs); // usually the global cooldown: no other spell meanwhile
            break;
        case BotActionStatus::kBusy:
            state.RetryAtMs[spell] = nowMs + kBusyRetryMs; // another cast is in progress: soon, not after the long retry
            state.NextCastMs = std::max(state.NextCastMs, nowMs + kBusyRetryMs * 2); // and no other spell is tried meanwhile
            break;
        case BotActionStatus::kMoving:
            state.CastTimeSpells.insert(spell);            // remembered: the bot stops before this spell from now on
            state.RetryAtMs[spell] = nowMs + kBusyRetryMs;
            break;
        case BotActionStatus::kNotEnoughPower:
            state.RetryAtMs[spell] = nowMs + kPowerRetryMs;
            break;
        case BotActionStatus::kRefused:
            state.RetryAtMs[spell] = nowMs + kRetryMs;
            // A silent refusal (result 32) is the global cooldown too. A refused pet cast is the answer to "is there a pet": no hold, the
            // follow-up (Revive Pet after Call Pet) goes at once.
            if (spell != kCallPet && spell != kRevivePet && spell != kMendPet)
                state.NextCastMs = std::max(state.NextCastMs, nowMs + kHeldCastMs);
            break;
        case BotActionStatus::kOutOfRange:
        case BotActionStatus::kNoLineOfSight:
        case BotActionStatus::kInvalidTarget:
            state.RetryAtMs[spell] = nowMs + kRetryMs;
            break;
        case BotActionStatus::kApplied:
        case BotActionStatus::kNone:
        case BotActionStatus::kNotOnline:
            break;
    }
}

std::vector<std::string> BotPolicy::ApplyChat(WorldTickSnapshotView const& view, ChatContext const& chat, ChatCommand const& command,
                                               std::uint64_t nowMs, std::vector<ChatAck>* acks)
{
    std::vector<std::string> lines;
    std::vector<std::uint64_t> accepted; // the bots that took the order, for the say-back
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
        std::uint32_t const refusedBefore = refused;
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
        if (refused == refusedBefore)
            accepted.push_back(bot.guid);
        ++index;
    }
    if (acks && chat.Say)
    {
        for (std::uint64_t const guid : PickSpeakers(accepted, command.Verb, chat.Recipient != 0, chat.SayMax))
            if (std::string text = AckLine(command.Verb, guid); !text.empty())
                acks->push_back({ guid, std::move(text) });
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
    // Whether another member of this bot's party is already stopping a cast. The snapshot says who casts what, and a member with
    // casting_spell set is mid-cast (cast_remaining_ms is 0 for a spell in flight, which the core still counts as casting).
    // Only the party is looked at, so the cost does not grow with the bot count.
    auto const interruptTaken = [&](BotSnapshot const& bot, std::uint64_t leaderGuid) -> bool
    {
        if (leaderGuid == 0)
            return false; // not in a party of this module's: nobody to coordinate with
        bool taken = false;
        forParty(leaderGuid, [&](BotSnapshot const& member)
        {
            if (member.guid == bot.guid || member.life_state != 0 || member.casting_spell == 0)
                return;
            if (IsInterruptSpell(member.casting_spell))
                taken = true;
        });
        return taken;
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

        // Layout 13: the core says why the bot lost its last target (0 none, 1 killed, 2 leashed, 3 despawned, 4 another player).
        // A creature that leashed or despawned is not worth pulling again at once, so it is avoided like a refused one;
        // a kill is not, which keeps the bot working the corpses it just made.
        if (state.SeenThreatTarget != bot.threat_target_guid)
        {
            if (state.SeenThreatTarget != 0 && bot.threat_target_guid == 0 && bot.last_target_lost_reason != 0 &&
                bot.last_target_lost_reason != state.LostTargetReason)
            {
                state.LostTargetReason = bot.last_target_lost_reason;
                if (bot.last_target_lost_reason >= 2)
                    state.AvoidUntilMs[state.SeenThreatTarget] = nowMs + kAvoidMs;
            }
            state.SeenThreatTarget = bot.threat_target_guid;
        }
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
                state.AutoShotTarget = 0;
                state.PetMissing = true; // the pet is dismissed with its dead owner: Call Pet after the resurrection
                state.PetDead = false;
                state.PetProbe = false;
                state.PetFought = false;
                state.PetCommanded = false;
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
            if (bot.resurrect_from != 0 && !(nowMs < state.AcceptIgnoreUntilMs && bot.resurrect_from == state.AcceptIgnoreFrom))
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

        // The closest friendly player an idle bot follows. A player is someone else: another bot this module controls is not one. Bots standing
        // together at a start zone each followed the nearest of the others, in circles, and never left (the Undead, Tauren and Night Elf bots of
        // the soak, which stand on the same few spots by race).
        NearbyUnit const* player = nullptr;
        {
            float bestPlayerDist = std::numeric_limits<float>::max();
            for (NearbyUnit const& unit : nearby)
            {
                if (!IsPlayer(unit) || indexOf.count(unit.guid & forever_module::kUnitRefCounterMask) != 0)
                    continue;
                float const dist = rotation::DistanceYards(bot, unit);
                if (dist < bestPlayerDist)
                {
                    bestPlayerDist = dist;
                    player = &unit;
                }
            }
        }
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
            else if (!alt && bot.life_state == 0 && (state.BagGearRequested || nowMs >= state.NextBagGearMs))
            {
                // Not tied to an auto gear pass, and it works with AutoGearing off: spell_id 2 is "only wear what is better in the bags".
                // The first look at a bot only sets its schedule.
                if (state.NextBagGearMs != 0 || state.BagGearRequested)
                {
                    BotAction gear;
                    gear.bot_guid = bot.guid;
                    gear.type = ActionType::kAutoGear;
                    gear.spell_id = 2u;
                    actions.push_back(gear);
                }
                state.BagGearRequested = false;
                state.NextBagGearMs = nowMs + kBagGearEveryMs + (bot.guid % kBagGearSpreadMs / 1000) * 1000;
            }
        }

        // Quests (M5): the planner's step for this bot. A party member follows its leader instead, and a hurt or fighting bot
        // leaves quests alone until it is done.
        QuestStep questStep;
        bool routeBusy = false;
        bool const questsAllowed = _quests.Enabled() && !partyMember && !lowHealth && bot.in_combat == 0 && !state.ChatStay && state.ChatFollow == 0;
        if (questsAllowed)
            questStep = _quests.Next(bot, state.Quest, nowMs, QuestBotInfo{ classId, _options.RaceOf ? _options.RaceOf(bot.guid) : 0u });
        // Every five minutes, what the quest planner is doing for this bot and what keeps it from doing more (the events are the only record,
        // since a planner that takes no quest logs nothing otherwise).
        if (_options.OnEvent && nowMs - state.QuestDiagAtMs >= 300000)
        {
            state.QuestDiagAtMs = nowMs;
            static constexpr char const* kinds[] = { "none", "walk_to", "accept", "hand_in", "hunt", "abandon", "wait" };
            _options.OnEvent("quest_state: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) +
                             " allowed=" + std::to_string(questsAllowed) + " enabled=" + std::to_string(_quests.Enabled()) +
                             " party=" + std::to_string(partyMember) + " low_health=" + std::to_string(lowHealth) + " in_combat=" + std::to_string(bot.in_combat) +
                             " chat_stay=" + std::to_string(state.ChatStay) + " chat_follow=" + std::to_string(state.ChatFollow != 0) +
                             " step=" + kinds[int(questStep.Kind)] + " quest=" + std::to_string(questStep.Quest) +
                             " log=" + std::to_string(bot.quest_count) + " start_index=" + std::to_string(state.Quest.StartIndex) +
                             " memory_done=" + std::to_string(state.Quest.Done.size()) + " memory_log=" + std::to_string(state.Quest.Progress.size()));
        }
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
        bool hasTarget = targetRef != 0 && !lowHealth;
        // The target of this fight is looted once it is dead: remember it, and forget the ones too old to still be on the ground.
        if (hasTarget && forever_module::UnitRefType(targetRef) == 8)
            state.FoughtAtMs[targetRef] = nowMs;
        for (auto it = state.FoughtAtMs.begin(); it != state.FoughtAtMs.end();)
            it = nowMs - it->second > kLootWindowMs ? state.FoughtAtMs.erase(it) : std::next(it);
        for (auto it = state.LootedUntilMs.begin(); it != state.LootedUntilMs.end();)
            it = nowMs >= it->second ? state.LootedUntilMs.erase(it) : std::next(it);
        float const distance = targetUnit ? rotation::DistanceYards(bot, *targetUnit) : bot.target_distance;

        // A fight ends a quest walk's stall timer: the quest block below only runs out of combat, so the timer is reset here.
        if (bot.in_combat != 0)
        {
            state.WalkStillSinceMs = 0;
            state.WalkAnchored = false;
        }

        // Fight watchdog: a visible hostile whose health, and the bot's own, have not gone down for kFightStallMs is unreachable,
        // evading or out of line of sight (the 8-hour soak had a fight of two hours). A threat the snapshot keeps set but that is not
        // in view is the same: telemetry holds the fight open until the guid clears, and the nearby list is all the bot can fight.
        // A gap between snapshots does not restart the timer; the stall continued while this tick was late.
        bool const unseenThreat = hasTarget && targetUnit == nullptr;
        bool const visibleStall = hasTarget && targetUnit && targetUnit->is_hostile && bot.in_combat != 0;
        if (unseenThreat || visibleStall)
        {
            if (state.StallTarget != targetRef)
            {
                state.StallTarget = targetRef;
                state.StallSinceMs = nowMs;
            }
            else if (targetUnit && (targetUnit->health < state.StallTargetHealth || bot.health < state.StallBotHealth))
                state.StallSinceMs = nowMs;
            state.StallLastMs = nowMs;
            if (targetUnit)
            {
                state.StallTargetHealth = targetUnit->health;
                state.StallBotHealth = bot.health;
            }
            if (nowMs - state.StallSinceMs > kFightStallMs)
            {
                AvoidStalledUnit(state.AvoidUntilMs, state.StallCount, targetRef, nowMs);
                if (_options.OnEvent)
                    _options.OnEvent("fight_stalled: bot=" + std::to_string(bot.guid) + " target_entry=" + std::to_string(forever_module::UnitRefEntry(targetRef)) +
                                     " target_hp=" + (targetUnit ? std::to_string(targetUnit->health) : std::string("unseen")) +
                                     " distance=" + std::to_string(int(distance)));
                state.StallTarget = 0;
                // Given up for good this tick too: the bot must not keep attacking it until the next one.
                targetRef = 0;
                targetUnit = nullptr;
                hasTarget = false;
            }
        }
        else
            state.StallTarget = 0;

        Decision wanted;
        if (lowHealth)
        {
            // Below kDisengagePct health: get clear of whatever is close enough to still kill it. In combat that is the threat
            // target; out of combat the closest live hostile is the one to run from, since nothing says yet that it is the target.
            // Standing still to rest used to be the whole reply, and a hostile five yards away killed the bot.
            NearbyUnit const* threat = targetUnit;
            if (!threat)
                threat = Closest(bot, nearby, IsLiveMob);
            float const away = threat ? (threat == targetUnit ? distance : rotation::DistanceYards(bot, *threat))
                                      : std::numeric_limits<float>::max();
            if (threat && away < kFleeYards)
            {
                float const dx = bot.pos_x - threat->pos_x;
                float const dy = bot.pos_y - threat->pos_y;
                float const dist = std::sqrt(dx * dx + dy * dy);
                if (dist > 0.0f)
                {
                    Point flee{ threat->pos_x + dx / dist * kFleeYards,
                                threat->pos_y + dy / dist * kFleeYards,
                                bot.pos_z };
                    wanted = Decision{ ActionType::kMoveTo, 0, flee };
                }
                else
                    wanted = Decision{ ActionType::kStop, 0 };
            }
            else
            {
                wanted = followRef ? Decision{ ActionType::kFollow, followRef } : Decision{ ActionType::kStop, 0 };
            }
        }
        else if (targetRef != 0)
        {
            if (!state.InFight)
            {
                state.InFight = true;
                state.FightStartMs = nowMs;
                // Layout 13: the pet is in the snapshot, so it can be sent at this fight's target. Once per fight, so a
                // long one does not repeat the order. A core that reports no pet never gets this action.
                if (PetOf(bot) == PetKnown::kOut)
                {
                    BotAction petGo;
                    petGo.bot_guid = bot.guid;
                    petGo.type = ActionType::kPetCommand;
                    petGo.spell_id = kPetAttack;
                    petGo.target_guid = targetRef;
                    actions.push_back(petGo);
                    state.PetCommanded = true;
                }
            }
            // A ranged bot that has run out of power closes to melee range: its spells are spent.
            bool const outOfPower = bot.max_power > 0 && std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * kOutOfPowerPct;
            // A hunter with no ammunition cannot shoot (the core refuses every ranged attack): it closes to melee range as well.
            float approach = outOfPower || NoAmmo(bot, classId) ? kMeleeApproachYards : rotation::ApproachYards(classId);
            if (nowMs < state.CloserUntilMs)
                approach = std::min(approach, kMeleeApproachYards + 1.0f);
            if (nowMs < state.RangeCloserUntilMs)
                approach = std::min(approach, approach > kMeleeApproachYards + 0.5f ? kCasterCloserYards : kMeleeCloserYards);
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
                // Kite: a ranged bot backs off when a melee target gets inside kiting distance, then resumes attacking
                // once it is back at range.
                if (approach > kMeleeApproachYards + 0.5f && targetUnit && distance < kRangedKiteYards)
                {
                    float const dx = bot.pos_x - targetUnit->pos_x;
                    float const dy = bot.pos_y - targetUnit->pos_y;
                    float const dist = std::sqrt(dx * dx + dy * dy);
                    if (dist > 0.0f)
                    {
                        Point kite{ targetUnit->pos_x + dx / dist * kRangedKiteYards,
                                    targetUnit->pos_y + dy / dist * kRangedKiteYards,
                                    bot.pos_z };
                        wanted = Decision{ ActionType::kMoveTo, 0, kite };
                    }
                    else
                        wanted = Decision{ ActionType::kAttackTarget, targetRef };
                }
                else
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
            // Layout 13: the fight just ended, so the pet has nothing to hunt. Call it back once; a core that reports no pet
            // never sent it out, so there is nothing to recall.
            if (state.InFight && state.PetCommanded)
            {
                BotAction back;
                back.bot_guid = bot.guid;
                back.type = ActionType::kPetCommand;
                back.spell_id = kPetStay;
                actions.push_back(back);
                state.PetCommanded = false;
            }
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
                // A waypoint the core finds no route to (a spawn on a ledge or off the navmesh, a generated point inside a wall) would hold the whole run for
                // good: after kRouteNoRouteSkip refusals in a row the leader gives that point up and goes on to the next one.
                // A boss point is never skipped: the boss stands between the party and the rest of the dungeon (a locked door opens when it dies, or it guards
                // the way). A boss that cannot be reached stops the run at that point, which the result reports as the furthest point reached.
                bool const bossHere = _options.RouteBossAt.count(state.RouteIndex) != 0 && !_killedBosses.count(_options.RouteBossAt.at(state.RouteIndex));
                if (state.RouteNoRoute >= kRouteNoRouteSkip && bossHere)
                {
                    if (state.RouteNoRoute == kRouteNoRouteSkip && _options.OnEvent)
                        _options.OnEvent("route_blocked: bot=" + std::to_string(bot.guid) + " point=" + std::to_string(state.RouteIndex) + "/" + std::to_string(_options.Route.size()) +
                                         " boss=" + std::to_string(_options.RouteBossAt.at(state.RouteIndex)) + " reason=no_route map=" + std::to_string(bot.map_id) +
                                         " from=" + std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)));
                    state.RouteNoRoute = kRouteNoRouteSkip + 1; // logged once
                }
                else if (state.RouteNoRoute >= kRouteNoRouteSkip)
                {
                    if (_options.OnEvent)
                    {
                        Point const& given = _options.Route[state.RouteIndex];
                        _options.OnEvent("route_skip: bot=" + std::to_string(bot.guid) + " point=" + std::to_string(state.RouteIndex) + "/" + std::to_string(_options.Route.size()) +
                                         " reason=no_route map=" + std::to_string(bot.map_id) + " from=" + std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) + "," +
                                         std::to_string(int(bot.pos_z)) + " to=" + std::to_string(int(given.X)) + "," + std::to_string(int(given.Y)) + "," + std::to_string(int(given.Z)) +
                                         " mounted=" + std::to_string(state.Mounted));
                    }
                    ++state.RouteIndex;
                    state.RouteNoRoute = 0;
                }
                Point const* waypoint = state.RouteIndex < _options.Route.size() ? &_options.Route[state.RouteIndex] : nullptr;
                if (waypoint)
                {
                    float const dx = bot.pos_x - waypoint->X;
                    float const dy = bot.pos_y - waypoint->Y;
                    if (std::sqrt(dx * dx + dy * dy) <= kRoutePointReachYards)
                    {
                        // At a boss point the leader stays until that boss is dead, so the party fights it where it stands (the pull logic takes what is near). A boss that
                        // is not there to be fought, after kBossHoldMs, is given up and the route goes on.
                        auto const boss = _options.RouteBossAt.find(state.RouteIndex);
                        bool hold = boss != _options.RouteBossAt.end() && !_killedBosses.count(boss->second);
                        if (hold)
                        {
                            if (state.RouteBossSinceMs == 0)
                                state.RouteBossSinceMs = nowMs;
                            if (nowMs - state.RouteBossSinceMs >= kBossHoldMs)
                            {
                                if (_options.OnEvent)
                                    _options.OnEvent("boss_missed: bot=" + std::to_string(bot.guid) + " point=" + std::to_string(state.RouteIndex) + " boss=" + std::to_string(boss->second));
                                hold = false;
                            }
                        }
                        if (!hold)
                        {
                            ++state.RouteIndex; // arrived: on to the next point in the same tick
                            state.RouteNoRoute = 0;
                            state.RouteBossSinceMs = 0;
                            waypoint = state.RouteIndex < _options.Route.size() ? &_options.Route[state.RouteIndex] : nullptr;
                        }
                        else
                        {
                            waypoint = nullptr; // stand here and let the party deal with the boss
                            routeBusy = true;   // and do not wander off while idle
                        }
                    }
                }
                if (waypoint && together && strong)
                {
                    wanted = Decision{ ActionType::kMoveTo, 0, *waypoint };
                    routeBusy = true;
                }
            }

            if (questStep.Kind != QuestStepKind::WalkTo)
            {
                state.WalkStillSinceMs = 0; // the stall timer only runs during a quest walk
                state.WalkAnchored = false;
                state.QuestTeleportSent = false;
            }

            switch (questStep.Kind)
            {
                case QuestStepKind::WalkTo:
                {
                    // Stuck on the walk to a quest start's giver. One teleport if QuestTeleport is on; otherwise, or if that teleport was
                    // already sent and the bot is still stuck, the start is dropped and the bot grinds. The planner only offers starts on
                    // the bot's own map, so the giver's position is on that map too.
                    Point const here{ bot.pos_x, bot.pos_y, bot.pos_z };
                    if (!state.WalkAnchored || DistanceTo(bot, state.WalkAnchor) > kQuestWalkStuckYards)
                    {
                        state.WalkAnchor = here;
                        state.WalkStillSinceMs = nowMs;
                        state.WalkAnchored = true;
                        state.QuestTeleportSent = false;
                    }
                    bool const stuck = nowMs - state.WalkStillSinceMs >= kQuestWalkStuckMs;
                    if (stuck && questStep.StartGiver && _options.QuestTeleport && !state.QuestTeleportSent)
                    {
                        wanted = Decision{ ActionType::kTeleport, 0, Point{ questStep.Dest.X, questStep.Dest.Y, questStep.Dest.Z }, bot.map_id };
                        state.QuestTeleportSent = true;
                        state.WalkStillSinceMs = nowMs;
                        if (_options.OnEvent)
                            _options.OnEvent("quest_walk_forced: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) +
                                             " quest=" + std::to_string(questStep.Quest) + " from=" + std::to_string(int(here.X)) + "," +
                                             std::to_string(int(here.Y)) + "," + std::to_string(int(here.Z)) + " to=" +
                                             std::to_string(int(questStep.Dest.X)) + "," + std::to_string(int(questStep.Dest.Y)) + "," +
                                             std::to_string(int(questStep.Dest.Z)));
                    }
                    else if (stuck && questStep.StartGiver)
                    {
                        _quests.NoteNoRoute(state.Quest, nowMs);
                        state.QuestTeleportSent = false;
                        state.WalkStillSinceMs = 0;
                        state.WalkAnchored = false;
                        if (_options.OnEvent)
                            _options.OnEvent("quest_walk_blocked: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) +
                                             " quest=" + std::to_string(questStep.Quest) + " reason=" +
                                             (_options.QuestTeleport ? "teleport_stuck" : "teleport_off"));
                        questStep = {};
                        wanted = Decision{ ActionType::kStop, 0 };
                    }
                    else
                        wanted = Decision{ ActionType::kMoveTo, 0, Point{ questStep.Dest.X, questStep.Dest.Y, questStep.Dest.Z } };
                    break;
                }
                case QuestStepKind::Hunt:
                    // Hunt with the target area equal to the bot's own place (no areas known): stand and wait for the creatures.
                    if (questStep.Dest.X != bot.pos_x || questStep.Dest.Y != bot.pos_y)
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

        // An enemy near keeps the bot from idling, which keeps it from walking to grind spots. Only one the bot would actually pull counts: a bot
        // whose start zone shows only elites, packs of four or more, or creatures well above its level used to stand there for the whole run
        // (all 73 Undead and many Night Elf and Tauren bots of the 8-hour soak), neither fighting nor leaving. A bot that holds position by
        // order (a party waiting in a dungeon, no self pull) still counts any live creature.
        bool enemyNear = false;
        if (dungeonHold || noSelfPull)
        {
            for (NearbyUnit const& unit : nearby)
                enemyNear = enemyNear || (IsLiveMob(unit) && (huntEntries.empty() ? !IsGrey(bot, unit) : true));
        }
        else
            enemyNear = PullTarget(bot, nearby, avoided, false, huntEntries, _routeActive) != nullptr;
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
        bool const urgentService = state.ServiceUrgent && !hasTarget && bot.in_combat == 0 && !lowHealth && !state.InFight;

        // Supplies (layout 15, supply_policy.h). A trip made earlier is judged once the core has counted again: more pieces than before is a
        // success; otherwise the vendor had nothing the bot could buy (or the bot never reached it) and the next trip waits longer.
        supply::Facts const supplyFacts = FactsOf(bot);
        if (state.SupplyCheckAtMs != 0 && nowMs >= state.SupplyCheckAtMs)
        {
            state.SupplyCheckAtMs = 0;
            bool const improved = supply::Improved(state.SupplyAsked, state.SupplyBefore, supplyFacts);
            state.SupplyFailures = improved ? 0 : state.SupplyFailures + 1;
            if (!improved)
                state.NextSupplyMs = nowMs + supply::BackoffMs(_options.Supplies, state.SupplyFailures);
            if (_options.OnEvent)
                _options.OnEvent(std::string(improved ? "supply_trip_done" : "supply_trip_failed") + ": bot=" + std::to_string(bot.guid) +
                                 " ammo=" + std::to_string(supplyFacts.AmmoCount) + " food=" + std::to_string(supplyFacts.FoodCount) +
                                 " drink=" + std::to_string(supplyFacts.DrinkCount) + " hpot=" + std::to_string(supplyFacts.HealthPotions) +
                                 " mpot=" + std::to_string(supplyFacts.ManaPotions) + " bandage=" + std::to_string(supplyFacts.Bandages) +
                                 " failures=" + std::to_string(state.SupplyFailures));
        }
        // A party member follows its leader, and a bot held by a chat order stays where it was told: neither goes shopping.
        supply::Needs const supplyNeeds = (partyMember || state.ChatStay || state.ChatFollow != 0) ? supply::Needs{}
                                                                                                    : supply::Assess(_options.Supplies, supplyFacts);
        supply::Stop const supplyStop = supply::NextStop(supplyNeeds);
        bool const supplyDue = supplyStop != supply::Stop::None && nowMs >= state.NextSupplyMs && state.SupplyCheckAtMs == 0;
        bool const serviceDue = _options.ServiceEveryMs != 0 && nowMs >= state.NextServiceMs;
        bool const shopping = (idle || urgentService) && !state.ChatStay && (serviceDue || supplyDue);
        if (!shopping && state.Traveling)
            state.Traveling = false; // a walk to a vendor cut short (a fight, an order) starts its timer again next time

        // Services and travel (M6). An idle bot due a vendor visit walks to the nearest vendor on its map, unless it is a
        // party member (it follows its leader). The visit is sent once the bot is in reach. A walk longer than
        // kTravelTimeoutMs is dropped and the visit waits for its next interval. A walking bot is not idle, so it neither
        // sits nor trains on the way. A bot short of ammunition, food or drink walks to a vendor that sells it instead (supplies).
        if (shopping)
        {
            // The nearest vendor on the bot's map, found by the core (layout 6); the nearest repairman instead when the gear is worn
            // (layout 9). None on the map, or a party member: no walk.
            bool const needRepair = bot.repair_valid != 0 && bot.durability_pct < kRepairBelowPct;
            // Worn gear first: the repairman is the stop then, and the supplies wait for the next trip (a few minutes).
            bool const supplyStopNow = supplyDue && !needRepair;
            bool const ammoStop = supplyStopNow && supplyStop == supply::Stop::Ammo;
            bool const goodsStop = supplyStopNow && supplyStop == supply::Stop::Goods;
            bool const reagentStop = supplyStopNow && supplyStop == supply::Stop::Reagent;
            Point const vendor = supplyStopNow ? (ammoStop      ? Point{ bot.ammo_vendor_x, bot.ammo_vendor_y, bot.ammo_vendor_z }
                                                  : goodsStop   ? Point{ bot.goods_vendor_x, bot.goods_vendor_y, bot.goods_vendor_z }
                                                  : reagentStop ? Point{ bot.reagent_vendor_x, bot.reagent_vendor_y, bot.reagent_vendor_z }
                                                                : Point{ bot.food_vendor_x, bot.food_vendor_y, bot.food_vendor_z })
                                 : needRepair  ? Point{ bot.repair_x, bot.repair_y, bot.repair_z }
                                               : Point{ bot.vendor_x, bot.vendor_y, bot.vendor_z };
            bool const vendorExists = supplyStopNow || needRepair || bot.vendor_valid != 0;
            bool const hasVendor = !partyMember && vendorExists;
            bool const inReach = vendorExists && DistanceTo(bot, vendor) <= kVendorReachYards;
            if (hasVendor && !inReach)
            {
                if (!state.Traveling)
                {
                    state.Traveling = true;
                    state.TravelStartMs = nowMs;
                    state.SupplyTrip = supplyStopNow;
                }
                if (nowMs - state.TravelStartMs <= kTravelTimeoutMs)
                {
                    wanted = Decision{ ActionType::kMoveTo, 0, vendor };
                    idle = false;
                }
                else
                {
                    state.Traveling = false;
                    state.ServiceUrgent = false;
                    if (state.SupplyTrip)
                    {
                        // The walk never got there: a trip that bought nothing.
                        state.SupplyTrip = false;
                        state.NextSupplyMs = nowMs + supply::BackoffMs(_options.Supplies, ++state.SupplyFailures);
                    }
                    else
                        state.NextServiceMs = nowMs + _options.ServiceEveryMs;
                }
            }
            else if (inReach || !vendorExists)
            {
                // In reach of a vendor, or none on the map at all: the visit is tried where the bot stands and the core refuses it when
                // no NPC is in range. What is left is a party member with a vendor it may not walk to, and it is sent nothing: it follows
                // its leader, and an action it will have refused still spent a slot of the tick's action budget (kRequestedActions is 2 a
                // bot, and PublishActions drops the whole tick when the list runs over) and pushed the next visit a whole ServiceEveryMs
                // out. At the 1500-bot ceiling that is 1200 refused actions on one tick, over the budget the core granted.
                state.Traveling = false;
                state.SupplyTrip = false;
                if (state.ServiceUrgent)
                {
                    state.ServiceUrgent = false;
                    _quests.NoteServiced(state.Quest); // the bags are emptier: the refused hand-ins are tried again
                }
                BotAction vendor;
                vendor.bot_guid = bot.guid;
                vendor.type = ActionType::kVendor;
                vendor.spell_id = kServiceSell | kServiceRepair | kServiceEquip;
                // Layout 13: the NPC's guid, not just its position. An older core leaves it 0 and the core resolves the
                // nearest vendor of the map instead.
                vendor.target_guid = needRepair ? bot.repair_guid : bot.vendor_guid;
                if (supplyStopNow)
                {
                    // Layout 15: buy what is short. The core picks the items from this vendor's stock and buys up to the counts given here.
                    supply::Request const request = supply::RequestFor(_options.Supplies, supplyNeeds);
                    vendor.target_guid = ammoStop ? bot.ammo_vendor_guid : goodsStop ? bot.goods_vendor_guid
                                       : reagentStop ? bot.reagent_vendor_guid : bot.food_vendor_guid;
                    if (request.Ammo != 0)
                        vendor.spell_id |= kServiceBuyAmmo;
                    if (request.Food != 0)
                        vendor.spell_id |= kServiceBuyFood;
                    if (request.Drink != 0)
                        vendor.spell_id |= kServiceBuyDrink;
                    if (request.HealthPotion)
                        vendor.spell_id |= kServiceBuyHealthPotion;
                    if (request.ManaPotion)
                        vendor.spell_id |= kServiceBuyManaPotion;
                    if (request.Bandage)
                        vendor.spell_id |= kServiceBuyBandage;
                    if (request.Reagent)
                        vendor.spell_id |= kServiceBuyReagent;
                    vendor.dest_x = float(request.Ammo);
                    vendor.dest_y = float(request.Food);
                    vendor.dest_z = float(request.Drink);
                    state.NextSupplyMs = nowMs + _options.Supplies.TripEveryMs;
                    state.SupplyCheckAtMs = nowMs + kSupplyJudgeAfterMs;
                    state.SupplyAsked = supplyNeeds;
                    state.SupplyBefore = supplyFacts;
                    if (_options.OnEvent)
                        _options.OnEvent("supply_trip: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " ammo=" +
                                         std::to_string(supplyFacts.AmmoCount) + " food=" + std::to_string(supplyFacts.FoodCount) + " drink=" +
                                         std::to_string(supplyFacts.DrinkCount) + " hpot=" + std::to_string(supplyFacts.HealthPotions) + " mpot=" +
                                         std::to_string(supplyFacts.ManaPotions) + " bandage=" + std::to_string(supplyFacts.Bandages) +
                                         " copper=" + std::to_string(bot.copper));
                }
                actions.push_back(vendor);
                if (!supplyStopNow || serviceDue)
                    state.NextServiceMs = nowMs + _options.ServiceEveryMs;
            }
        }

        // Hubs (M7): a bot that has been idle for kWanderIdleMs walks to the next hub of its map, and waits there until the
        // next walk. A hurt bot rests instead, and a party member follows its leader. Anything that comes near stops the walk.
        // Mana (the soak's hunter spent most of its time at a third of its mana, because only health made a bot sit): an idle mana user
        // below kRestManaBelowPct sits and drinks until kRestManaUntilPct, at most kManaRestMaxMs at a time (and not again for a while
        // when that was not enough: mana that does not come back is not waited for). Anything else the bot does ends the rest.
        bool const manaUser = bot.power_type == 0 && bot.max_power > 0;
        bool manaResting = false;
        if (idle && manaUser)
        {
            if (state.ManaRestSinceMs == 0 && nowMs >= state.ManaRestBlockedUntilMs &&
                std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * kRestManaBelowPct)
                state.ManaRestSinceMs = nowMs;
            if (state.ManaRestSinceMs != 0)
            {
                if (std::uint64_t(bot.power) * 100 >= std::uint64_t(bot.max_power) * kRestManaUntilPct)
                    state.ManaRestSinceMs = 0;
                else if (nowMs - state.ManaRestSinceMs > kManaRestMaxMs)
                {
                    state.ManaRestSinceMs = 0;
                    state.ManaRestBlockedUntilMs = nowMs + kManaRestBlockMs;
                }
                else
                    manaResting = true;
            }
        }
        else
            state.ManaRestSinceMs = 0;
        bool const healthy = !manaResting && (bot.max_health == 0 || std::uint64_t(bot.health) * 100 >= std::uint64_t(bot.max_health) * kRestBelowPct);
        auto const hubs = _options.HubsByMap.find(bot.map_id);
        bool const grindMode = _options.Grind && !_options.Grind->Spots.empty() && bot.level != 0;
        if (idle && healthy && _options.WanderWhenIdle && grindMode && nowMs >= state.GrindRetryAtMs && !partyMember && !state.ChatStay && state.ChatFollow == 0)
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
                {
                    // Nothing on this map for this level (a start zone the spot file does not cover, such as the new races' island): the bot
                    // explores on foot for a minute, as a bot without grind data does, and then looks again. Without this it stood at its
                    // start for the whole run.
                    state.IdleSinceMs = nowMs;
                    state.GrindRetryAtMs = nowMs + kGrindRetryMs;
                }
            }
        }
        else if (idle && healthy && _options.WanderWhenIdle && !partyMember && !state.ChatStay && state.ChatFollow == 0 && hubs != _options.HubsByMap.end() && !hubs->second.empty() &&
                 state.HubFailures < kHubFailuresMax)
        {
            std::vector<Point> const& list = hubs->second;
            if (state.Wandering)
            {
                Point const& hub = list[state.NextHub % list.size()];
                bool const arrived = DistanceTo(bot, hub) <= kHubReachYards;
                bool const timedOut = nowMs - state.WanderStartMs > kWanderTimeoutMs;
                if (arrived || timedOut)
                {
                    // Next time, the next hub. An arrival waits a full idle interval, as before. A timeout does not: a hub in rock or in
                    // another zone must not cost the bot 30 s of standing every time it goes round the list, so it walks on after a short
                    // pause. Walks that end without arriving are counted: after kHubFailuresMax in a row the bot explores on foot instead.
                    state.Wandering = false;
                    state.HubFailures = arrived ? 0 : state.HubFailures + 1;
                    state.NextHub = (state.NextHub + 1) % list.size();
                    state.IdleSinceMs = arrived ? nowMs : nowMs - (kWanderIdleMs - kHubRetryIdleMs);
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

        // Exploring on foot: a bot with no grind spot and no hub it can reach (a map with neither, or hubs it never got to) would stand where it
        // began and never meet a creature, as 45 bots did for eight hours in the soak. After a short quiet it takes a walk of 50 to 100 yards in a
        // turning direction, and after a few walks tries its hubs again.
        else if (idle && healthy && _options.WanderWhenIdle && (!grindMode || nowMs < state.GrindRetryAtMs) && !partyMember && !state.ChatStay && state.ChatFollow == 0)
        {
            if (state.Wandering)
            {
                bool const arrived = DistanceTo(bot, state.ExploreTarget) <= kExploreReachYards;
                bool const timedOut = nowMs - state.WanderStartMs > kExploreTimeoutMs;
                if (arrived || timedOut)
                {
                    state.Wandering = false;
                    state.IdleSinceMs = nowMs; // the next walk starts after kExploreIdleMs
                    if (state.ExploreStep % 8 == 0)
                        state.HubFailures = 0; // try the hubs again now and then
                }
                else
                {
                    wanted = Decision{ ActionType::kMoveTo, 0, state.ExploreTarget };
                    idle = false;
                }
            }
            else if (nowMs - state.IdleSinceMs >= kExploreIdleMs)
            {
                float const angle = float((bot.guid * 2654435761ull) % 628) * 0.01f + float(state.ExploreStep) * 2.399963f;
                float const radius = 50.0f + 25.0f * float(state.ExploreStep % 3);
                ++state.ExploreStep;
                state.ExploreTarget = Point{ bot.pos_x + std::cos(angle) * radius, bot.pos_y + std::sin(angle) * radius, bot.pos_z };
                state.Wandering = true;
                state.WanderStartMs = nowMs;
                wanted = Decision{ ActionType::kMoveTo, 0, state.ExploreTarget };
                idle = false;
            }
        }

        // A bot that stood on the same spot for a whole check interval, not fighting, says why in one event line (stuck_idle). The 73 Undead bots
        // of the soak did this for eight hours and nothing in the logs said what held them; this line shows which condition did.
        if (_options.OnEvent && nowMs - state.DiagAtMs >= kStuckCheckMs)
        {
            float const moved = std::hypot(bot.pos_x - state.DiagX, bot.pos_y - state.DiagY);
            if (state.DiagAtMs != 0 && moved < 5.0f && bot.in_combat == 0 && !state.Sitting)
            {
                std::size_t hostiles = 0;
                for (NearbyUnit const& unit : nearby)
                    if (IsLiveMob(unit))
                        ++hostiles;
                _options.OnEvent("stuck_idle: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) +
                                 " idle=" + std::to_string(idle) + " hasTarget=" + std::to_string(hasTarget) + " enemyNear=" + std::to_string(enemyNear) +
                                 " wanted=" + std::to_string(int(wanted.Type)) + " quest=" + std::to_string(int(questStep.Kind)) + " route=" + std::to_string(routeBusy) +
                                 " healthy=" + std::to_string(healthy) + " mana=" + std::to_string(manaResting) + " grind=" + std::to_string(grindMode) +
                                 " wandering=" + std::to_string(state.Wandering) + " traveling=" + std::to_string(state.Traveling) + " party=" + std::to_string(partyMember) +
                                 " stay=" + std::to_string(state.ChatStay) + " liveHostiles=" + std::to_string(hostiles) + " nearby=" + std::to_string(nearby.size()) +
                                 " idleForS=" + std::to_string(state.Idling ? (nowMs - state.IdleSinceMs) / 1000 : 0));
            }
            state.DiagAtMs = nowMs;
            state.DiagX = bot.pos_x;
            state.DiagY = bot.pos_y;
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
        bool const manaLow = holdIdle && manaUser &&
                             std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * (state.Sitting ? kPartyReadySitHealthPct : kPartyReadyPowerPct);
        bool const sitWanted = (idle || holdIdle) && (state.Sitting ? (stillHurt || manaLow || manaResting) : (hurtEnough || manaLow || manaResting)) &&
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

        // Eat or drink while resting (layout 15): a bot sat down because it is hurt or low on mana uses food or drink it carries, which restores
        // far faster than sitting alone. Not while moving or fighting, and not again until the last piece has done its work.
        if (_options.Supplies.Enabled && state.Sitting && sitWanted && bot.moving == 0 && bot.in_combat == 0 && nowMs >= state.NextConsumeMs)
        {
            std::uint32_t const healthPct = bot.max_health > 0 ? std::uint32_t(std::uint64_t(bot.health) * 100 / bot.max_health) : 100;
            std::uint32_t const manaPct = manaUser && bot.max_power > 0 ? std::uint32_t(std::uint64_t(bot.power) * 100 / bot.max_power) : 100;
            std::uint32_t what = supply::ConsumeNow(_options.Supplies, supplyFacts, healthPct, manaPct);
            if (what == 0)
                what = supply::BandageNow(_options.Supplies, supplyFacts, healthPct); // layout 16: no food to eat, so a bandage
            if (what != 0)
            {
                BotAction eat;
                eat.bot_guid = bot.guid;
                eat.type = ActionType::kConsume;
                eat.spell_id = what;
                actions.push_back(eat);
                state.NextConsumeMs = nowMs + (what == 5 ? _options.Supplies.BandageEveryMs : _options.Supplies.ConsumeEveryMs);
            }
        }

        // Potions (layout 16): a bot in a fight that is badly hurt, or a mana user nearly out of mana, drinks one. It costs no global cooldown and
        // no standing still, so it goes alongside whatever the bot does; the game's shared potion cooldown is why it is not asked for sooner.
        if (_options.Supplies.Enabled && _options.Supplies.Potions && bot.in_combat != 0 && nowMs >= state.NextPotionMs)
        {
            std::uint32_t const healthPct = bot.max_health > 0 ? std::uint32_t(std::uint64_t(bot.health) * 100 / bot.max_health) : 100;
            std::uint32_t const manaPct = manaUser && bot.max_power > 0 ? std::uint32_t(std::uint64_t(bot.power) * 100 / bot.max_power) : 100;
            if (std::uint32_t const which = supply::PotionNow(_options.Supplies, supplyFacts, healthPct, manaPct))
            {
                BotAction potion;
                potion.bot_guid = bot.guid;
                potion.type = ActionType::kConsume;
                potion.spell_id = which;
                actions.push_back(potion);
                state.NextPotionMs = nowMs + _options.Supplies.PotionEveryMs;
            }
        }

        bool const levelUp = bot.level != 0 && bot.level != state.LastTrainLevel;
        // A sitting bot (resting, or drinking) is not available for a trainer check, exactly as a walking bot is not: it stands first.
        if (idle && !state.Sitting && _options.TrainEveryMs != 0 && (levelUp || nowMs >= state.NextTrainMs))
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
                if (!state.FoughtAtMs.count(unit.guid) || rotation::DistanceYards3d(bot, unit) > kLootRangeYards)
                    continue;
                // A groupmate already took this corpse (the core loots all of it for the group): nothing is left for this bot.
                std::uint64_t const lootGroup = bot.group_leader_guid != 0 ? bot.group_leader_guid : bot.guid;
                if (auto const claim = _lootClaims.find(unit.guid); claim != _lootClaims.end() && nowMs < claim->second.UntilMs)
                {
                    if (claim->second.Group == lootGroup)
                    {
                        state.FoughtAtMs.erase(unit.guid);
                        continue;
                    }
                }
                else if (_lootClaims.size() > 512)
                    for (auto it = _lootClaims.begin(); it != _lootClaims.end();)
                        it = nowMs >= it->second.UntilMs ? _lootClaims.erase(it) : std::next(it);
                _lootClaims[unit.guid] = LootClaim{ lootGroup, nowMs + kLootWindowMs };

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

        // Flight points (layout 17): a bot on a flight takes no walking orders; a long walk of a bot that is on its own is replaced by a walk to the nearest
        // flight master and a flight from it (taxi_policy.h). The core refuses a flight that would not bring the bot closer, and the bot then walks.
        if (_options.Taxi.Enabled && wanted.Type == ActionType::kMoveTo && !partyMember && !partyLeaderBot)
        {
            taxi::Facts facts;
            facts.X = bot.pos_x;
            facts.Y = bot.pos_y;
            facts.InFlight = bot.in_flight != 0;
            facts.InCombat = bot.in_combat != 0;
            facts.Alive = bot.life_state == 0;
            facts.MasterValid = bot.taxi_master_valid != 0;
            facts.MasterX = bot.taxi_master_x;
            facts.MasterY = bot.taxi_master_y;
            switch (taxi::Decide(_options.Taxi, facts, wanted.Dest.X, wanted.Dest.Y, nowMs, state.TaxiBlockedUntilMs, state.TaxiWaitUntilMs))
            {
                case taxi::Move::Walk:
                    break;
                case taxi::Move::WalkToMaster:
                    wanted.Dest = Point{ bot.taxi_master_x, bot.taxi_master_y, bot.taxi_master_z };
                    break;
                case taxi::Move::Fly:
                {
                    BotAction fly;
                    fly.bot_guid = bot.guid;
                    fly.type = ActionType::kTaxi;
                    fly.target_guid = bot.taxi_master_guid;
                    fly.spell_id = bot.map_id;
                    fly.dest_x = wanted.Dest.X;
                    fly.dest_y = wanted.Dest.Y;
                    fly.dest_z = wanted.Dest.Z;
                    actions.push_back(fly);
                    state.TaxiWaitUntilMs = nowMs + _options.Taxi.WaitMs;
                    if (_options.OnEvent)
                        _options.OnEvent("taxi: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) +
                                         " walk=" + std::to_string(int(std::hypot(bot.pos_x - wanted.Dest.X, bot.pos_y - wanted.Dest.Y))));
                    [[fallthrough]];
                }
                case taxi::Move::Wait:
                    if (state.Last)
                        wanted = *state.Last; // nothing new is sent while the flight starts or runs
                    else
                        wanted = Decision{};
                    break;
            }
        }
        else if (bot.in_flight != 0 && state.Last)
            wanted = *state.Last; // on a flight with nothing to walk to: the stop order would be a new order too

        // Mount for long travel, dismount when the walk is short, combat starts, or the bot arrives.
        if (bot.in_combat == 0 && bot.life_state == 0 && bot.casting_spell == 0 && nowMs >= state.MountRetryMs)
        {
            if (wanted.Type == ActionType::kMoveTo && !state.Mounted)
            {
                float const destDist = DistanceTo(bot, wanted.Dest);
                if (destDist > kMountMinDistanceYards)
                {
                    BotAction mount;
                    mount.bot_guid = bot.guid;
                    mount.type = ActionType::kMount;
                    mount.spell_id = 1; // the core interprets 1 as mount up
                    actions.push_back(mount);
                    state.Mounted = true;
                    state.MountRetryMs = nowMs + 5000;
                    wanted.Type = ActionType::kStop; // stop moving while the mount cast completes
                    wanted.Target = 0;
                }
            }
            else if (state.Mounted &&
                     (wanted.Type != ActionType::kMoveTo ||
                      DistanceTo(bot, wanted.Dest) <= kMountDismountDistanceYards))
            {
                BotAction dismount;
                dismount.bot_guid = bot.guid;
                dismount.type = ActionType::kMount;
                dismount.spell_id = 0; // 0 = dismount
                actions.push_back(dismount);
                state.Mounted = false;
                state.MountRetryMs = nowMs + 5000;
            }
        }

        if (!state.Last || state.Last->Type != wanted.Type || state.Last->Target != wanted.Target ||
            state.Last->Dest.X != wanted.Dest.X || state.Last->Dest.Y != wanted.Dest.Y || state.Last->Dest.Z != wanted.Dest.Z)
        {
            BotAction action;
            action.bot_guid = bot.guid;
            action.type = wanted.Type;
            action.target_guid = wanted.Target;
            action.spell_id = wanted.Type == ActionType::kTeleport ? wanted.Map : 0;
            action.dest_x = wanted.Dest.X;
            action.dest_y = wanted.Dest.Y;
            action.dest_z = wanted.Dest.Z;
            actions.push_back(action);
            state.Last = wanted;
        }

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
        if (partyLeader != 0 && nowMs >= state.NextCastMs && !(IsCasting(bot)))
        {
            std::uint32_t const healSpell = state.Role_ == BotState::Role::Dps ? 0u : HealSpellFor(classId, bot.level);
            auto const retry = healSpell != 0 ? state.RetryAtMs.find(healSpell) : state.RetryAtMs.end();
            if (healSpell != 0 && (retry == state.RetryAtMs.end() || nowMs >= retry->second))
            {
                std::uint64_t const healGroup = partyLeader != 0 ? partyLeader : bot.guid;
                if (_healClaims.size() > 64)
                    for (auto it = _healClaims.begin(); it != _healClaims.end();)
                        it = nowMs >= it->second.UntilMs ? _healClaims.erase(it) : std::next(it);
                BotSnapshot const* hurt = nullptr;
                std::uint64_t hurtPct = kHealBelowPct;
                BotSnapshot const* leaderHurt = nullptr;   // the tank with a target: healed first, whatever its share
                std::uint64_t leaderHurtPct = kTankHealBelowPct;
                auto const consider = [&](BotSnapshot const& member)
                {
                    if (member.life_state != 0 || member.max_health == 0 || Distance2d(bot, member) > kHealRangeYards)
                        return;
                    // Another healer of this party already has this member: leave it, someone else needs the heal more.
                    if (auto const claim = _healClaims.find(member.guid); claim != _healClaims.end() &&
                        nowMs < claim->second.UntilMs && claim->second.Group == healGroup)
                        return;
                    std::uint64_t const pct = std::uint64_t(member.health) * 100 / member.max_health;
                    if (member.guid == partyLeader && member.threat_target_guid != 0)
                    {
                        if (pct < leaderHurtPct)
                        {
                            leaderHurtPct = pct;
                            leaderHurt = &member;
                        }
                        return;
                    }
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
                if (leaderHurt) // the tank is kept alive first while it holds a target
                {
                    hurt = leaderHurt;
                    hurtPct = leaderHurtPct;
                }
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
                        _healClaims[hurt->guid] = HealClaim{ healGroup, nowMs + kHealClaimMs };
                        if (_options.OnEvent)
                            _options.OnEvent("heal: healer=" + std::to_string(bot.guid) + " spell=" + std::to_string(healSpell) +
                                             " target=" + std::to_string(hurt->guid) + " target_hp_pct=" + std::to_string(hurtPct) +
                                             (hurt->guid == partyLeader ? " tank" : ""));
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
                    continue;
                }
            }
        }

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
                    // The bot's own buff is read from the snapshot's auras (layout 13): a buff another player cast over it,
                    // or one it lost to a death, is now seen as the fact it is. A party member's buff is not in this bot's
                    // snapshot, so its timer and the party claim carry it.
                    if (recipient == 0)
                    {
                        if (AuraRunning(bot, row.Spell))
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
            if (cast)
                continue;
        }

        if (!hasTarget)
        {
            state.AutoShotTarget = 0; // the fight is over: Auto Shot stopped with it
            continue;
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
            continue;
        }

        float const castDistance = targetUnit ? rotation::DistanceYards3d(bot, *targetUnit) : distance;

        // Class rotation: the first row that is off cooldown, in range and allowed by its rule. One cast per global cooldown.
        if (nowMs < state.NextCastMs)
            continue;
        // A cast or channel in progress finishes first: no cast can start meanwhile.
        if (IsCasting(bot))
            continue;
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
                continue;
            }
        }

        for (rotation::Row const& row : RowsFor(classId))
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
            if (row.OnTarget && castDistance < row.MinRange)
                continue;
            if (row.OnTarget && row.MaxRange > 0.0f && castDistance > (row.MaxRange > kRangeSlackYards * 2.0f ? row.MaxRange - kRangeSlackYards : row.MaxRange))
                continue;
            if (!rotation::Allowed(row.When, facts))
                continue;
            // Only one member of the party stops a cast: when a party member is already casting an interrupt, this bot uses its next row.
            if (row.When.Condition == rotation::Cond::TargetCasting && interruptTaken(bot, partyLeader))
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
