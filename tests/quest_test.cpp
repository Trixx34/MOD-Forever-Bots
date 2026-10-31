// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the quest data parser, the planner and the policy's use of them (M5). No core needed.
#include "bot_policy.h"
#include "quest_data.h"
#include "quest_planner.h"

#include <cstdio>
#include <memory>
#include <sstream>
#include <string>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotSnapshot;
using forever_module::MakeUnitRef;
using forever_module::NearbyUnit;
using forever_module::WorldTickSnapshotView;

namespace {

// The pull and combat tests were written for bots that start Aggressive; the defensive default has its own test.
PolicyOptions AggressiveOptions()
{
    PolicyOptions options;
    options.AggressiveStart = true;
    return options;
}

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

char const* kData =
    "# Northshire\n"
    "start Human\n"
    "anchor 0 -8902.6 -162.6 82.0\n"
    "quest 783 giver -8933.5 -136.5 83.4 ender -8902.6 -162.6 82.0\n"
    "quest 7 giver -8902.6 -162.6 82.0 ender -8902.6 -162.6 82.0\n"
    "hunt 7 0 10 6\n"
    "area 7 -8796.8 -173.2 81.7\n"
    "area 7 -8810.7 -216.7 83.8\n";

std::shared_ptr<QuestData const> Data()
{
    std::istringstream in(kData);
    std::vector<std::string> warnings;
    auto data = std::make_shared<QuestData>(ParseQuestData(in, warnings));
    return data;
}

BotSnapshot Bot(float x, float y)
{
    BotSnapshot bot;
    bot.guid = 10;
    bot.health = 1000;
    bot.max_health = 1000;
    bot.level = 1;
    bot.map_id = 0;
    bot.pos_x = x;
    bot.pos_y = y;
    return bot;
}

void LogQuest(BotSnapshot& bot, std::uint32_t quest, std::uint8_t status, std::uint16_t count0 = 0)
{
    forever_module::QuestEntry& entry = bot.quests[bot.quest_count++];
    entry.quest_id = quest;
    entry.status = status;
    entry.counts[0] = count0;
}

void ParserReadsTheFormat()
{
    std::istringstream in(kData);
    std::vector<std::string> warnings;
    QuestData const data = ParseQuestData(in, warnings);
    CHECK(warnings.empty());
    CHECK(data.Starts.size() == 1);
    CHECK(data.Starts[0].Name == "Human" && data.Starts[0].HasAnchor && data.Starts[0].Map == 0);
    CHECK(data.Starts[0].Quests.size() == 2);
    QuestDef const& seven = data.Starts[0].Quests[1];
    CHECK(seven.Id == 7 && seven.Objectives.size() == 1 && seven.Objectives[0].Need == 10 && seven.Objectives[0].Entries[0] == 6);
    CHECK(seven.Areas.size() == 2);
}

void ParserDropsBadLines()
{
    std::istringstream in("quest 1 giver 0 0 0 ender 0 0 0\n"   // before any start
                          "start A\n"
                          "quest 5 giver 0 0\n"                  // incomplete
                          "hunt 9 0 1 3\n"                       // unknown quest
                          "frobnicate\n");
    std::vector<std::string> warnings;
    QuestData const data = ParseQuestData(in, warnings);
    CHECK(warnings.size() == 4);
    CHECK(data.Starts.size() == 1 && data.Starts[0].Quests.empty());
}

void FarBotWalksToTheGiverThenAccepts()
{
    QuestPlanner planner(Data());
    QuestMemory memory;
    QuestStep step = planner.Next(Bot(-8900.0f, -160.0f), memory, 1000);
    CHECK(step.Kind == QuestStepKind::WalkTo && step.Quest == 783);
    CHECK(step.Dest.X == -8933.5f);

    step = planner.Next(Bot(-8933.0f, -136.0f), memory, 2000);
    CHECK(step.Kind == QuestStepKind::Accept && step.Quest == 783);
    planner.NoteSent(memory, kQuestAccept, 783, 2000);
    step = planner.Next(Bot(-8933.0f, -136.0f), memory, 3000); // the log has not caught up
    CHECK(step.Kind == QuestStepKind::Wait);
}

void CompleteQuestIsHandedInThenTheNextIsAccepted()
{
    QuestPlanner planner(Data());
    QuestMemory memory;
    BotSnapshot bot = Bot(-8902.0f, -162.0f);
    LogQuest(bot, 783, 2);
    QuestStep step = planner.Next(bot, memory, 1000);
    CHECK(step.Kind == QuestStepKind::HandIn && step.Quest == 783);
    planner.NoteSent(memory, kQuestHandIn, 783, 1000);

    BotSnapshot after = Bot(-8902.0f, -162.0f); // the quest left the log
    step = planner.Next(after, memory, 9000);
    CHECK(memory.Done.count(783) == 1);
    CHECK(step.Kind == QuestStepKind::Accept && step.Quest == 7);
}

void HuntsTheTargetsAndWalksBetweenAreas()
{
    QuestPlanner planner(Data());
    QuestMemory memory;
    BotSnapshot bot = Bot(-8902.0f, -162.0f);
    LogQuest(bot, 7, 1, 3);
    QuestStep step = planner.Next(bot, memory, 1000);
    CHECK(step.Kind == QuestStepKind::Hunt && step.Entries.size() == 1 && step.Entries[0] == 6);
    CHECK(step.Dest.X == -8796.8f);

    // At the first area the bot moves on to the second.
    BotSnapshot there = Bot(-8796.0f, -173.0f);
    LogQuest(there, 7, 1, 3);
    step = planner.Next(there, memory, 2000);
    CHECK(step.Dest.X == -8810.7f);
}

void ObjectiveDoneMeansHandInNotHunt()
{
    QuestPlanner planner(Data());
    QuestMemory memory;
    BotSnapshot bot = Bot(-8902.0f, -162.0f);
    LogQuest(bot, 7, 2, 10);
    CHECK(planner.Next(bot, memory, 1000).Kind == QuestStepKind::HandIn);
}

void NoProgressForTenMinutesAbandons()
{
    QuestPlanner planner(Data());
    QuestMemory memory;
    BotSnapshot bot = Bot(-8802.0f, -170.0f);
    LogQuest(bot, 7, 1, 3);
    CHECK(planner.Next(bot, memory, 1000).Kind == QuestStepKind::Hunt);
    QuestStep step = planner.Next(bot, memory, 1000 + kStuckMs + 1);
    CHECK(step.Kind == QuestStepKind::Abandon && step.Quest == 7);
    planner.NoteSent(memory, kQuestAbandon, 7, 1000 + kStuckMs + 1);

    // A kill moves the clock: no abandon while the count grows.
    QuestMemory fresh;
    BotSnapshot a = Bot(-8802.0f, -170.0f);
    LogQuest(a, 7, 1, 3);
    planner.Next(a, fresh, 1000);
    BotSnapshot b = Bot(-8802.0f, -170.0f);
    LogQuest(b, 7, 1, 4);
    planner.Next(b, fresh, 500000);
    CHECK(planner.Next(b, fresh, 1000 + kStuckMs + 1).Kind == QuestStepKind::Hunt);
}

void RefusalBlocksTheQuestForAWhile()
{
    QuestPlanner planner(Data());
    QuestMemory memory;
    BotSnapshot bot = Bot(-8933.0f, -136.0f);
    planner.NoteSent(memory, kQuestAccept, 783, 1000);
    planner.NoteRefused(memory, 1000);
    QuestStep step = planner.Next(bot, memory, 10000);
    CHECK(step.Kind == QuestStepKind::WalkTo && step.Quest == 7); // 783 is blocked, the next quest is tried
    step = planner.Next(bot, memory, 1000 + kBlockAfterRefusalMs + 1);
    CHECK(step.Quest == 783);
}

void NoStartOnAnotherMapMeansNoQuests()
{
    QuestPlanner planner(Data());
    QuestMemory memory;
    BotSnapshot bot = Bot(-8900.0f, -160.0f);
    bot.map_id = 1;
    CHECK(planner.Next(bot, memory, 1000).Kind == QuestStepKind::None);
}


// ---- whole-world files (req records, several starts) ----

char const* kWorld =
    "start Elwynn\n"
    "anchor 0 -9000 -100 60\n"
    "quest 100 giver -9000 -100 60 ender -9000 -100 60\n"
    "req 100 level 5\n"
    "quest 101 giver -9000 -100 60 ender -9000 -100 60\n"
    "req 101 level 6 prev 100 minlevel 5\n"
    "quest 102 giver -9000 -100 60 ender -9000 -100 60\n"
    "req 102 level 7 classes 128\n"                     // mages only (class id 8: bit 7)
    "start Westfall\n"
    "anchor 0 -10000 1000 30\n"
    "quest 200 giver -10000 1000 30 ender -10000 1000 30\n"
    "req 200 level 12 minlevel 10\n"
    "quest 201 giver -10000 1000 30 ender -10000 1000 30\n"
    "req 201 level 14\n"
    "start Durotar\n"
    "anchor 1 300 -4000 20\n"
    "quest 300 giver 300 -4000 20 ender 300 -4000 20\n"
    "req 300 level 5 races 2\n";                        // orcs only (race id 2: bit 1)

std::shared_ptr<QuestData const> World()
{
    std::istringstream in(kWorld);
    std::vector<std::string> warnings;
    auto data = std::make_shared<QuestData>(ParseQuestData(in, warnings));
    CHECK(warnings.empty());
    return data;
}

BotSnapshot WorldBot(std::uint32_t level, float x = -9000.0f, float y = -100.0f)
{
    BotSnapshot bot = Bot(x, y);
    bot.level = level;
    return bot;
}

void ReqRecordsAreParsed()
{
    auto const data = World();
    QuestDef const& q = data->Starts[0].Quests[1];
    CHECK(q.Id == 101 && q.QuestLevel == 6 && q.PrevQuest == 100 && q.MinLevel == 5);
    CHECK(data->Starts[0].Quests[2].ClassMask == 128 && data->Starts[2].Quests[0].RaceMask == 2);
    std::istringstream bad("start A\nquest 1 giver 0 0 0 ender 0 0 0\nreq 1 level\nreq 9 level 3\nreq 1 colour 4\n");
    std::vector<std::string> warnings;
    ParseQuestData(bad, warnings);
    CHECK(warnings.size() == 3);
}

void ClassQuestInLogIsRewardedFromAnywhere()
{
    // A class quest (a reward record) is completed with its reward where the bot stands, no walk and no objectives.
    std::istringstream in("start A\nanchor 0 0 0 0\nquest 500 giver 0 0 0 ender 0 0 0\nreq 500 classes 8\nreward 500\n");
    std::vector<std::string> warnings;
    auto data = std::make_shared<QuestData>(ParseQuestData(in, warnings));
    CHECK(warnings.empty() && data->Starts[0].Quests[0].Reward);
    QuestPlanner planner(data);
    QuestMemory memory;
    BotSnapshot bot = Bot(-9000.0f, -100.0f);
    LogQuest(bot, 500, 1);
    QuestStep step = planner.Next(bot, memory, 1000);
    CHECK(step.Kind == QuestStepKind::Reward && step.Quest == 500);
    planner.NoteSent(memory, kQuestReward, 500, 1000);
    step = planner.Next(bot, memory, 1001); // the log has not caught up
    CHECK(step.Kind == QuestStepKind::Wait);
    BotSnapshot after = Bot(-9000.0f, -100.0f); // the reward left the log
    planner.Next(after, memory, 2000);
    CHECK(memory.Done.count(500) == 1);
}

void LevelLimitsDecideWhichQuestsAreTaken()
{
    QuestPlanner planner(World());
    QuestMemory memory;
    // A level 5 bot: 100 (level 5) is fine, 101 needs its predecessor first, Westfall's quests are red (level 12+).
    QuestStep step = planner.Next(WorldBot(5), memory, 1000);
    CHECK(step.Quest == 100);
    // A level 18 bot is far above Elwynn's quests (grey: 18 > 5 + 7) and takes Westfall's (level 12 and 14).
    QuestMemory second;
    step = planner.Next(WorldBot(18), second, 1000);
    CHECK(step.Kind == QuestStepKind::WalkTo && step.Quest == 200); // it has to walk there
}

void ClassAndRaceLimitsApply()
{
    QuestPlanner planner(World());
    QuestMemory memory;
    BotSnapshot bot = WorldBot(7);
    // A rogue (class 4) cannot take 102; a mage (class 8) can. 100 and 101 are first in the data, so block them for this check.
    memory.Done = { 100, 101 };
    CHECK(planner.Next(bot, memory, 1000, QuestBotInfo{ 4, 1 }).Quest != 102);
    QuestMemory mage;
    mage.Done = { 100, 101 };
    CHECK(planner.Next(bot, mage, 1000, QuestBotInfo{ 8, 1 }).Quest == 102);
    // Durotar's quest is for orcs on map 1.
    BotSnapshot durotar = WorldBot(5, 300.0f, -4000.0f);
    durotar.map_id = 1;
    QuestMemory human;
    CHECK(planner.Next(durotar, human, 1000, QuestBotInfo{ 1, 1 }).Kind == QuestStepKind::None);
    QuestMemory orc;
    CHECK(planner.Next(durotar, orc, 1000, QuestBotInfo{ 1, 2 }).Quest == 300);
}

void PrerequisiteQuestsComeAfterTheirPredecessors()
{
    QuestPlanner planner(World());
    QuestMemory memory;
    BotSnapshot bot = WorldBot(6);
    memory.Done = { 102 };
    // 100 is not done: 101 (prev 100) waits behind it even though both are open.
    CHECK(planner.Next(bot, memory, 1000).Quest == 100);
    // After a restart the memory is empty and 100 may be long done (the core refuses it): 101 is then the next one tried.
    planner.NoteSent(memory, kQuestAccept, 100, 1000);
    planner.NoteRefused(memory, 1000);
    CHECK(planner.Next(bot, memory, 5000).Quest == 101);
}

void FinishedZoneSendsTheBotToTheNext()
{
    QuestPlanner planner(World());
    QuestMemory memory;
    BotSnapshot bot = WorldBot(11);
    memory.Done = { 100, 101, 102 };
    QuestStep step = planner.Next(bot, memory, 1000);
    CHECK(step.Kind == QuestStepKind::WalkTo && step.Quest == 200); // Westfall: level 12 quest for an 11 (not red), minlevel 10 met
    CHECK(step.Dest.X == -10000.0f);
    bot.pos_x = -10000.0f;
    bot.pos_y = 1000.0f;
    CHECK(planner.Next(bot, memory, 2000).Kind == QuestStepKind::Accept);
}

void LogFullMeansNoNewQuest()
{
    QuestPlanner planner(World());
    QuestMemory memory;
    BotSnapshot bot = WorldBot(5);
    for (std::uint32_t i = 0; i < kMaxAcceptedQuests; ++i)
        LogQuest(bot, 9000 + i, 1); // quests the data does not know
    CHECK(planner.Next(bot, memory, 1000).Kind == QuestStepKind::None);
}

void QuestsOfAnotherStartInTheLogAreStillWorked()
{
    QuestPlanner planner(World());
    QuestMemory memory;
    BotSnapshot bot = WorldBot(12, -9500.0f, 0.0f);
    LogQuest(bot, 200, 2); // a Westfall quest, complete, while the bot stands in Elwynn
    QuestStep step = planner.Next(bot, memory, 1000);
    CHECK(step.Kind == QuestStepKind::WalkTo && step.Quest == 200 && step.Dest.X == -10000.0f);
}

void RefusalsBlockLongerEachTime()
{
    QuestPlanner planner(World());
    QuestMemory memory;
    planner.NoteSent(memory, kQuestAccept, 100, 0);
    planner.NoteRefused(memory, 0);
    CHECK(memory.BlockedUntilMs[100] == kBlockAfterRefusalMs);
    planner.NoteSent(memory, kQuestAccept, 100, 100000);
    planner.NoteRefused(memory, 100000);
    CHECK(memory.BlockedUntilMs[100] == 100000 + 10 * kBlockAfterRefusalMs);
    planner.NoteSent(memory, kQuestAccept, 100, 1000000);
    planner.NoteRefused(memory, 1000000);
    CHECK(memory.BlockedUntilMs[100] == 1000000 + kBlockAfterAbandonMs);
}


void WideMasksAreCutOrTreatedAsAny()
{
    std::istringstream in(
        "start A\nanchor 0 0 0 0\n"
        "quest 1 giver 0 0 0 ender 0 0 0\nreq 1 level 55 races 18446744073709551616 classes 2\n"   // too large to read: any race, the class limit stays
        "quest 2 giver 0 0 0 ender 0 0 0\nreq 2 level 5 races 18446744073709551615\n"              // all 64 bits set: cut to 32 bits
        "quest 3 giver 0 0 0 ender 0 0 0\nreq 3 level 5 races 77\n"
        "quest 4 giver 0 0 0 ender 0 0 0\nreq 4 level 99999999999\n");                           // not a mask: too wide, the record is dropped
    std::vector<std::string> warnings;
    QuestData const data = ParseQuestData(in, warnings);
    CHECK(data.Starts[0].Quests[0].QuestLevel == 55 && data.Starts[0].Quests[0].RaceMask == 0 && data.Starts[0].Quests[0].ClassMask == 2);
    CHECK(data.Starts[0].Quests[1].RaceMask == 0xFFFFFFFFu);
    CHECK(data.Starts[0].Quests[2].RaceMask == 77);
    CHECK(warnings.size() == 1);
}

// ---- policy ----

BotPolicy QuestPolicy()
{
    PolicyOptions options = AggressiveOptions();
    options.Quests = Data();
    return BotPolicy(options);
}

WorldTickSnapshotView OneBotView(BotSnapshot bot, std::vector<NearbyUnit> const& nearby)
{
    WorldTickSnapshotView view;
    view.tick_number = 1;
    view.max_nearby_units = 4;
    bot.nearby_count = static_cast<std::uint32_t>(nearby.size());
    view.bots.push_back(bot);
    view.nearby.resize(view.max_nearby_units);
    for (std::size_t i = 0; i < nearby.size(); ++i)
        view.nearby[i] = nearby[i];
    return view;
}

// A walk longer than the mount threshold spends its first tick mounting up: kMount, then kStop instead of the walk. The walk to the
// destination follows on the next tick. BotMountsForALongWalk in policy_test covers the mount itself.
std::vector<BotAction> AfterMount(BotPolicy& policy, WorldTickSnapshotView const& view, std::uint64_t nowMs)
{
    policy.Decide(view, nowMs);
    return policy.Decide(view, nowMs + 1);
}


NearbyUnit Mob(std::uint32_t entry, std::uint64_t counter, float x, float y)
{
    NearbyUnit unit;
    unit.guid = MakeUnitRef(8, entry, counter);
    unit.pos_x = x;
    unit.pos_y = y;
    unit.health = 100;
    unit.is_hostile = true;
    unit.level = 1;
    return unit;
}

bool Has(std::vector<BotAction> const& actions, ActionType type)
{
    for (BotAction const& action : actions)
        if (action.type == type)
            return true;
    return false;
}

void PolicyWalksToTheGiverAndSendsTheAccept()
{
    BotPolicy policy = QuestPolicy();
    auto far = policy.Decide(OneBotView(Bot(-8900.0f, -160.0f), {}), 1000);
    CHECK(Has(far, ActionType::kMoveTo) && !Has(far, ActionType::kQuest));

    auto near = policy.Decide(OneBotView(Bot(-8933.0f, -136.0f), {}), 2000);
    bool sent = false;
    for (BotAction const& action : near)
        if (action.type == ActionType::kQuest)
        {
            sent = true;
            CHECK(action.spell_id == QuestCode(kQuestAccept, 783));
        }
    CHECK(sent);
}

void PolicyHuntsOnlyTheQuestTargets()
{
    BotPolicy policy = QuestPolicy();
    BotSnapshot bot = Bot(-8800.0f, -170.0f);
    LogQuest(bot, 7, 1, 3);
    // A boar (entry 99) is closer than the kobold (entry 6): the bot goes for the kobold.
    auto actions = policy.Decide(OneBotView(bot, { Mob(99, 1, -8798.0f, -170.0f), Mob(6, 2, -8790.0f, -170.0f) }), 1000);
    bool attack = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kAttackTarget || action.type == ActionType::kFollow)
        {
            attack = true;
            CHECK(forever_module::UnitRefEntry(action.target_guid) == 6);
        }
    CHECK(attack);
}

void PolicyWalksToTheHuntingAreaWhenNoTargetIsNear()
{
    BotPolicy policy = QuestPolicy();
    BotSnapshot bot = Bot(-8900.0f, -165.0f);
    LogQuest(bot, 7, 1, 3);
    // The hunting area is 103 yards away, over the mount threshold, so the first tick mounts and the walk follows on the next one.
    WorldTickSnapshotView const view = OneBotView(bot, { Mob(99, 1, -8890.0f, -165.0f) });
    auto const actions = AfterMount(policy, view, 1000);
    bool moved = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo)
        {
            moved = true;
            CHECK(action.dest_x == -8796.8f);
        }
    CHECK(moved);
    CHECK(!Has(actions, ActionType::kAttackTarget)); // the boar is not the quest's
}

void PolicyDefendsAgainstAnyAttackerWhileHunting()
{
    BotPolicy policy = QuestPolicy();
    BotSnapshot bot = Bot(-8800.0f, -170.0f);
    bot.in_combat = 1;
    LogQuest(bot, 7, 1, 3);
    auto actions = policy.Decide(OneBotView(bot, { Mob(99, 1, -8799.0f, -170.0f) }), 1000);
    CHECK(Has(actions, ActionType::kAttackTarget) || Has(actions, ActionType::kFollow));
}

// A hand-in the core refuses (the reward does not fit: bags full) sends the bot to a vendor at once, even though it has quest work and so is never
// idle, and the hand-in is tried again after the visit instead of waiting out the block.
void RefusedHandInSendsTheBotToAVendor()
{
    PolicyOptions options = AggressiveOptions();
    options.Quests = Data();
    options.ServiceEveryMs = 600000;
    BotPolicy policy(options);
    auto const botAt = [](float x)
    {
        BotSnapshot bot = Bot(x, -162.6f);
        LogQuest(bot, 783, 2); // complete
        bot.vendor_valid = 1;
        bot.vendor_x = -8800.0f;
        bot.vendor_y = -162.6f;
        bot.vendor_z = 82.0f;
        return bot;
    };
    auto const kind = [](std::vector<BotAction> const& actions, ActionType type)
    {
        for (BotAction const& action : actions)
            if (action.type == type)
                return &action;
        return static_cast<BotAction const*>(nullptr);
    };

    auto first = policy.Decide(OneBotView(botAt(-8902.6f), {}), 1000);
    BotAction const* handIn = kind(first, ActionType::kQuest);
    CHECK(handIn && handIn->spell_id == QuestCode(kQuestHandIn, 783));
    CHECK(!kind(first, ActionType::kVendor));

    BotSnapshot refused = botAt(-8902.6f);
    refused.last_action_sequence = 1;
    refused.last_action_type = std::uint8_t(ActionType::kQuest);
    refused.last_action_status = forever_module::BotActionStatus::kRefused;
    WorldTickSnapshotView const walking = OneBotView(refused, {});
    auto const walk = AfterMount(policy, walking, 5000); // 103 yards to the vendor: mounts first, then walks
    BotAction const* move = kind(walk, ActionType::kMoveTo);
    CHECK(move && move->dest_x == -8800.0f); // to the vendor, not back to the quest NPC
    for (BotAction const& action : walk)
        CHECK(!(action.type == ActionType::kQuest && action.spell_id == QuestCode(kQuestHandIn, 783))); // blocked for now

    BotSnapshot atVendor = botAt(-8800.0f);
    atVendor.last_action_sequence = 1;
    atVendor.last_action_type = std::uint8_t(ActionType::kQuest);
    atVendor.last_action_status = forever_module::BotActionStatus::kRefused;
    CHECK(kind(policy.Decide(OneBotView(atVendor, {}), 9000), ActionType::kVendor));

    BotSnapshot back = botAt(-8902.6f);
    back.last_action_sequence = 1;
    back.last_action_type = std::uint8_t(ActionType::kQuest);
    back.last_action_status = forever_module::BotActionStatus::kRefused;
    auto const afterwards = policy.Decide(OneBotView(back, {}), 10000);
    bool again = false;
    for (BotAction const& action : afterwards)
        again = again || (action.type == ActionType::kQuest && action.spell_id == QuestCode(kQuestHandIn, 783));
    CHECK(again); // not blocked for a minute any more
}

// Three bots sent a refused quest action every three seconds for two hours. A run of refusals (four within five minutes, each answer counted once) pauses the
// bot's quest actions for ten minutes, whichever quests they were for.
void ARunOfRefusedQuestActionsPausesTheQuestEngine()
{
    PolicyOptions options = AggressiveOptions();
    options.Quests = Data();
    bool paused = false;
    options.OnEvent = [&](std::string const& text) { paused = paused || text.find("quest_paused") != std::string::npos; };
    BotPolicy policy(options);
    BotSnapshot bot = Bot(-8902.6f, -162.6f);
    LogQuest(bot, 783, 2);
    std::uint64_t now = 1000;
    for (std::uint32_t sequence = 1; sequence <= 4; ++sequence, now += 4000)
    {
        bot.last_action_sequence = sequence;
        bot.last_action_type = std::uint8_t(ActionType::kQuest);
        bot.last_action_status = forever_module::BotActionStatus::kRefused;
        policy.Decide(OneBotView(bot, {}), now);
    }
    CHECK(paused);
    bool sent = false;
    for (std::uint64_t at = now; at < now + 540000; at += 5000)
        for (BotAction const& action : policy.Decide(OneBotView(bot, {}), at))
            sent = sent || action.type == ActionType::kQuest;
    CHECK(!sent);
}

void HandInRefusalsBlockLessThanOtherRefusals()
{
    QuestPlanner planner(Data());
    QuestMemory memory;
    planner.NoteSent(memory, kQuestHandIn, 783, 0);
    CHECK(planner.NoteRefused(memory, 0));
    CHECK(memory.BlockedUntilMs[783] == kBlockAfterRefusalMs);
    planner.NoteSent(memory, kQuestHandIn, 783, 100000);
    CHECK(planner.NoteRefused(memory, 100000));
    CHECK(memory.BlockedUntilMs[783] == 100000 + kBlockAfterRefusalMs); // the second is still a minute
    planner.NoteSent(memory, kQuestHandIn, 783, 200000);
    planner.NoteRefused(memory, 200000);
    CHECK(memory.BlockedUntilMs[783] == 200000 + 10 * kBlockAfterRefusalMs);
    planner.NoteServiced(memory);
    CHECK(memory.BlockedUntilMs.count(783) == 0);
    // An accept refusal is not a hand-in refusal.
    planner.NoteSent(memory, kQuestAccept, 7, 300000);
    CHECK(!planner.NoteRefused(memory, 300000));
}

// The giver of quest 783. A stuck walk must not keep sending the bot there.
bool WalksToGiver(std::vector<BotAction> const& actions)
{
    for (BotAction const& action : actions)
        if ((action.type == ActionType::kMoveTo || action.type == ActionType::kTeleport) && action.dest_x < -8930.0f)
            return true;
    return false;
}

// Teleport is off (the sim default). A walk that does not move is dropped, not retried as a teleport the core will refuse.
void StuckQuestWalkIsDroppedWhenTeleportIsOff()
{
    PolicyOptions options = AggressiveOptions();
    options.Quests = Data();
    options.WanderWhenIdle = false;
    BotPolicy policy(options);
    BotSnapshot const bot = Bot(-8900.0f, -160.0f);
    CHECK(WalksToGiver(policy.Decide(OneBotView(bot, {}), 0)));
    auto const stuck = policy.Decide(OneBotView(bot, {}), 120000);
    CHECK(!Has(stuck, ActionType::kTeleport));
    CHECK(!WalksToGiver(stuck));
    CHECK(!WalksToGiver(policy.Decide(OneBotView(bot, {}), 130000)));
}

// Teleport on: one attempt, then a refusal drops the start. A silent core does not get a second teleport.
void StuckQuestWalkTeleportsOnceThenDropsTheStart()
{
    PolicyOptions options = AggressiveOptions();
    options.Quests = Data();
    options.QuestTeleport = true;
    options.WanderWhenIdle = false;
    BotPolicy policy(options);
    BotSnapshot bot = Bot(-8900.0f, -160.0f);
    policy.Decide(OneBotView(bot, {}), 0);
    auto const forced = policy.Decide(OneBotView(bot, {}), 120000);
    CHECK(Has(forced, ActionType::kTeleport));

    bot.last_action_sequence = 1;
    bot.last_action_type = std::uint8_t(ActionType::kTeleport);
    bot.last_action_status = forever_module::BotActionStatus::kRefused;
    auto const blocked = policy.Decide(OneBotView(bot, {}), 121000);
    CHECK(!Has(blocked, ActionType::kTeleport));
    CHECK(!WalksToGiver(blocked));

    PolicyOptions again = options;
    BotPolicy silent(again);
    BotSnapshot still = Bot(-8900.0f, -160.0f);
    silent.Decide(OneBotView(still, {}), 0);
    CHECK(Has(silent.Decide(OneBotView(still, {}), 120000), ActionType::kTeleport));
    auto const second = silent.Decide(OneBotView(still, {}), 240000);
    CHECK(!Has(second, ActionType::kTeleport));
    CHECK(!WalksToGiver(second));
}

} // namespace

// ---- no route and far world starts ----

void WorldStartFarAwayIsNotWalkedTo()
{
    // A world start (its quests carry levels) is walked to only up to kWorldStartRadiusYards; this one is about 15000 yards away.
    std::istringstream in("start Far\nanchor 0 5000 5000 0\nquest 500 giver 5000 5000 0 ender 5000 5000 0\nreq 500 level 5\n");
    std::vector<std::string> warnings;
    auto data = std::make_shared<QuestData>(ParseQuestData(in, warnings));
    QuestPlanner planner(data);
    QuestMemory memory;
    CHECK(planner.Next(WorldBot(5), memory, 1000).Kind == QuestStepKind::None);
}

void NoRouteToAStartGiverBlocksThatStart()
{
    QuestPlanner planner(World());
    QuestMemory memory;
    BotSnapshot const bot = WorldBot(5, -9300.0f, -100.0f); // 300 yards from the giver: a walk, not an accept
    QuestStep step = planner.Next(bot, memory, 1000);
    CHECK(step.Kind == QuestStepKind::WalkTo && step.Quest == 100);
    planner.NoteNoRoute(memory, 2000);
    CHECK(planner.Next(bot, memory, 3000).Kind == QuestStepKind::None); // the only start in reach is blocked
    step = planner.Next(bot, memory, 2000 + kNoRouteBlockMs + 1);
    CHECK(step.Kind == QuestStepKind::WalkTo && step.Quest == 100); // unblocked when the block runs out
}

void NoRouteAtTheGiverIsIgnored()
{
    QuestPlanner planner(Data());
    QuestMemory memory;
    CHECK(planner.Next(Bot(-8900.0f, -160.0f), memory, 1000).Kind == QuestStepKind::WalkTo);
    CHECK(planner.Next(Bot(-8933.0f, -136.0f), memory, 2000).Kind == QuestStepKind::Accept); // not a walk now
    planner.NoteNoRoute(memory, 2500);
    CHECK(memory.StartIndex == 0); // the start is kept: the planner was not walking to it
}

void AStuckWalkToAnEnderBlocksThatQuestForAWhile()
{
    QuestPlanner planner(World());
    QuestMemory memory;
    planner.NoteWalkStuck(memory, 100, 5000);
    CHECK(memory.BlockedUntilMs[100] == 5000 + kWalkStuckBlockMs);
    planner.NoteWalkStuck(memory, 0, 6000); // no quest: nothing is blocked
    CHECK(memory.BlockedUntilMs.count(0) == 0);
}

int main()
{
    AStuckWalkToAnEnderBlocksThatQuestForAWhile();
    StuckQuestWalkIsDroppedWhenTeleportIsOff();
    StuckQuestWalkTeleportsOnceThenDropsTheStart();
    WorldStartFarAwayIsNotWalkedTo();
    NoRouteToAStartGiverBlocksThatStart();
    NoRouteAtTheGiverIsIgnored();
    RefusedHandInSendsTheBotToAVendor();
    ARunOfRefusedQuestActionsPausesTheQuestEngine();
    HandInRefusalsBlockLessThanOtherRefusals();
    ParserReadsTheFormat();
    ParserDropsBadLines();
    FarBotWalksToTheGiverThenAccepts();
    CompleteQuestIsHandedInThenTheNextIsAccepted();
    HuntsTheTargetsAndWalksBetweenAreas();
    ObjectiveDoneMeansHandInNotHunt();
    NoProgressForTenMinutesAbandons();
    RefusalBlocksTheQuestForAWhile();
    NoStartOnAnotherMapMeansNoQuests();
    ReqRecordsAreParsed();
    ClassQuestInLogIsRewardedFromAnywhere();
    LevelLimitsDecideWhichQuestsAreTaken();
    ClassAndRaceLimitsApply();
    PrerequisiteQuestsComeAfterTheirPredecessors();
    FinishedZoneSendsTheBotToTheNext();
    LogFullMeansNoNewQuest();
    QuestsOfAnotherStartInTheLogAreStillWorked();
    RefusalsBlockLongerEachTime();
    WideMasksAreCutOrTreatedAsAny();
    PolicyWalksToTheGiverAndSendsTheAccept();
    PolicyHuntsOnlyTheQuestTargets();
    PolicyWalksToTheHuntingAreaWhenNoTargetIsNear();
    PolicyDefendsAgainstAnyAttackerWhileHunting();
    if (failures == 0)
        std::printf("quest_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
