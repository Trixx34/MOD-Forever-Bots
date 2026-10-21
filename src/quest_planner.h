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
// A quest with no progress for kStuckMs is abandoned and left alone for kBlockAfterAbandonMs. A refused accept or hand-in
// blocks that quest for kBlockAfterRefusalMs.
#pragma once

#include "quest_data.h"
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
    Wait,      // stand still: an action was just sent and the log has not caught up
};

struct QuestStep
{
    QuestStepKind Kind = QuestStepKind::None;
    std::uint32_t Quest = 0;
    QuestPoint Dest;
    std::vector<std::uint32_t> Entries;
    bool StartGiver = false; // the walk is to the giver of a quest start on the bot's map (not a turn-in or a hunt)
};

// What the planner remembers about one bot.
struct QuestMemory
{
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
    std::map<std::uint32_t, std::uint32_t> Refusals;    // quest -> how many accepts or hand-ins the core refused (the block grows with it)
    std::map<std::size_t, std::uint64_t> NoRouteUntilMs; // start index -> not walked to before this (the core found no route to its giver)
    bool WalkingToStart = false;                        // the step from the last Next is the walk to a start's giver
    std::set<std::uint32_t> RefusedHandIns;             // quests whose hand-in was refused since the last vendor visit
};

// Who the bot is, for the quests' race and class limits. 0 = unknown (no limit applied).
struct QuestBotInfo
{
    std::uint32_t ClassId = 0;
    std::uint32_t Race = 0;
};

constexpr float kQuestReachYards = 4.0f;                // the core allows 5 yards for talking to an NPC
constexpr float kAreaReachYards = 15.0f;                // a hunting area is searched when the bot is this close
constexpr std::uint64_t kQuestActionGapMs = 3000;       // wait this long after sending an accept or hand-in
constexpr std::uint64_t kBlockAfterRefusalMs = 60000;
constexpr std::uint64_t kStuckMs = 600000;              // no progress for 10 minutes: abandon
constexpr std::uint64_t kBlockAfterAbandonMs = 3600000;
constexpr std::uint64_t kWalkStuckBlockMs = 900000;          // a quest whose walk is stuck (no route to its ender or hunting ground) is left alone this long
constexpr float kWorldStartRadiusYards = 2000.0f;      // a world start (its quests carry levels) is walked to from up to this far (see kStartRadiusYards)
constexpr std::uint64_t kNoRouteBlockMs = 1800000;     // a start whose giver the core found no route to is left alone for this long
constexpr std::uint32_t kGreyQuestMargin = 7;          // a bot this many levels above a quest's level skips it (little experience)
constexpr std::uint32_t kRedQuestMargin = 4;           // a quest this many levels above the bot is left for later
constexpr std::uint32_t kMaxAcceptedQuests = 5;        // no new quest while the log holds this many
constexpr float kStartRadiusYards = 1500.0f;            // a bot farther than this from every anchor of its map has no start

// kQuest's spell_id: (operation << 24) | quest id.
constexpr std::uint32_t kQuestAccept = 1;
constexpr std::uint32_t kQuestHandIn = 2;
constexpr std::uint32_t kQuestAbandon = 3;
constexpr std::uint32_t QuestCode(std::uint32_t operation, std::uint32_t quest) { return (operation << 24) | (quest & 0xFFFFFF); }

class QuestPlanner
{
public:
    explicit QuestPlanner(std::shared_ptr<QuestData const> data);

    bool Enabled() const { return _data && !_data->Starts.empty(); }

    // The step for this bot now.
    QuestStep Next(forever_module::BotSnapshot const& bot, QuestMemory& memory, std::uint64_t nowMs, QuestBotInfo const& info = {}) const;

    // The policy sent an accept, hand-in or abandon for `quest` (operation as in QuestCode).
    void NoteSent(QuestMemory& memory, std::uint32_t operation, std::uint32_t quest, std::uint64_t nowMs) const;

    // The result of the last quest action the core reported: a refusal blocks the quest for a while. Returns true when the refused action
    // was a hand-in: the reward did not fit (bags full), which a visit to a vendor fixes.
    bool NoteRefused(QuestMemory& memory, std::uint64_t nowMs) const;

    // The bot has been to a vendor (its bags are emptier): the quests blocked by refusals are tried again.
    void NoteServiced(QuestMemory& memory) const;

    // The core found no route for the move to the step's destination (kNoRoute). When that was the walk to a start's giver, the start is left
    // alone for kNoRouteBlockMs and the planner looks for the next start.
    void NoteNoRoute(QuestMemory& memory, std::uint64_t nowMs) const;
    // The walk to a quest in the log (its ender, or its hunting ground) got nowhere for a long time: the quest is left alone for kWalkStuckBlockMs.
    void NoteWalkStuck(QuestMemory& memory, std::uint32_t quest, std::uint64_t nowMs) const;

private:
    int FindStart(forever_module::BotSnapshot const& bot) const;
    // Whether the bot may take the quest now (everything but the log, done and blocked checks).
    static bool Eligible(QuestDef const& quest, forever_module::BotSnapshot const& bot, QuestBotInfo const& info);
    QuestDef const* Lookup(std::uint32_t quest) const;
    // Whether the bot's log or memory says the quest is done.
    static bool Taken(QuestMemory const& memory, forever_module::BotSnapshot const& bot, std::uint32_t quest, std::uint64_t nowMs);

    std::shared_ptr<QuestData const> _data;
    std::map<std::uint32_t, std::pair<std::size_t, std::size_t>> _index; // quest id -> (start, quest) of its first definition
};
