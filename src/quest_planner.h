// SPDX-License-Identifier: GPL-2.0-or-later
// QuestPlanner (M5): what a bot should do about quests, from its quest log in the snapshot and the quest data. Pure logic: it
// reads the snapshot and the bot's memory and returns a step; the policy turns the step into actions. Unit-tested in
// tests/quest_test.cpp.
//
// A bot belongs to the start (see quest_data.h) whose anchor is nearest on its map. Its order of work, every tick:
//   1. a quest in the log that is complete: walk to its ender and hand it in;
//   2. a quest in the log with an objective still short: hunt its targets (the policy fights only those creatures, and walks
//      between the quest's areas when none is near);
//   3. otherwise the next quest of the start that is not done, not in the log and not blocked: walk to its giver and accept.
// Whole-world files (many starts, one per area of quest givers, with 'req' records) work the same way, with two additions: the start is chosen again
// whenever the bot has no quest in its log (the nearest start on its map that has a quest the bot can take now: level, race and class allow it, it is
// not done, in the log or blocked), so a bot that has finished a zone moves on to the next one; and a quest whose 'prev' quest is not known to be
// done is tried after the others. A bot takes new quests while its log has room (kMaxAcceptedQuests).
// A quest with no progress for kStuckMs is abandoned and left alone for kBlockAfterAbandonMs. A refused accept is blocked for
// kBlockAfterAcceptRefusalMs; a refused hand-in for kBlockAfterRefusalMs, growing with each refusal.
// Skyborne exit quests (config SkyborneExitQuests, off by default; quest_data.h 'hop'): a quest whose ender is on another map, in the log of a bot still on the
// start's map (2991), comes first: walk to the dock (or the giver), then a Hop step (kTeleport to the hop point); on the other map the quest is handed in as any.
// Only map 2991 to map 0 or 1 (kSkyborneMap, HopAllowed). A refused or silent teleport blocks the quest (an hour, six hours, then for good).
// The quest right after such a hop may have a 'use' record (94947, Take the Skyborne Portal to Stormwind: the objective is a criteria tree whose criterion is the
// spell of the portal object; doc/skyborne-alliance-94947.md): in the log and within kPortalAreaYards of the object, walk to it and use it (a UseObject step,
// kUseObject); the portal moves the bot, and the hand-in follows where it lands. kUseTriesBeforeHop uses that do not move the bot are followed by a teleport to
// the 'land' point (the fallback, on the same map; refused or silent like a hop). Same key; a 'use' quest that does not follow a usable hop is unknown.
#pragma once

#include "quest_data.h"
#include "path_variety.h"
#include "forever_module/tier2_session.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <vector>

enum class QuestStepKind
{
    None,      // no quest work: the bot grinds as before
    WalkTo,    // walk to Dest (a giver, an ender)
    Accept,    // at the giver: send the accept for Quest
    HandIn,    // at the ender: send the hand-in for Quest
    Hunt,      // fight only Entries; walk to Dest when none is near
    Abandon,   // send the abandon for Quest
    Reward,    // send the reward for Quest: a class quest completed with its reward, no objectives, from anywhere (no walk)
    Wait,      // stand still: an action was just sent and the log has not caught up
    WaitGroup, // a group quest in the log (Quest, GroupSize): wait for the party (the policy looks for partners); the wait times out into an abandon (config QuestGroups)
    Hop,       // a Skyborne exit quest in the log, the bot at the dock: teleport to Dest on Map (config SkyborneExitQuests)
    UseObject, // a quest with a 'use' record in the log, the bot at the object: use the object Object at Dest (kUseObject; config SkyborneExitQuests)
};

struct QuestStep
{
    QuestStepKind Kind = QuestStepKind::None;
    std::uint32_t Quest = 0;
    QuestPoint Dest;
    std::vector<std::uint32_t> Entries;
    std::uint32_t GroupSize = 0;      // WaitGroup: the party size the quest wants
    std::uint32_t StartEntry = 0; // Accept: the quest starts from an item or a game object (kQuestStartFrom): the object's entry, 0 for an item
    bool StartFrom = false;       // Accept: send operation kQuestStartFrom instead of kQuestAccept
    bool StartGiver = false; // the walk is to the giver of a quest start on the bot's map (not a turn-in or a hunt)
    std::uint32_t Map = 0;   // Hop: the map of Dest
    bool HopLeg = false;     // a step of a Skyborne exit quest (the walk to the dock, the hop, the hand-in after it): LevelSafeWalks does not drop it
    std::uint32_t Object = 0; // UseObject: the game object's entry
};

// What the planner remembers about one bot.
struct QuestMemory
{
    QuestData const* List = nullptr;                    // the data the start and area indices below refer to (reset when the planner's list changes)
    int StartIndex = -2;                                // -2 not looked up yet, -1 no start for this bot
    std::set<std::uint32_t> Done;                       // quests handed in
    std::map<std::uint32_t, std::uint64_t> BlockedUntilMs;
    std::uint32_t PendingHandIn = 0;                    // a hand-in sent; the quest leaving the log makes it done
    std::uint64_t NextActionMs = 0;                     // no accept or hand-in before this
    std::map<std::uint32_t, std::size_t> AreaIndex;     // quest -> the area being searched
    struct ProgressInfo
    {
        std::uint32_t Sum = 0;
        std::uint64_t SinceMs = 0;
    };
    std::map<std::uint32_t, ProgressInfo> Progress;         // quest -> its objective total and since when it has been that
    std::uint32_t LastActionQuest = 0;                  // the quest of the last accept, hand-in or abandon sent
    bool LastActionAbandon = false;                     // the last action sent was an abandon (its refusal is not a "cannot take")
    std::map<std::uint32_t, std::uint32_t> Refusals;    // quest -> how many accepts or hand-ins the core refused (the block grows with it)
    std::map<std::size_t, std::uint64_t> NoRouteUntilMs; // start index -> not walked to before this (the core found no route to its giver)
    bool WalkingToStart = false;                        // the step from the last Next is the walk to a start's giver
    std::uint32_t LastWalkQuest = 0;                    // the quest of the last walk or hunt step the planner gave (0 = the last step was neither)
    QuestStepKind LastWalkKind = QuestStepKind::None;   // WalkTo or Hunt, for LastWalkQuest
    QuestPoint LastWalkDest;                            // where that step walks to
    std::map<std::uint32_t, std::uint32_t> NoRouteCount; // quest -> no-route answers in a row on its walks (ender, giver or hunting ground)
    std::map<std::uint32_t, std::uint64_t> GroupWaitSinceMs; // group quest -> since when the bot waits for its party
    std::set<std::uint32_t> NearEnough;                 // quests whose giver or ender could not be walked to but is near: the accept or hand-in is tried from here
    std::set<std::uint32_t> RefusedHandIns;             // quests whose hand-in was refused since the last vendor visit
    std::map<std::uint32_t, std::uint32_t> RetryAtLevel;   // quest -> not tried before the bot has this level (refused: level too low)
    std::map<std::uint32_t, std::size_t> RetryAfterDone;   // quest -> not tried before more quests are done than this (refused: a prerequisite missing)
    std::map<std::uint32_t, std::uint32_t> LevelUnsafeAreas; // quest -> hunting areas skipped in a row as too high for the bot (LevelSafeWalks)
    std::uint32_t HopPending = 0;                       // Skyborne exit: the quest whose hop teleport was sent; the bot on HopPendingMap makes it done
    std::uint32_t HopPendingMap = 0;
    std::uint64_t HopSentMs = 0;
    std::map<std::uint32_t, std::uint32_t> HopRefusals; // quest -> hops refused (or silent): the block grows with it
    QuestPoint HopPendingDest;                          // where the pending hop puts the bot
    bool HopSameMap = false;                            // the pending hop stays on the bot's map (the portal fallback): it has landed when the bot is near HopPendingDest
    std::uint32_t UsePending = 0;                       // a 'use' quest whose object was used; the bot moving away (the portal) or kUseWaitMs without a move settles it
    std::uint64_t UseSentMs = 0;
    std::uint32_t UseFromMap = 0;                       // where the bot stood when it used the object
    QuestPoint UseFrom;
    std::map<std::uint32_t, std::uint32_t> UseFails;    // quest -> uses that did not move the bot (refused or silent); kUseTriesBeforeHop of them: the fallback hop
};

// Who the bot is, for the quests' race and class limits. 0 = unknown (no limit applied).
struct QuestBotInfo
{
    std::uint32_t ClassId = 0;
    std::uint32_t Race = 0;
    // Path variety (off when Variety is null): the bot's guid and the run's seed key a per-bot quest order, skips, area start and start ranking.
    path_variety::Settings const* Variety = nullptr;
    std::uint64_t Seed = 0;
    std::uint64_t Bot = 0;
};

constexpr float kQuestReachYards = 4.0f;                // the core allows 5 yards for talking to an NPC
constexpr float kQuestNearTryYards = 9.0f;              // a giver or ender this near that the core has no route to (it stands behind a counter or on an object) is tried from here:
                                                        // the core measures the reach from the NPC's edge, so the action can still work
constexpr std::uint32_t kNoRouteWalksBlock = 3;         // this many no-route answers in a row on one quest's walks block the quest (kWalkStuckBlockMs)
constexpr std::uint64_t kBlockAfterAcceptRefusal2Ms = 3600000;  // the second refused accept of a quest: an hour
constexpr std::uint64_t kBlockAfterAcceptRefusal3Ms = 21600000; // the third and later: six hours (rewarded long ago, wrong class or race, a missing chain step: it will not change)
constexpr float kAreaReachYards = 15.0f;                // a hunting area is searched when the bot is this close
constexpr std::uint64_t kQuestActionGapMs = 3000;       // wait this long after sending an accept or hand-in
constexpr std::uint64_t kBlockAfterRefusalMs = 60000;
constexpr std::uint64_t kBlockAfterAcceptRefusalMs = 600000; // a refused accept: ten minutes (an hour kept bots away from quests they could take once the core allowed it)
constexpr std::uint64_t kStuckMs = 600000;              // no progress for 10 minutes: abandon
constexpr std::uint64_t kBlockAfterAbandonMs = 3600000;
// Refusal reasons (ActionResult::reason, protocol 27): what a refused accept is blocked for, by the reason the core gave.
constexpr std::uint64_t kBlockNeverMs = 3153600000000ull;       // class or race, already done, level too high: it will not change (about 100 years)
constexpr std::uint64_t kBlockPrerequisiteMs = 3600000;         // a prerequisite is missing: until another quest is done, at the latest after an hour
constexpr std::uint64_t kBlockLogFullMs = 120000;               // the quest log is full: a short wait
constexpr std::uint64_t kBlockBagsFullMs = 60000;               // the bags are full (a vendor visit empties them): a short wait
constexpr std::uint64_t kBlockSkillOrReputationMs = 3600000;    // a skill or reputation is short: an hour
constexpr std::uint64_t kWalkStuckBlockMs = 900000;          // a quest whose walk is stuck (no route to its ender or hunting ground) is left alone this long
constexpr std::uint64_t kLevelUnsafeBlockMs = 1800000;       // a quest whose walk goes to or through a place far above the bot (LevelSafeWalks) is left alone this long
constexpr float kWorldStartRadiusYards = 2000.0f;      // a world start (its quests carry levels) is walked to from up to this far (see kStartRadiusYards)
constexpr std::uint64_t kNoRouteBlockMs = 1800000;     // a start whose giver the core found no route to is left alone for this long
constexpr std::uint32_t kGreyQuestMargin = 7;          // a bot this many levels above a quest's level skips it (little experience)
constexpr std::uint32_t kRedQuestMargin = 4;           // a quest this many levels above the bot is left for later
constexpr std::uint32_t kMaxAcceptedQuests = 5;        // no new quest while the log holds this many
constexpr float kStartRadiusYards = 1500.0f;            // a bot farther than this from every anchor of its map has no start
// Skyborne exit quests (SkyborneExitQuests).
constexpr std::uint32_t kSkyborneMap = 2991;             // the Skyborne start map: the only map a hop leaves from
constexpr float kHopDockYards = 8.0f;                    // the hop is sent this near the dock (or giver) point
constexpr std::uint64_t kHopWaitMs = 20000;              // a hop sent and the bot still on its map after this: the core did not move it (counted as refused)
constexpr std::uint64_t kHopRefusedBlockMs = 3600000;    // the first refused hop: an hour; the second kBlockAfterAcceptRefusal3Ms; the third and later for good
constexpr std::uint32_t kHopRefusalsForGood = 3;
// The portal after the exit hop ('use' records, 94947).
constexpr float kUseReachYards = 4.0f;                   // the object is used this near its point (the core allows 8 yards)
constexpr float kPortalAreaYards = 200.0f;               // a 'use' quest in the log is worked at its object while the bot is this near it (Dalaran); farther, it has left
constexpr float kPortalMovedYards = 100.0f;              // a bot this far from where it used the object (or on another map) was moved by it; a fallback hop has landed this near
constexpr std::uint64_t kUseWaitMs = 20000;              // a use sent and the bot not moved after this: the use did nothing (counted as failed)
constexpr std::uint32_t kUseTriesBeforeHop = 2;          // this many failed uses: the fallback teleport to the 'land' point
constexpr std::uint32_t kRaceSkyborneAlliance = 95;      // RACE_SKYBORNE_ALLIANCE (core RaceMask.h)
constexpr std::uint32_t kRaceSkyborneHorde = 96;         // RACE_SKYBORNE_HORDE
// The map pairs a hop may join: 2991 to Eastern Kingdoms (0) or Kalimdor (1). No other hop, and never for zone spread.
constexpr bool HopAllowed(std::uint32_t from, std::uint32_t to) { return from == kSkyborneMap && (to == 0 || to == 1); }

// kQuest's spell_id: (operation << 24) | quest id.
constexpr std::uint32_t kQuestAccept = 1;
constexpr std::uint32_t kQuestHandIn = 2;
constexpr std::uint32_t kQuestAbandon = 3;
constexpr std::uint32_t kQuestReward = 4;
constexpr std::uint32_t kQuestStartFrom = 5;   // protocol 27: start from a carried quest-starting item, or from the game object whose entry is target_guid
constexpr std::uint32_t QuestCode(std::uint32_t operation, std::uint32_t quest) { return (operation << 24) | (quest & 0xFFFFFF); }

// Group quests (config QuestGroups, off by default): quests with a 'group <n>' marker are taken only when this is on; the bot then waits for a party of n.
struct QuestPlannerOptions
{
    bool GroupQuests = false;
    std::uint64_t GroupWaitMs = 600000;     // the wait for a party ends here: the quest is abandoned and left alone for kBlockAfterAbandonMs
    bool SkyborneExitQuests = false;        // config SkyborneExitQuests: quests with a 'hop' record are taken and hopped; off, they are unknown to the planner
};

// What CheckHop saw about a hop that was sent.
enum class HopCheck
{
    None,     // no hop pending, or still waiting for the core
    Done,     // the bot is on the hop's map
    NoMove,   // the bot is still on its map after kHopWaitMs: counted as refused (the quest is blocked)
};

// What CheckUse saw about an object use that was sent ('use' records).
enum class UseCheck
{
    None,     // no use pending, or still waiting
    Moved,    // the bot was moved (the portal worked)
    NoMove,   // the bot is still where it used the object after kUseWaitMs: a failed use (UseFails)
};

class QuestPlanner
{
public:
    // `data` is the list the planner picks new quests from. `alsoKnown` (the leveling list, for the level 60 planner) is only looked up: a quest of it
    // in the bot's log is still handed in, hunted and abandoned, but no new quest is taken from it.
    explicit QuestPlanner(std::shared_ptr<QuestData const> data, std::shared_ptr<QuestData const> alsoKnown = nullptr, QuestPlannerOptions options = {});
    QuestPlannerOptions const& Options() const { return _options; }

    bool Enabled() const { return _data && !_data->Starts.empty(); }

    // The step for this bot now.
    QuestStep Next(forever_module::BotSnapshot const& bot, QuestMemory& memory, std::uint64_t nowMs, QuestBotInfo const& info = {}) const;

    // The policy sent an accept, hand-in or abandon for `quest` (operation as in QuestCode).
    void NoteSent(QuestMemory& memory, std::uint32_t operation, std::uint32_t quest, std::uint64_t nowMs) const;

    // The result of the last quest action the core reported: a refusal blocks the quest for a while. Returns true when the refused action
    // was a hand-in: the reward did not fit (bags full), which a visit to a vendor fixes.
    // `reason` is the core's ActionReason of the refusal (0 = none given, the old behaviour) and `botLevel` the bot's level: a refused accept is then blocked
    // by its reason (never for class or race and already done; until the next level for level too low; until another quest is done for a prerequisite;
    // a short wait for a full log or full bags). A refused hand-in is not split by reason.
    bool NoteRefused(QuestMemory& memory, std::uint64_t nowMs, std::uint32_t reason = 0, std::uint32_t botLevel = 0) const;

    // The bot has been to a vendor (its bags are emptier): the quests blocked by refusals are tried again.
    void NoteServiced(QuestMemory& memory) const;

    // The core found no route for the move to the step's destination (kNoRoute). When that was the walk to a start's giver, the start is left
    // alone for kNoRouteBlockMs and the planner looks for the next start.
    // With `bot` given, a giver or ender within kQuestNearTryYards is tried from where the bot stands instead; the walk to a quest's ender or hunting
    // ground counts the answers (the hunting area moves on, and kNoRouteWalksBlock in a row block the quest).
    void NoteNoRoute(QuestMemory& memory, std::uint64_t nowMs, forever_module::BotSnapshot const* bot = nullptr) const;
    // The walk to a quest in the log (its ender, or its hunting ground) got nowhere for a long time: the quest is left alone for kWalkStuckBlockMs.
    void NoteWalkStuck(QuestMemory& memory, std::uint32_t quest, std::uint64_t nowMs) const;
    // LevelSafeWalks (level_safe.h): the step's walk goes to or through a place far above the bot's level. A hunt moves on to the quest's next hunting area
    // while it has one not tried yet; otherwise the quest (a giver, an ender, or every hunting area) is left alone for kLevelUnsafeBlockMs and the planner
    // gives another step (another quest, another start) on the next call.
    void NoteLevelUnsafe(QuestMemory& memory, std::uint32_t quest, QuestStepKind kind, std::uint64_t nowMs) const;

    // Skyborne exit quests (SkyborneExitQuests). The policy sent the hop teleport of `quest` to `map`.
    // `sameMap`: the hop stays on the bot's map (the portal fallback); it has landed when the bot is within kPortalMovedYards of `dest`.
    void NoteHopSent(QuestMemory& memory, std::uint32_t quest, std::uint32_t map, std::uint64_t nowMs, bool sameMap = false, QuestPoint dest = {}) const;
    // The core refused the hop teleport (Bot.Actions.Teleport off, in combat, ...): the quest is blocked, kHopRefusedBlockMs, then six hours, then for good.
    // Returns the number of refusals of that quest so far (0 when no hop was pending).
    std::uint32_t NoteHopRefused(QuestMemory& memory, std::uint64_t nowMs) const;
    // Whether the pending hop has landed (the bot is on its map) or has gone unanswered too long (then it is refused as by NoteHopRefused).
    HopCheck CheckHop(forever_module::BotSnapshot const& bot, QuestMemory& memory, std::uint64_t nowMs) const;
    // The 'use' records (94947). The policy sent kUseObject for `quest` with the bot standing as in `bot`.
    void NoteUseSent(QuestMemory& memory, std::uint32_t quest, forever_module::BotSnapshot const& bot, std::uint64_t nowMs) const;
    // The core refused the use (Bot.Actions.UseObject off, out of range, no object): a failed use. Returns the failed uses of that quest (0 when none was pending).
    std::uint32_t NoteUseRefused(QuestMemory& memory) const;
    // Whether the pending use has moved the bot (the portal worked) or has done nothing for kUseWaitMs (a failed use).
    UseCheck CheckUse(forever_module::BotSnapshot const& bot, QuestMemory& memory, std::uint64_t nowMs) const;
    // A quest of the Skyborne exit: one with an allowed hop, or the 'prev' of one (93089, 93090), or the 'use' quest after one (94947). False when
    // SkyborneExitQuests is off.
    bool ExitChain(std::uint32_t quest) const;
    // The accept of `quest` needs room in the bags (SkyborneExitMakeRoom): the vendor visit that follows lifts its block (as for a refused hand-in).
    void NoteNeedsRoom(QuestMemory& memory, std::uint32_t quest) const;

private:
    int FindStart(forever_module::BotSnapshot const& bot) const;
    // Whether the bot may take the quest now (everything but the log, done and blocked checks).
    bool Eligible(QuestDef const& quest, forever_module::BotSnapshot const& bot, QuestBotInfo const& info) const;
    QuestDef const* Lookup(std::uint32_t quest) const;
    // A quest the planner does not know: a hop it may not use, or a 'use' quest that does not follow a usable hop.
    bool Hidden(QuestDef const& quest) const;
    // Whether the bot's log or memory says the quest is done.
    static bool Taken(QuestMemory const& memory, forever_module::BotSnapshot const& bot, std::uint32_t quest, std::uint64_t nowMs);

    QuestPlannerOptions _options;
    std::shared_ptr<QuestData const> _data;
    std::shared_ptr<QuestData const> _alsoKnown; // kept alive for the pointers in _index
    std::set<std::uint32_t> _prerequisites;          // quests that are the 'prev' of another one (path variety never skips them)
    std::set<std::uint32_t> _exitChain;              // SkyborneExitQuests: the hop quests, their 'prev' quests and the 'use' quests after them
    std::set<std::uint32_t> _useQuests;              // SkyborneExitQuests: the 'use' quests whose 'prev' is a usable hop quest
    std::map<std::uint32_t, QuestDef const*> _index; // quest id -> its first definition (in `data`, else in `alsoKnown`); the lists are immutable
};
