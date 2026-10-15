// BotPolicy: decides what each controlled bot should do, from Tier 2 snapshots. Pure logic: it reads snapshots and returns
// actions, so it is unit-tested with hand-built snapshots (tests/policy_test.cpp).
//
// Behaviour, in order: below 30% health disengage (follow the closest player, or stand still); with a target, approach it
// when it is more than 5 yards away, otherwise attack it, and cast the class rotation (combat_rotation.h) once per global
// cooldown; with no target, pull the closest hostile unless it is part of a group of more than three (over-pull control),
// else follow the closest player or stand still. A threat target is always fought. Decisions are sent when they change.
// A refused cast is retried after a while, and its spell is not used again before then. A spell the snapshot reports as on
// cooldown is skipped. Rend's bleed is tracked per target from accepted casts (layout 13 puts the bot's own auras in the
// snapshot, so a self buff is read as fact; a target's DOTs are only a coarse mask, so the estimate stays).
// Rest (M2): an idle bot below 90% health sits down, and stays sat until its health is full. It stands before it moves,
// follows or fights. A sit refused in combat is retried after a second.
// Dungeon party control (route active): the leader (tank) pulls only when every living member has 80% health, mana users 60% mana, and all
// are within 30 yards (sitting to recover meanwhile; never longer than two minutes); members do not pull and join a fight 2.5 s after
// the tank; a healer class (priest, paladin, shaman, druid) heals the party member below 60% health; a cast refused for line of sight
// makes the bot close to melee range for five seconds.
// Dead bots (life_state, not health: a released ghost has health 1). A corpse waits kReleaseDelay for a living priest, paladin or shaman to
// resurrect it, and releases its spirit at once when none is up, when the delay is over, or when the whole party is down; it never releases
// while the party is fighting (capped). A ghost walks back to its corpse (through the instance door when the corpse is inside, see
// PolicyOptions::Portal), reclaims it within reach, and accepts a resurrect offer at any time. A rezzer out of combat resurrects the
// nearest dead member in range after healing the hurt, and the party leader waits for a corpse or a ghost on its way (capped).
// Parties (M8): with PartySize set, bots are cut into parties. The leader invites its members and a member accepts the
// invite, leaves any other party, follows its leader when it is more than 8 yards away, and fights the leader's target
// when it has none of its own. Party actions are sent only out of combat.
#pragma once

#include "forever_module/tier2_session.h"
#include "chat_commands.h"
#include "combat_rotation.h"
#include "grind_data.h"
#include "quest_planner.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// A place in the world, for travel (M6).
struct Point
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

// Options for the policy. TestSpell (0 = off) replaces the rotation: it is cast at the target every CastEveryTicks
// ticks, which exercises the cast path (milestone M1). ClassId selects the rotation (0 = none).
struct PolicyOptions
{
    std::uint32_t TestSpell = 0;
    std::uint64_t CastEveryTicks = 20;
    std::uint32_t ClassId = 0;
    // The class of each bot, when bots differ (random picks). Unset, or returning 0, falls back to ClassId.
    std::function<std::uint32_t(std::uint64_t botGuid)> ClassOf;
    // Vendor visits and trainer checks for an idle bot (M3, M4). A level-up checks the trainer at once. Zero turns that
    // service off; the module turns them on from its settings.
    std::uint64_t ServiceEveryMs = 0;
    std::uint64_t TrainEveryMs = 0;
    // Parties (M8). PartyCount parties of PartySize bots are cut from the lowest-guid bots, in guid order. Each party's
    // leader is its first warrior, else its lowest-guid bot. PartySize 0 turns parties off.
    std::uint32_t PartySize = 0;
    std::uint32_t PartyCount = 1;
    // Hubs (M7): the places an idle bot walks to when it has seen nothing for a while, per map, in order. The bot's map
    // comes from its snapshot. A map with no hubs has no wandering.
    std::map<std::uint32_t, std::vector<Point>> HubsByMap;
    // Quests (M5): the starter quest data. Unset or empty means the bots do no quests and grind as before.
    std::shared_ptr<QuestData const> Quests;
    // A quest walk that has not moved for two minutes teleports to the giver when this is set. Off by default: the core refuses
    // the teleport unless Bot.Actions.Teleport is on, and retrying it held level-1 bots in walk_to. With this off, that start is
    // dropped the same way a no-route move is, and the bot grinds. A refused teleport drops the start either way.
    bool QuestTeleport = false;
    // Dungeon route (doc 19): the waypoints the party leader walks, in order, while the route is active (SetRouteActive). Empty = none.
    std::vector<Point> Route;
    // Auto gear (AutoGearing setting): every bot is dressed for its level when it first appears, and again after AutoGearEveryLevels levels.
    bool AutoGearing = false;
    // Receives one line per notable decision (a heal cast, a pull held back or released), for the run logs. Unset = silent.
    std::function<void(std::string const&)> OnEvent;
    std::uint32_t AutoGearEveryLevels = 5;
    // An idle bot walks to a grind spot, a hub, or takes an exploring walk. false keeps it where it is (for tests that need a bot to stand still).
    bool WanderWhenIdle = true;
    // Grind spots (leveling): the places an idle bot walks to for its level, instead of its map's hubs. SideOf gives a bot's faction (1 Alliance,
    // 2 Horde, 0 unknown).
    std::shared_ptr<GrindData const> Grind;
    std::function<std::uint8_t(std::uint64_t botGuid)> SideOf;
    // A bot's race id (0 unknown), for the quests' race limits.
    std::function<std::uint32_t(std::uint64_t botGuid)> RaceOf;
    // Rotations from a file (data/rotations.txt, tools/make_rotations.py): the fight rows and the upkeep buffs of the classes it lists. A class it does
    // not list keeps the built-in rows.
    std::shared_ptr<rotation::RotationData const> Rotations;
    // Chat commands: a bot accepts a party invite from a player (a human leader), so the player can bring bots into a group and command them.
    bool AcceptInvites = false;
    // Alts (a player's own characters playing as bots): never dressed by the module.
    std::function<bool(std::uint64_t botGuid)> IsAlt;
    // Dead-bot recovery. Portal: the instance door on the outside map (PortalMap) that leads to PortalInsideMap; a ghost released outside walks
    // there when its corpse is inside. Without it such a ghost gives up (corpse_run_failed).
    bool HasPortal = false;
    std::uint32_t PortalMap = 0;
    Point PortalPoint;
    std::uint32_t PortalInsideMap = 0;
    std::uint64_t ReleaseDelayMs = 20000;       // a corpse with a rezzer up waits this long before it releases
    std::uint64_t ReleaseCombatHoldMs = 60000;  // and a release is held back while the party fights, but never longer than this after death
    std::uint64_t CorpseRunTimeoutMs = 240000;  // a ghost that has not reached its corpse by then gives up
    std::uint64_t WaitForRezMs = 90000;         // the party waits this long for a corpse to be resurrected
    std::uint64_t WaitForGhostMs = 180000;      // and this long after the ghost entered the corpse's map
};

// Name of an action status, for logs.
char const* ActionStatusName(forever_module::BotActionStatus status);

class BotPolicy
{
public:
    explicit BotPolicy(PolicyOptions options = {}) : _options(options), _quests(options.Quests) {}

    // Actions for the bots in `view`. nowMs is a monotonic clock in milliseconds (cooldowns, retries, fight time).
    // Decisions are sent only when they change; casts follow the rotation, the global cooldown and the retries.
    // Turns the dungeon route on or off (the scenario director does, once the party is inside).
    void SetRouteActive(bool active) { _routeActive = active; }

    // Dress this bot for its level at the next tick (the .autogear command).
    void RequestAutoGear(std::uint64_t botGuid) { _bots[botGuid].GearRequested = true; }

    // Who said a chat command and where, for ApplyChat. Issuer is the speaker's character guid counter (the group leader), Recipient the
    // whispered bot's guid counter (0 for party or raid chat). Pos and MapId are the speaker's; TargetRef is its selected unit (0 = none).
    struct ChatContext
    {
        std::uint64_t Issuer = 0;
        std::uint64_t Recipient = 0;
        std::uint32_t MapId = 0;
        Point Pos;
        std::uint64_t TargetRef = 0;
    };

    // Applies a chat command (chat_commands.h) to the bots of the speaker's group that its selector names (a whisper: that one bot, when the
    // speaker leads its group). Returns the lines for the speaker: a status line per bot, or why nothing happened. Silent on success.
    std::vector<std::string> ApplyChat(forever_module::WorldTickSnapshotView const& view, ChatContext const& chat, ChatCommand const& command,
                                       std::uint64_t nowMs);

    std::vector<forever_module::BotAction> Decide(forever_module::WorldTickSnapshotView const& view, std::uint64_t nowMs = 0);

private:
    struct Decision
    {
        forever_module::ActionType Type = forever_module::ActionType::kStop;
        std::uint64_t Target = 0;
        Point Dest;                                      // kMoveTo destination
        std::uint32_t Map = 0;                           // kTeleport map (spell_id of the action)
    };

    // A damage-over-time effect the policy believes is on a target, from an accepted cast.
    struct Dot
    {
        std::uint64_t Target = 0;
        std::uint64_t UntilMs = 0;
    };

    struct BotState
    {
        std::optional<Decision> Last;                    // the last movement or attack decision sent
        bool InFight = false;
        std::uint64_t FightStartMs = 0;
        std::uint64_t NextCastMs = 0;                    // global cooldown: no rotation cast before this
        std::uint32_t LastSequence = 0;                  // last action result seen
        std::unordered_map<std::uint32_t, std::uint64_t> RetryAtMs; // spell -> time it may be cast again
        std::unordered_map<std::uint32_t, std::uint64_t> CastTarget; // spell -> target of the last cast sent for it
        std::map<std::pair<std::uint32_t, std::uint64_t>, std::uint64_t> BuffUntilMs; // (spell, recipient reference; 0 = the bot itself) -> when the buff runs out
        std::unordered_map<std::uint32_t, Dot> Dots;     // spell -> effect on a target, while it lasts
        std::optional<std::uint64_t> LastTestCastTick;
        std::uint64_t AutoShotTarget = 0;                // the unit Auto Shot was accepted on and is believed to be shooting (0 = not active)
        std::uint64_t SeenThreatTarget = 0;              // the threat target of the last tick, to see it go away
        std::uint32_t LostTargetReason = 0;              // last_target_lost_reason already acted on (layout 13)
        // Hunter pet. The snapshot has no pet, so this is only what the accepted and refused pet casts imply.
        bool PetMissing = true;                          // the pet is not believed to be out: Call Pet (or Revive Pet when PetDead) is due
        bool PetDead = false;                            // Call Pet was refused: the pet is believed dead, Revive Pet is due
        bool PetProbe = false;                           // a fight ended: Mend Pet (from level 12) or a Call Pet checks the pet is still there
        bool PetFought = false;                          // the bot fought since the last pet check
        std::uint32_t PetFailures = 0;                   // pet casts refused in a row, for the backoff
        std::uint64_t PetRetryAtMs = 0;                  // no pet cast out of combat before this
        std::uint64_t PetProbeAtMs = 0;                  // below level 12 (no Mend Pet) a Call Pet probe is not sent before this
        std::uint64_t PetMendAtMs = 0;                   // no Mend Pet in a fight before this
        bool PetCommanded = false;                       // the pet has been sent at this fight's target (layout 13)
        bool Sitting = false;                            // sat down to rest; stands before it moves or fights
        std::uint64_t RestRetryAtMs = 0;                 // a refused sit is not tried again before this
        bool Mounted = false;                            // the bot is mounted: travel is faster, combat dismounts it
        std::uint64_t MountRetryMs = 0;                  // no mount or dismount request before this
        std::uint64_t NextServiceMs = 0;                 // next vendor visit (sell grey, repair, equip)
        std::uint64_t NextTrainMs = 0;                   // next trainer check
        std::uint32_t ComboPoints = 0;                   // combo points on ComboTarget, from the bot's own Sinister Strikes
        std::uint64_t ComboTarget = 0;
        std::uint32_t LastTrainLevel = 0;                // the level at the last trainer check
        std::uint32_t AcceptRefusals = 0;                // resurrect answers refused in a row
        std::uint64_t AcceptIgnoreFrom = 0;              // the resurrector whose offer is ignored until then
        std::uint64_t AcceptIgnoreUntilMs = 0;           // a resurrect offer is not answered before this (it kept being refused)
        std::uint64_t StallTarget = 0;                   // fight watchdog: the target being watched
        std::uint32_t StallTargetHealth = 0;
        std::uint32_t StallBotHealth = 0;
        std::uint64_t StallLastMs = 0;                   // when the watchdog last looked; a gap does not restart the timer
        std::uint64_t StallSinceMs = 0;                  // since when neither the target's nor the bot's health has gone down
        std::unordered_map<std::uint64_t, std::uint32_t> StallCount;   // unit -> how many fights against it stalled (the avoid time grows)
        std::uint64_t ManaRestSinceMs = 0;               // when the current rest for mana began (0 = not resting for mana)
        std::uint64_t ManaRestBlockedUntilMs = 0;        // a rest that ran its full time without refilling the mana is not started again before this
        bool ServiceUrgent = false;                      // a quest hand-in was refused (bags full): go to a vendor now, even with quest work to do
        float DiagX = 0.0f, DiagY = 0.0f;                // where the bot was at the last stuck check
        std::uint64_t DiagAtMs = 0;                      // when the stuck check last ran
        std::uint64_t QuestDiagAtMs = 0;                 // when the quest line was last written (quest_state event)
        std::uint32_t NoRouteSequence = 0;               // the last kNoRoute result read (last_action_sequence)
        std::uint64_t GrindRetryAtMs = 0;                // no grind spot on this map for this level: explore on foot until this time, then look again
        std::uint32_t HubFailures = 0;                   // hub walks in a row that ended without arriving
        std::uint32_t ExploreStep = 0;                   // random walks made, for the direction of the next
        Point ExploreTarget;
        std::uint64_t InviteAcceptAtMs = 0;              // a player's invite is not answered again before this
        std::uint64_t PartyRetryAtMs = 0;                // a party action is not sent again before this
        std::unordered_map<std::uint64_t, std::uint64_t> AvoidUntilMs; // unit -> until when the bot picks another target
        std::unordered_map<std::uint64_t, std::uint32_t> AvoidCount;   // unit -> how many times it has been avoided
        bool Approaching = false;                        // walking to its target, not yet in range
        std::uint64_t ApproachTarget = 0;
        std::uint64_t ApproachSinceMs = 0;               // when it started walking to ApproachTarget
        std::unordered_map<std::uint64_t, std::uint64_t> FoughtAtMs;   // creature -> when it was last the bot's target (to loot it)
        std::unordered_map<std::uint64_t, std::uint64_t> LootedUntilMs; // creature -> not looted again before this
        bool Idling = false;                             // seeing nothing and doing nothing since IdleSinceMs
        std::uint64_t IdleSinceMs = 0;
        bool Wandering = false;                          // walking to a hub
        std::uint64_t WanderStartMs = 0;                 // when this walk began; dropped after kWanderTimeoutMs
        Point GrindTarget;                               // the grind spot being walked to
        std::vector<std::size_t> GrindVisited;           // the last spots visited (indexes), so the bot moves on instead of going back and forth
        std::size_t NextHub = 0;                         // index of the hub to walk to next, on the bot's map
        Point WalkAnchor;                                // where the bot stood when its quest walk was last seen moving
        std::uint64_t WalkStillSinceMs = 0;              // when it was at WalkAnchor; 0 when no quest walk is watched
        bool QuestTeleportSent = false;                  // one teleport already sent for this stuck walk; a second stuck drops the start
        bool Traveling = false;                          // walking to the vendor for a visit
        std::uint64_t TravelStartMs = 0;                 // when this walk began; it is dropped after kTravelTimeoutMs
        std::unordered_set<std::uint32_t> CastTimeSpells; // spells the core refused because the bot was moving: they need it standing
        // Chat orders (chat_commands.h).
        bool ChatStay = false;                           // hold the position
        std::uint64_t ChatFollow = 0;                    // follow this player reference (0 = no order)
        enum class Stance : std::uint8_t { Aggressive, Defensive, Passive };
        Stance Stance_ = Stance::Aggressive;             // aggressive: fights what the group fights or meets; defensive: only what attacks it or the group; passive: only what attacks it
        enum class Role : std::uint8_t { Auto, Tank, Healer, Dps };
        Role Role_ = Role::Auto;                         // overrides what the class gives (tank pulls and leads, dps does not heal on its own)
        std::uint64_t FocusMissingMs = 0;                // since when the focus target has not been in view
        std::uint64_t Focus = 0;                         // the creature attacked first in a fight
        float FollowYards = 0.0f;                        // follow distance (0 = the default)
        bool FollowWalking = false;                      // walking after the speaker (hysteresis between FollowYards and half of it)
        std::optional<forever_module::BotAction> OrderAction; // a summon, revive or mount order, sent on this bot's next turn
        bool GotoActive = false;                         // walking to GotoPoint
        Point GotoPoint;
        std::uint64_t GotoUntilMs = 0;
        std::uint64_t ForcedTarget = 0;                  // the creature a pull order points at
        std::uint64_t ForcedUntilMs = 0;
        std::uint64_t ForcedHeal = 0;                    // the friend a heal order points at (once)
        bool ReleaseNow = false;                         // a release order: no waiting for a resurrect
        std::uint32_t SeenHealthPct = 100;               // for the status line, from the last snapshot
        std::uint32_t SeenPowerPct = 100;
        std::uint8_t SeenLife = 0;
        bool SeenInCombat = false;
        std::uint64_t RangeCloserUntilMs = 0;            // standing closer to the target after an out-of-range refusal, until this time
        std::uint32_t LosRefusals = 0;                   // line-of-sight refusals in a row
        std::uint64_t CloserUntilMs = 0;                 // closing to melee range after a line-of-sight refusal, until this time
        std::uint64_t HoldSinceMs = 0;                   // the party leader waits for the party to be ready since this time (0 = not waiting)
        std::uint32_t GearedAtLevel = 0;                 // the level at the last auto gear (0 = not yet)
        bool GearRequested = false;                      // the .autogear command asked for it
        std::uint64_t GearRetryMs = 0;                   // no auto gear before this
        std::size_t RouteIndex = 0;                      // the leader's next route waypoint
        QuestMemory Quest;                               // the quest planner's memory of this bot
        std::unordered_map<std::uint64_t, std::uint64_t> InvitedAtMs; // member guid -> when this bot last invited it
        // Death and recovery.
        std::uint8_t Life = 0;                           // life_state at the last tick (0 alive, 1 corpse, 2 ghost)
        std::uint64_t DeathMs = 0;
        bool ReleaseSent = false;
        std::uint64_t ReleaseSentMs = 0;                 // a release is not sent again before kReleaseRetryMs
        bool RunStarted = false;
        bool Inside = false;                             // the ghost is on its corpse's map (InsideSinceMs)
        std::uint8_t DeadWaitKind = 0;                   // the leader waits for a dead member: 0 no, 1 for a rez, 2 for a ghost
        std::uint64_t DeadWaitSinceMs = 0;
        bool ReleaseHeldLogged = false;
        std::uint64_t AcceptRetryMs = 0;
        std::uint64_t AcceptedFrom = 0;                  // the resurrecter of the last accept sent (0 = none)
        std::uint64_t ReclaimRetryMs = 0;
        bool Reclaimed = false;                          // a reclaim was sent in this death
        std::uint64_t RunStartMs = 0;                    // the corpse run began (0 = not yet)
        std::uint64_t InsideSinceMs = 0;                 // the ghost is on the corpse's map since
        bool RunFailed = false;
        float RunStartDist = 0.0f;                       // distance to the corpse at the start (same map), for progress lines
        std::uint32_t RunProgress = 0;                   // 25, 50 or 75: the last progress line written
        std::unordered_map<std::uint64_t, std::uint64_t> RezRetryAtMs; // dead member -> no resurrect cast on it before this
        std::unordered_map<std::uint64_t, std::uint32_t> RezRefusals;  // dead member -> refusals in a row
    };

    // The rows of a class's rotation: from the rotation file when it lists the class, else the built-in ones.
    std::span<rotation::Row const> RowsFor(std::uint32_t classId) const;
    // How long a spell's effect lasts, for the dot and buff upkeep: from the rotation file, else the built-in table (0 = not tracked).
    std::uint64_t DurationMsOf(std::uint32_t classId, std::uint32_t spell) const;

    void ReadResult(BotState& state, forever_module::BotSnapshot const& bot, std::uint64_t nowMs);

    std::unordered_map<std::uint64_t, BotState> _bots; // bot guid -> state
    // Corpses a bot has sent a loot for -> the looter's group (its leader's reference, else its own guid) and when the claim ends. The
    // core loots a whole corpse for a group at once, so a second member's loot is refused ("no right to loot": already looted).
    struct LootClaim
    {
        std::uint64_t Group = 0;
        std::uint64_t UntilMs = 0;
    };
    std::unordered_map<std::uint64_t, LootClaim> _lootClaims;
    // A buff cast on a party member: (spell, recipient reference) -> who cast it and until when it runs. Several healers must
    // not refresh the same buff on the same member; the caster's own BuffUntilMs cannot see another bot's cast.
    struct BuffClaim
    {
        std::uint64_t Group = 0;
        std::uint64_t UntilMs = 0;
    };
    std::map<std::pair<std::uint32_t, std::uint64_t>, BuffClaim> _buffClaims;
    // Who is healing which member and until when: two healers of a party must not cast on the same member.
    struct HealClaim
    {
        std::uint64_t Group = 0;
        std::uint64_t UntilMs = 0;
    };
    std::unordered_map<std::uint64_t, HealClaim> _healClaims; // member bot guid -> claim
    PolicyOptions _options;
    QuestPlanner _quests;
    bool _routeActive = false;
};
