// BotPolicy: decides what each controlled bot should do, from Tier 2 snapshots. Pure logic: it reads snapshots and returns
// actions, so it is unit-tested with hand-built snapshots (tests/policy_test.cpp).
//
// Behaviour, in order: below 30% health disengage (follow the closest player, or stand still); with a target, approach it
// when it is more than 5 yards away, otherwise attack it, and cast the class rotation (combat_rotation.h) once per global
// cooldown; with no target, pull the closest hostile unless it is part of a group of more than three (over-pull control),
// else follow the closest player or stand still. A threat target is always fought. The default stance is defensive: a bot
// pulls only when it is aggressive (chat "aggressive") and otherwise joins a fight that its group or it is in. Decisions are sent when they change.
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
#include "spell_costs.h"
#include "crafting_foci.h"
#include "bot_random.h"
#include "chat_say.h"
#include "dungeon_events.h"
#include "errand_trainers.h"
#include "supply_policy.h"
#include "taxi_policy.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
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

// A town errand order (errand_trainers.h, errand_policy.h): walk to a capital trainer, then train. The trip is done when the core applies the train.
// An auction order walks to the auctioneer instead and is done once the bot is in reach (the listing visit does the rest).
struct ErrandOrder
{
    std::uint32_t Map = 0;
    Point Dest;                 // the trainer's or the auctioneer's position
    std::uint32_t Skill = 0;    // the profession skill line the bot trains
    std::uint32_t Entry = 0;    // the trainer's creature entry
    bool Auction = false;       // walk to the Auction House, not a trainer
};

enum class ErrandOutcome : std::uint8_t { kNone, kDone, kFailed };

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
    // Randomness (bot_random.h). RandomSeed makes a run repeatable. RandomPct is how much the bots' timers and thresholds vary from bot to bot and from time
    // to time (+- this many percent); 0 turns the variation off (the tests). PartyPercent is the share of the bots that take part in the parties at all
    // (the others stay on their own); the parties are then of random sizes from 2 up to PartySize. The defaults keep the old behavior: no variation, every bot partied.
    std::uint64_t RandomSeed = 0;
    // Crafting (SDK 0.17, kSkillUp): a bot with nothing to fight makes bandages from cloth and cooks meat, which raises First Aid and Cooking and uses up what fills its
    // bags. Off by default (the core must know the action); the module turns it on from its settings.
    bool Crafting = false;
    // SDK 0.18: with Crafting, the bot also makes things for the crafting profession it has (Blacksmithing, Leatherworking, Alchemy, Tailoring, Engineering, Smelting),
    // and walks to the nearest anvil, forge or fire of Foci when the core says a recipe needs one (kNeedsFocus). Gathering: it asks the core to gather (kGather) with
    // the gathering profession it has (Mining, Herbalism, Skinning).
    bool Gathering = false;
    std::shared_ptr<std::vector<crafting::FocusPoint> const> Foci;
    std::uint32_t RandomPct = 0;
    std::uint32_t PartyPercent = 100;
    // Hubs (M7): the places an idle bot walks to when it has seen nothing for a while, per map, in order. The bot's map
    // comes from its snapshot. A map with no hubs has no wandering.
    std::map<std::uint32_t, std::vector<Point>> HubsByMap;
    // Quests (M5): the starter quest data. Unset or empty means the bots do no quests and grind as before.
    std::shared_ptr<QuestData const> Quests;
    // A quest walk that has not moved for two minutes teleports to the giver when this is set. Off by default: the core refuses
    // the teleport unless Bot.Actions.Teleport is on, and retrying it held level-1 bots in walk_to. With this off, that start is
    // dropped the same way a no-route move is, and the bot grinds. A refused teleport drops the start either way.
    bool QuestTeleport = false;
    // Dungeon recovery by teleport (config DungeonTeleport; needs Bot.Actions.Teleport in the core, so for tests and scenarios): a leader that has no route to a waypoint is put
    // back on the route once, and a member that has lost its leader (far away, or on another level) is put next to it. Off by default.
    bool DungeonTeleport = false;
    // Dungeon revive out of combat (config DungeonReviveOutOfCombat; needs Bot.Chat.Orders.Enabled in the core): a dead member of a dungeon party is revived by its
    // leader, when the leader is alive and not fighting, instead of walking back to its corpse. Off by default.
    bool DungeonReviveOutOfCombat = false;
    // Bots start Aggressive: they pull what is in range, as they did before the defensive default. Off by default; the combat and pull
    // tests turn it on. A chat stance order still changes the stance either way.
    bool AggressiveStart = false;
    // Dungeon route (doc 19): the waypoints the party leader walks, in order, while the route is active (SetRouteActive). Empty = none.
    std::vector<Point> Route;
    // Which route points (0-based) are bosses, and the entry of the boss at each (ScenarioBossAt): the leader waits at such a point until that boss is dead.
    std::map<std::size_t, std::uint32_t> RouteBossAt;
    // The events of the scenario's dungeon (dungeon_events.h): the leader runs them along the route.
    std::vector<dungeon_events::Event> Events;
    // The dungeons the "tank lead" chat command can run, by map id: the route, the boss points on it and the events.
    struct DungeonRoute
    {
        std::vector<Point> Route;
        std::map<std::size_t, std::uint32_t> BossAt;
        std::vector<dungeon_events::Event> Events;
    };
    std::map<std::uint32_t, DungeonRoute> DungeonsByMap;
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
    // The faction for the bot parties (a party is one side): like SideOf, and it also knows the custom races. Unset, SideOf is used.
    std::function<std::uint8_t(std::uint64_t botGuid)> PartySideOf;
    // A bot's race id (0 unknown), for the quests' race limits.
    std::function<std::uint32_t(std::uint64_t botGuid)> RaceOf;
    // Rotations from a file (data/rotations.txt, tools/make_rotations.py): the fight rows and the upkeep buffs of the classes it lists. A class it does
    // not list keeps the built-in rows.
    std::shared_ptr<rotation::RotationData const> Rotations;
    // What spells cost (data/spell_costs.txt, made by the core): with it a bot does not try a spell it cannot pay for. Unset, every cast is tried.
    std::shared_ptr<spell_cost::SpellCostData const> SpellCosts;
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
    // Supplies (layout 15): a bot short of ammunition, food or drink walks to a vendor that sells it and buys, and eats or drinks while resting.
    supply::Settings Supplies;
    // Flight points (layout 17, taxi_policy.h): a long walk becomes a flight from the nearest flight master. Off by default.
    taxi::Settings Taxi;
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
    // The creature entries of the bosses seen dead so far (the scenario director's), for the route's boss points.
    void SetKilledBosses(std::set<std::uint32_t> const& entries) { _killedBosses = entries; }

    // Dress this bot for its level at the next tick (the .autogear command).
    void RequestAutoGear(std::uint64_t botGuid) { StateOf(botGuid).GearRequested = true; }
    // Asks for a bag-only gear pass at the next tick (a trade just finished): the core wears what the bot carries when it is better.
    void RequestBagGear(std::uint64_t botGuid) { StateOf(botGuid).BagGearRequested = true; }
    // Town errands: the bot walks to the order's trainer and trains there. CancelErrand drops it (the bot was called away); TakeErrandOutcome
    // returns how the errand ended (done or failed) once and clears it.
    void StartErrand(std::uint64_t botGuid, ErrandOrder const& order);
    void CancelErrand(std::uint64_t botGuid);
    ErrandOutcome TakeErrandOutcome(std::uint64_t botGuid);

    // Who said a chat command and where, for ApplyChat. Issuer is the speaker's character guid counter (the group leader), Recipient the
    // whispered bot's guid counter (0 for party or raid chat). Pos and MapId are the speaker's; TargetRef is its selected unit (0 = none).
    struct ChatContext
    {
        std::uint64_t Issuer = 0;
        std::uint64_t Recipient = 0;
        std::uint32_t MapId = 0;
        Point Pos;
        std::uint64_t TargetRef = 0;
        bool Say = false;           // the bots that take the order answer out loud (ChatSay)
        std::uint32_t SayMax = 0;   // at most this many bots answer one order (a whispered bot always does)
    };

    // Applies a chat command (chat_commands.h) to the bots of the speaker's group that its selector names (a whisper: that one bot, when the
    // speaker leads its group). Returns the lines for the speaker: a status line per bot, or why nothing happened. Silent on success.
    // `acks`, when given, receives the lines the bots that took the order say out loud (chat_say.h), at most chat.SayMax of them.
    // Presence (bot_presence.h): whether nothing a log off would cut short is going on for this bot: no fight, death, party or invite, chat order,
    // vendor walk or supply trip, and no walk to hand in a quest.
    bool SafeToLogOut(forever_module::BotSnapshot const& bot) const;

    std::vector<std::string> ApplyChat(forever_module::WorldTickSnapshotView const& view, ChatContext const& chat, ChatCommand const& command,
                                       std::uint64_t nowMs, std::vector<ChatAck>* acks = nullptr);

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
        std::uint32_t TeleportLoggedSequence = 0;        // the last teleport result written to the log
        Point NoRouteDest{};                             // the destination of the last move the core found no route to
        std::uint64_t NoRouteUntilMs = 0;                // a move to (about) that place is not sent before this time
        std::uint32_t NoRouteStreak = 0;                 // no-route answers in a row for that place: the pause doubles each time
        std::uint64_t WalkBlockedAtMs = 0;               // when a move got the answer "no route": the walk (grind spot, hub, exploring) that began before it is over, and the bot picks another later
        std::uint64_t NoRouteLoggedMs = 0;               // when a no-route refusal of any action was last written to the log (diagnostics)
        std::uint32_t NoRouteSequence = 0;               // the last kNoRoute result read (last_action_sequence)
        std::uint64_t GrindRetryAtMs = 0;                // no grind spot on this map for this level: explore on foot until this time, then look again
        std::uint32_t HubFailures = 0;                   // hub walks in a row that ended without arriving
        std::uint32_t ExploreStep = 0;                   // random walks made, for the direction of the next
        std::uint64_t CraftRetryAtMs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 }; // per craft line (kCraftLines in bot_policy.cpp): no kSkillUp for the skill before this time
        std::uint32_t CraftTurn = 0;                     // which line is tried first next (they take turns)
        std::uint32_t CraftSent = 0;                     // the line of the last kSkillUp sent: its answer is for that line, whatever spell the core reports
        std::uint64_t GatherRetryAtMs[3] = { 0, 0, 0 };  // Mining, Herbalism, Skinning: no kGather for the skill before this time
        std::uint32_t GatherSent = 0;                    // the line of the last kGather sent
        bool CraftWalking = false;                       // walking to an anvil, forge or fire because a recipe needs one (kNeedsFocus)
        std::uint32_t CraftWalkLine = 0;                 // the craft line whose recipes need it
        std::uint32_t CraftWalkIndex = 0;                // the object, an index into Foci
        std::uint64_t CraftWalkStartMs = 0;
        std::uint32_t RestPct = 0;                       // the health below which this bot rests (a trait drawn once from the seed; 0 = not drawn yet)
        float LeaderYards = 0.0f;                        // how close to its leader this bot stays put (a trait, like RestPct)
        std::uint32_t WanderCycles = 0;                  // walks to a grind spot or hub begun, the counter of the random draws for the next one
        std::uint64_t WanderIdleMs = 0;                  // how long this bot idles before the next walk (varied per bot and per walk; 0 = not drawn yet)
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
        std::uint64_t WalkStillSinceMs = 0;              // when it was at WalkAnchor; only read while WalkAnchored is set
        bool WalkAnchored = false;                       // a quest walk is being watched: WalkAnchor and WalkStillSinceMs are live.
                                                        // A flag, not WalkStillSinceMs == 0, because 0 is a legal timestamp.
        bool QuestTeleportSent = false;                  // one teleport already sent for this stuck walk; a second stuck drops the start
        bool Traveling = false;                          // walking to the vendor for a visit
        // Supplies (supply_policy.h).
        std::uint64_t NextSupplyMs = 0;                  // no supply trip before this
        std::uint32_t SupplyFailures = 0;                // trips in a row that bought nothing
        std::uint64_t SupplyCheckAtMs = 0;               // when the last trip is judged (0 = none pending)
        supply::Needs SupplyAsked;                       // what that trip asked for
        supply::Facts SupplyBefore;                      // and what the bot carried before it
        bool SupplyTrip = false;                         // the walk in progress is a supply trip
        std::uint64_t NextConsumeMs = 0;                 // no eat or drink before this
        bool TankLead = false;                           // told "tank lead": this bot walks its group through the dungeon of its map
        std::shared_ptr<dungeon_events::Runner> Events;  // the events of the dungeon, run by the bot that leads the route
        std::uint32_t EventsMap = 0;                     // the map those events are for
        std::size_t RouteRecoveredAt = std::size_t(-1);  // the route point the leader was last put back on the route for (once per point)
        std::uint64_t LostSinceMs = 0;                   // a member has been far from its leader since this time (0 = not lost)
        std::uint64_t NextRecoverMs = 0;                 // no teleport back to the leader before this
        std::uint64_t RouteBossSinceMs = 0;              // when the leader began to wait at a boss point (0 = not waiting)
        std::uint32_t RouteNoRoute = 0;                  // moves in a row to the current dungeon waypoint that the core found no route for
        std::uint64_t TaxiBlockedUntilMs = 0;            // the core refused a flight: walk until this
        std::uint64_t TaxiWaitUntilMs = 0;               // a flight was ordered: give it time to show
        std::uint64_t NextPotionMs = 0;                  // no potion before this (the game shares one cooldown between potions)
        std::uint64_t TravelStartMs = 0;                 // when this walk began; it is dropped after kTravelTimeoutMs
        std::unordered_set<std::uint32_t> CastTimeSpells; // spells the core refused because the bot was moving: they need it standing
        // Chat orders (chat_commands.h).
        bool ChatStay = false;                           // hold the position
        std::uint64_t ChatFollow = 0;                    // follow this player reference (0 = no order)
        enum class Stance : std::uint8_t { Aggressive, Defensive, Passive };
        Stance Stance_ = Stance::Defensive;              // aggressive: fights what the group fights or meets; defensive: only what attacks it or the group; passive: only what attacks it
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
        bool TrainAfterGoto = false;                     // a train order: when the goto ends, the bot learns from the trainer it stands at
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
        std::uint64_t HoldLastNotReadyMs = 0;            // the last tick the party was not ready (the pull hold ends after kPullHoldClearMs of readiness)
        std::uint64_t HoldSinceMs = 0;                   // the party leader waits for the party to be ready since this time (0 = not waiting)
        std::uint32_t GearedAtLevel = 0;                 // the level at the last auto gear (0 = not yet)
        bool GearRequested = false;                      // the .autogear command asked for it
        bool BagGearRequested = false;                   // a bag-only pass is wanted now (a trade finished)
        std::uint64_t NextBagGearMs = 0;                 // the next bag-only pass (0 = not scheduled yet)
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
        std::uint64_t CorpseTeleportRetryMs = 0;         // the next teleport of a ghost to its corpse (no portal route)
        std::uint64_t ReviveRetryMs = 0;                 // the next out-of-combat revive order of this ghost by its leader
        std::uint32_t ReviveAttempts = 0;                // revive orders sent in this death
        bool Reclaimed = false;                          // a reclaim was sent in this death
        std::optional<ErrandOrder> Errand;               // the town errand this bot is on (none = not on one)
        std::uint64_t ErrandTrainSentMs = 0;             // the next train order of the errand (0 = send now)
        std::uint32_t ErrandRefusals = 0;                // train refusals in a row
        ErrandOutcome ErrandResult = ErrandOutcome::kNone; // the errand's end, until the module takes it
        std::uint64_t RunStartMs = 0;                    // the corpse run began (0 = not yet)
        std::uint64_t InsideSinceMs = 0;                 // the ghost is on the corpse's map since
        bool RunFailed = false;
        bool RunTimeoutTeleported = false;               // the ghost was put at its corpse once after a run that took too long (a corpse far away, or a route that goes nowhere)
        std::uint64_t DeathTimesMs[3] = { 0, 0, 0 };     // when this bot last died (newest last): two deaths close together mean it dies where it stands up (DeathLoopMs)
        std::uint64_t EscapeUntilMs = 0;                 // after a death loop the bot is put at a suitable grind spot; not again before this
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

    // The state of a bot, created on first use with the starting stance (PolicyOptions::AggressiveStart).
    BotState& StateOf(std::uint64_t botGuid)
    {
        auto const [at, created] = _bots.try_emplace(botGuid);
        if (created && _options.AggressiveStart)
            at->second.Stance_ = BotState::Stance::Aggressive;
        return at->second;
    }

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
    std::map<std::uint32_t, std::set<std::uint32_t>> _tankKilled;   // map -> bosses seen dead during a "tank lead" run
    std::set<std::uint32_t> _killedBosses;
};
