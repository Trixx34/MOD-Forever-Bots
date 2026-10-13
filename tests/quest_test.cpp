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

// ---- policy ----

BotPolicy QuestPolicy()
{
    PolicyOptions options;
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
    auto actions = policy.Decide(OneBotView(bot, { Mob(99, 1, -8890.0f, -165.0f) }), 1000);
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

void ShippedFileParsesWithoutWarnings()
{
    std::vector<std::string> warnings;
    QuestData const data = LoadQuestData(QUEST_DATA_FILE, warnings);
    for (std::string const& warning : warnings)
        std::printf("warning: %s\n", warning.c_str());
    CHECK(warnings.empty());
    CHECK(data.Starts.size() == 5);
    for (QuestStartDef const& start : data.Starts)
    {
        CHECK(start.HasAnchor && !start.Quests.empty());
        for (QuestDef const& quest : start.Quests)
            if (!quest.Objectives.empty())
                CHECK(!quest.Areas.empty()); // a quest with targets has somewhere to look for them
    }
}

} // namespace

int main()
{
    ShippedFileParsesWithoutWarnings();
    ParserReadsTheFormat();
    ParserDropsBadLines();
    FarBotWalksToTheGiverThenAccepts();
    CompleteQuestIsHandedInThenTheNextIsAccepted();
    HuntsTheTargetsAndWalksBetweenAreas();
    ObjectiveDoneMeansHandInNotHunt();
    NoProgressForTenMinutesAbandons();
    RefusalBlocksTheQuestForAWhile();
    NoStartOnAnotherMapMeansNoQuests();
    PolicyWalksToTheGiverAndSendsTheAccept();
    PolicyHuntsOnlyTheQuestTargets();
    PolicyWalksToTheHuntingAreaWhenNoTargetIsNear();
    PolicyDefendsAgainstAnyAttackerWhileHunting();
    if (failures == 0)
        std::printf("quest_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
