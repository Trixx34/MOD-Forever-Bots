// The party planner (Phase 2 of the 2026-11-10 bot AI plan): a plan for one party, made once per interval from the whole party's view, instead
// of each bot taking the first free job in guid order. Plain structs and free functions, no SDK dependency, unit-tested with hand-built input
// (tests/party_planner_test.cpp). The Turn* steps read the plan first and fall back to their own scan when there is none, so a party with the
// planner off (Module.forever-bots.PartyPlanner = 0; it is on by default) behaves as before.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace party_planner {

struct Settings
{
    bool Enabled = false;
    std::uint64_t PlanIntervalMs = 400;     // a party's plan is recomputed at most this often
    std::uint64_t BackupDelayMs = 700;      // a backup kicker may fire only once the cast has been in view this long without a kick
    std::size_t MaxCcAssignments = 2;
    std::uint32_t HealBelowPct = 60;        // a member at or above this share needs no heal (the caller sets the healers' own threshold)
    std::uint32_t EmergencyPct = 20;        // below this share a member comes before the tank
    std::uint32_t TankHealBelowPct = 55;    // the tank (or leader) in combat is healed below this share
    float HealRangeYards = 30.0f;
    float BuffRangeYards = 30.0f;           // a party buff reaches members this close
    float PurgeRangeYards = 30.0f;          // a purge, and the polymorph of a mind controlled member, reach this far
    float SpreadMaxMoveYards = 15.0f;       // a spread nudge moves a member at most this far from where it stands
};

// One party member as the planner sees it.
struct Member
{
    std::uint64_t Guid = 0;
    bool Alive = true;
    bool Tank = false;
    float X = 0.0f, Y = 0.0f;
    bool CanInterrupt = false;              // the class has an interrupt row
    bool InterruptReady = true;             // not on cooldown (the caller's NextCast / retry state)
    std::uint32_t CcSpell = 0;              // the class's crowd control spell for its level, 0 for none
    bool CcReady = true;
    std::uint64_t Holding = 0;              // the unit it holds right now (its crowd control still running), 0 for none
    bool CanHeal = false;                   // a healer: a heal spell for its level and not playing the tank or damage role
    std::uint32_t HealPct = 100;            // its health share
    bool InCombat = false;
    bool Leader = false;                    // the party leader, who tanks in a dungeon run (the healers' "tank")
    // Raid rules (raid_rules.h). DispelKinds: bit k set when the member can dispel the kind k (1 magic, 2 curse, 3 disease, 4 poison, the SDK's dispel_type values).
    std::uint32_t DispelKinds = 0;
    bool DispelReady = true;
    struct Debuff
    {
        std::uint32_t Spell = 0;
        std::uint8_t Kind = 0;              // the dispel kind the rule table gives the aura
    };
    std::vector<Debuff> Debuffs;            // the member's auras that the rule table wants dispelled (the caller filters)
    std::vector<std::uint32_t> BuffSpells;  // the party buff spells (upkeep rows) the member can cast at its level
    std::uint32_t SwapStacks = 0;           // stacks of the rule table's tank swap aura on the member
    std::uint32_t SwapAt = 0;               // the stack count at which a tank holding it is swapped, 0 for no swap rule in this fight
    // Raid rules, spacing and aura jobs (raid_rules.h rows spread, purge, mindcontrol).
    std::uint8_t SpreadKind = 0;            // the spread group of the member (raid_rules::kWhoMelee, kWhoRanged, kWhoHealers), 0 for one that is never spread (a tank, an unknown role)
    bool Movable = true;                    // false for a human (the plan cannot move it; its position still counts as taken)
    std::uint32_t PurgeSpell = 0;           // the spell that removes a magic buff from an enemy (priest Dispel Magic, shaman Purge), 0 for none
    std::uint32_t ControlCcSpell = 0;       // the spell that holds a mind controlled member (a mage's Polymorph), 0 for none
    bool ControlReady = true;               // ControlCcSpell is not on cooldown or retry
    bool Controlled = false;                // carries an aura of a mindcontrol row
};

// One spread rule in force: members of the groups in `Who` keep `Yards` apart.
struct SpreadRule
{
    float Yards = 0.0f;
    std::uint8_t Who = 7;
};

// One hostile unit in the party's merged view.
struct Enemy
{
    std::uint64_t Ref = 0;
    float X = 0.0f, Y = 0.0f;
    bool Casting = false;
    bool Engaged = false;                   // already fought by the party
    bool Protected = false;                 // never held: the dungeon's kill priority, a boss
    bool InPack = false;                    // in the tank's pack (the tank's target or near it): a candidate for the shared kill target
    std::uint32_t KillRank = 1000;          // the dungeon file's kill priority (lower first), 1000 for an unlisted unit
    bool InterruptFirst = false;            // its cast is one the raid rules list to interrupt before any other cast
    bool NoAoe = false;                     // the raid rules say nobody uses area damage while it fights
    std::uint64_t Health = 0;
    std::vector<SpreadRule> Spread;         // the spread rules of the raid rules that this engaged enemy puts in force
    std::uint32_t PurgeAura = 0;            // an aura of a purge row that a member's target_auras shows on this enemy, 0 for none
    std::vector<std::uint32_t> CcSpells;    // the crowd control spells that may hold it (creature type, not a boss, not the kill priority, in the tank's pack)
};

struct InterruptAssignment
{
    std::uint64_t Bot = 0;
    std::uint64_t Target = 0;
    std::uint8_t Rank = 0;                  // 0 first choice, 1 backup
};

struct HealAssignment
{
    std::uint64_t Bot = 0;                  // the healer
    std::uint64_t Target = 0;               // the member it heals
};

struct DispelAssignment
{
    std::uint64_t Bot = 0;                  // the dispeller
    std::uint64_t Target = 0;               // the member with the debuff
    std::uint32_t Aura = 0;
    std::uint8_t Kind = 0;
};

struct BuffAssignment
{
    std::uint64_t Bot = 0;                  // the caster
    std::uint64_t Recipient = 0;            // the member (a player guid, not a unit reference)
    std::uint32_t Spell = 0;
};

struct CcAssignment
{
    std::uint64_t Bot = 0;
    std::uint64_t Target = 0;
};

struct SpreadAssignment
{
    std::uint64_t Bot = 0;
    float X = 0.0f, Y = 0.0f;               // where the member goes so that it stands clear of the others
};

struct PurgeAssignment
{
    std::uint64_t Bot = 0;
    std::uint64_t Target = 0;               // the enemy unit reference
    std::uint32_t Aura = 0;
    std::uint32_t Spell = 0;
    std::uint8_t Rank = 0;                  // 0 first choice, 1 backup (fires once the aura has been seen for BackupDelayMs)
};

struct ControlAssignment
{
    std::uint64_t Bot = 0;
    std::uint64_t Target = 0;               // the mind controlled member's guid (the module's bot counter)
    std::uint32_t Spell = 0;
    std::uint8_t Rank = 0;
};

// One party's plan, kept across ticks under its leader.
struct Order
{
    std::uint64_t MainTank = 0;             // kept while the bot is alive and a member; otherwise the first tank. A tank holding its swap aura at SwapAt stacks hands it to another tank
    std::vector<BuffAssignment> Buffs;      // one caster for each (party buff spell, member): an other offering member in reach: the one with the fewest members so far, then the nearest, then the lowest guid
    bool TankSwapped = false;               // true while MainTank was handed over because of the swap aura (the new main tank takes the boss: taunt)
    bool NoAoe = false;                     // an enemy in view has the no-area-damage rule
    std::vector<DispelAssignment> Dispels;  // one member per dispeller, the lowest guid first; a healer on an emergency heal is not given one
    std::vector<InterruptAssignment> Interrupts;
    std::vector<CcAssignment> CrowdControl;
    std::vector<HealAssignment> Heals;      // one member per healer, the neediest first; a member nobody needs to heal is not listed
    std::vector<SpreadAssignment> Spread;   // members too close to a lower-guid (or immovable) member of a spread group, each with its clear point
    std::vector<PurgeAssignment> Purges;    // one enemy aura per plan: the nearest ready purger (rank 0) and the next nearest (rank 1)
    std::vector<std::uint64_t> Controlled;  // the members that carry a mindcontrol aura: nobody attacks them
    std::vector<ControlAssignment> Control; // per controlled member: the nearest ready polymorph caster (rank 0) and a backup (rank 1)
    std::unordered_map<std::uint64_t, std::uint64_t> PurgeSeenMs;      // purge target -> when the plan first saw the aura
    std::unordered_map<std::uint64_t, std::uint64_t> ControlSeenMs;    // controlled member -> when the plan first saw it controlled
    std::uint64_t KillTarget = 0;           // the unit the damage dealers share: best kill rank, then a caster, then the weakest; never one that is held
    std::unordered_map<std::uint64_t, std::uint64_t> CastSeenMs;   // casting target -> when the plan first saw it casting
    std::uint64_t LastPlannedMs = 0;
};

// Rebuilds `order` from the party's members and the hostile units in view. A no-op (false) until the interval has passed since the last plan.
bool Plan(Settings const& settings, std::vector<Member> const& members, std::vector<Enemy> const& enemies, std::uint64_t nowMs, Order& order);

// Whether `bot` may send an interrupt at `target` now: true with no plan for that target (the claim map decides), true for its rank 0
// assignment, true for a rank 1 backup once the cast has been in view for settings.BackupDelayMs, false for a bot that is not assigned.
bool MayInterrupt(Settings const& settings, Order const* order, std::uint64_t bot, std::uint64_t target, std::uint64_t nowMs);

// Whether the plan names `bot` for this target at all (any rank): used by targeting to turn on the caster only for the assigned kickers.
bool IsAssignedInterrupt(Order const* order, std::uint64_t bot, std::uint64_t target);

// The shared kill target of the plan, 0 for none (also with no plan).
std::uint64_t KillTargetOf(Order const* order);

// True when the plan gives `member` to another healer and `bot` has no assignment for it: the healer leaves that member alone.
bool HealAssignedElsewhere(Order const* order, std::uint64_t bot, std::uint64_t member);

// True when the plan gives `recipient` this buff spell to another caster: the bot leaves that member's buff to it (the buff claims still guard a pair the plan does not cover).
bool BuffAssignedElsewhere(Order const* order, std::uint64_t bot, std::uint32_t spell, std::uint64_t recipient);

// The dispel the plan gives `bot`, nullptr for none.
DispelAssignment const* DispelFor(Order const* order, std::uint64_t bot);

// Whether the plan says nobody uses area damage now (the raid rules' noaoe row); false with no plan.
bool NoAoeNow(Order const* order);

// The point the plan moves `bot` to for spacing, nullptr for none.
SpreadAssignment const* SpreadFor(Order const* order, std::uint64_t bot);

// The purge the plan gives `bot` now: its rank 0 job, or its rank 1 job once the aura has been in view for settings.BackupDelayMs; nullptr for none.
PurgeAssignment const* PurgeFor(Settings const& settings, Order const* order, std::uint64_t bot, std::uint64_t nowMs);

// Whether `member` is mind controlled by the plan's reading (nobody attacks it).
bool IsControlled(Order const* order, std::uint64_t member);

// The polymorph the plan gives `bot` now (rank 0, or rank 1 after the backup delay), nullptr for none.
ControlAssignment const* ControlFor(Settings const& settings, Order const* order, std::uint64_t bot, std::uint64_t nowMs);

// The unit a bot is to hold, 0 if the plan holds none for it.
std::uint64_t CcTargetFor(Order const* order, std::uint64_t bot);

} // namespace party_planner
