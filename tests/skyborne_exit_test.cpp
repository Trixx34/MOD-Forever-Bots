// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the Skyborne exit quests (config SkyborneExitQuests, SkyborneExitMakeRoom; doc/world-quests.md): the 'hop' record of the quest data, the
// planner's walk to the dock and Hop step, a refused or silent teleport, the hand-in on the other map, and the vendor visit for full bags. No core needed.
#include "bot_policy.h"
#include "quest_data.h"
#include "quest_planner.h"

#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotActionStatus;
using forever_module::BotSnapshot;
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

// The Alliance exit as in data/world_quests_alliance.txt, cut down: 93089 before it, and a quest on map 0 after it.
char const* kData =
    "start W2991\n"
    "anchor 2991 1991.8 532.8 658.3\n"
    "quest 93089 giver 2118.8 550.3 735.3 ender 1991.9 525.0 658.0\n"
    "req 93089 level 13\n"
    "quest 94946 giver 1991.9 525.0 658.0 ender 449.0 439.0 111.3\n"
    "req 94946 level 13 minlevel 7 prev 93089\n"
    "hop 94946 0 448.96 439.35 111.33\n"
    "quest 92000 giver 1991.9 525.0 658.0 ender 1991.9 525.0 658.0\n"
    "req 92000 level 13\n"
    "start W0\n"
    "anchor 0 449.0 439.0 111.0\n"
    "quest 98021 giver 449.0 439.0 111.3 ender 449.0 439.0 111.3\n"
    "req 98021 level 13 prev 94946\n";

std::shared_ptr<QuestData const> Parse(char const* text, std::vector<std::string>* warningsOut = nullptr)
{
    std::istringstream in(text);
    std::vector<std::string> warnings;
    auto data = std::make_shared<QuestData>(ParseQuestData(in, warnings));
    if (warningsOut)
        *warningsOut = warnings;
    return data;
}

QuestPlannerOptions On()
{
    QuestPlannerOptions options;
    options.SkyborneExitQuests = true;
    return options;
}

BotSnapshot Bot(std::uint32_t map, float x, float y, float z, std::uint32_t level = 15)
{
    BotSnapshot bot;
    bot.guid = 10;
    bot.health = 1000;
    bot.max_health = 1000;
    bot.level = level;
    bot.map_id = map;
    bot.pos_x = x;
    bot.pos_y = y;
    bot.pos_z = z;
    bot.bag_free_slots = 10;
    return bot;
}

void LogQuest(BotSnapshot& bot, std::uint32_t quest, std::uint8_t status)
{
    forever_module::QuestEntry& entry = bot.quests[bot.quest_count++];
    entry.quest_id = quest;
    entry.status = status;
}

WorldTickSnapshotView View(BotSnapshot bot)
{
    WorldTickSnapshotView view;
    view.tick_number = 1;
    view.max_nearby_units = 4;
    bot.nearby_count = 0;
    view.bots.push_back(bot);
    view.nearby.resize(view.max_nearby_units);
    return view;
}

BotAction const* Find(std::vector<BotAction> const& actions, ActionType type)
{
    for (BotAction const& action : actions)
        if (action.type == type)
            return &action;
    return nullptr;
}

bool Sends(std::vector<BotAction> const& actions, std::uint32_t operation, std::uint32_t quest)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kQuest && action.spell_id == QuestCode(operation, quest))
            return true;
    return false;
}

// ---- parser ----

void ParserReadsTheHopRecord()
{
    std::vector<std::string> warnings;
    auto const data = Parse(kData, &warnings);
    CHECK(warnings.empty());
    CHECK(data->Starts.size() == 2);
    QuestDef const& exit = data->Starts[0].Quests[1];
    CHECK(exit.Id == 94946 && exit.HasHop && exit.HopFrom == 2991 && exit.HopMap == 0);
    CHECK(exit.Hop.X == 448.96f && exit.Hop.Y == 439.35f && exit.Hop.Z == 111.33f);
    CHECK(!exit.HasDock);
    CHECK(!data->Starts[0].Quests[0].HasHop);

    auto const docked = Parse("start S\nanchor 2991 0 0 0\nquest 95349 giver 1997.6 940.9 660.1 ender -658.0 426.6 187.2\n"
                              "hop 95349 1 -658.0 426.6 187.2 dock 1938.0 897.0 650.0\n", &warnings);
    QuestDef const& horde = docked->Starts[0].Quests[0];
    CHECK(warnings.empty() && horde.HasHop && horde.HopMap == 1 && horde.HasDock && horde.Dock.X == 1938.0f && horde.Dock.Z == 650.0f);
}

void ParserDropsBadHopLines()
{
    std::vector<std::string> warnings;
    auto const data = Parse("start S\n"
                            "hop 1 0 1 2 3\n"                                   // no anchor yet
                            "anchor 2991 0 0 0\n"
                            "quest 1 giver 0 0 0 ender 1 1 1\n"
                            "hop 2 0 1 2 3\n"                                   // no such quest
                            "hop 1 2991 1 2 3\n"                                // to the start's own map
                            "hop 1 0 1 2\n"                                     // no z
                            "hop 1 0 1 2 3 dock 4 5\n"                          // dock without z
                            "hop 1 0 1 2 3 boat 4 5 6\n"                        // not 'dock'
                            "hop 1 0 1 2 3 dock 4 5 6 7\n",                     // something after the dock
                            &warnings);
    CHECK(warnings.size() == 7);
    CHECK(!data->Starts[0].Quests[0].HasHop);
    for (std::string const& warning : warnings)
        CHECK(warning.find("hop") != std::string::npos || warning.find("before the first") != std::string::npos);
}

void TheShippedSideFilesParseWithTheirHop()
{
#ifdef FOREVER_BOTS_DATA_DIR
    for (auto const& [file, quest, map] : { std::tuple<char const*, std::uint32_t, std::uint32_t>{ "/world_quests_alliance.txt", 94946u, 0u },
                                            std::tuple<char const*, std::uint32_t, std::uint32_t>{ "/world_quests_horde.txt", 95349u, 1u } })
    {
        std::vector<std::string> warnings;
        QuestData const data = LoadQuestData(std::string(FOREVER_BOTS_DATA_DIR) + file, warnings);
        CHECK(warnings.empty());
        int hops = 0;
        for (QuestStartDef const& start : data.Starts)
            for (QuestDef const& def : start.Quests)
                if (def.HasHop)
                {
                    ++hops;
                    CHECK(def.Id == quest && def.HopFrom == 2991 && def.HopMap == map && HopAllowed(def.HopFrom, def.HopMap));
                }
        CHECK(hops == 1);
    }
#endif
}

// ---- planner ----

// Key off: the exit quest is unknown (today's behaviour): not taken, and in the log it gets no step (no walk to an ender on another map).
void KeyOffIgnoresTheExitQuest()
{
    QuestPlanner const planner(Parse(kData));
    QuestMemory memory;
    memory.Done.insert(93089);
    memory.Done.insert(92000);
    BotSnapshot bot = Bot(2991, 1991.9f, 525.0f, 658.0f);
    QuestStep const step = planner.Next(bot, memory, 1000);
    CHECK(step.Kind == QuestStepKind::None);
    CHECK(!planner.ExitChain(94946) && !planner.ExitChain(93089));

    LogQuest(bot, 94946, 2);
    QuestMemory logged;
    logged.Done = memory.Done;
    CHECK(planner.Next(bot, logged, 1000).Kind == QuestStepKind::None);
}

// Key on: the accept at the giver on 2991; in the log, the walk to the dock (the giver), the hop, the wait, the landing, and the hand-in on map 0.
void KeyOnWalksHopsAndHandsIn()
{
    QuestPlanner const planner(Parse(kData), nullptr, On());
    CHECK(planner.ExitChain(94946) && planner.ExitChain(93089) && !planner.ExitChain(92000));
    QuestMemory memory;
    memory.Done.insert(93089);
    memory.Done.insert(92000);
    BotSnapshot atGiver = Bot(2991, 1991.9f, 525.0f, 658.0f);
    QuestStep const accept = planner.Next(atGiver, memory, 1000);
    CHECK(accept.Kind == QuestStepKind::Accept && accept.Quest == 94946);

    // In the log, away from the giver: walk to it (a hop leg, so LevelSafeWalks leaves it alone).
    BotSnapshot away = Bot(2991, 2300.0f, 600.0f, 700.0f);
    LogQuest(away, 94946, 2);
    QuestStep const walk = planner.Next(away, memory, 5000);
    CHECK(walk.Kind == QuestStepKind::WalkTo && walk.Quest == 94946 && walk.HopLeg && !walk.StartGiver);
    CHECK(walk.Dest.X == 1991.9f && walk.Dest.Y == 525.0f);

    // At the giver: the hop.
    BotSnapshot dock = Bot(2991, 1993.0f, 527.0f, 658.0f);
    LogQuest(dock, 94946, 2);
    QuestStep const hop = planner.Next(dock, memory, 6000);
    CHECK(hop.Kind == QuestStepKind::Hop && hop.Quest == 94946 && hop.Map == 0 && hop.Dest.X == 448.96f && hop.Dest.Y == 439.35f && hop.HopLeg);
    planner.NoteHopSent(memory, 94946, 0, 6000);
    CHECK(planner.Next(dock, memory, 7000).Kind == QuestStepKind::Wait);
    CHECK(planner.CheckHop(dock, memory, 7000) == HopCheck::None);

    // Landed on map 0 at the ender: done, and the hand-in follows.
    BotSnapshot landed = Bot(0, 448.96f, 439.35f, 111.33f);
    LogQuest(landed, 94946, 2);
    CHECK(planner.CheckHop(landed, memory, 8000) == HopCheck::Done);
    CHECK(memory.HopPending == 0);
    QuestStep const handIn = planner.Next(landed, memory, 10000);
    CHECK(handIn.Kind == QuestStepKind::HandIn && handIn.Quest == 94946 && handIn.HopLeg);
    planner.NoteSent(memory, kQuestHandIn, 94946, 10000);

    // Handed in: the next quest is the one on map 0, never one back on 2991.
    BotSnapshot after = Bot(0, 448.96f, 439.35f, 111.33f);
    QuestStep const next = planner.Next(after, memory, 20000);
    CHECK(memory.Done.count(94946) == 1);
    CHECK(next.Kind == QuestStepKind::Accept && next.Quest == 98021);
}

// The hop is not offered when the quest is not complete in the log either: the hop comes before the objectives (a talk quest), and the stuck rule does not
// abandon a quest whose bot is on its way to the hop.
void TheHopComesBeforeTheStuckRule()
{
    QuestPlanner const planner(Parse(kData), nullptr, On());
    QuestMemory memory;
    BotSnapshot far = Bot(2991, 2300.0f, 600.0f, 700.0f);
    LogQuest(far, 94946, 1);
    for (std::uint64_t now = 0; now <= kStuckMs * 2; now += 60000)
    {
        QuestStep const step = planner.Next(far, memory, now);
        CHECK(step.Kind == QuestStepKind::WalkTo && step.Quest == 94946);
    }
}

// A dock in the data is walked to instead of the giver.
void ADockIsWalkedToInsteadOfTheGiver()
{
    QuestPlanner const planner(Parse("start S\nanchor 2991 1997.6 940.9 660.1\nquest 95349 giver 1997.6 940.9 660.1 ender -658.0 426.6 187.2\n"
                                     "hop 95349 1 -658.0 426.6 187.2 dock 1938.0 897.0 650.0\n"),
                               nullptr, On());
    QuestMemory memory;
    BotSnapshot atGiver = Bot(2991, 1997.6f, 940.9f, 660.1f);
    LogQuest(atGiver, 95349, 2);
    QuestStep const walk = planner.Next(atGiver, memory, 1000);
    CHECK(walk.Kind == QuestStepKind::WalkTo && walk.Dest.X == 1938.0f && walk.Dest.Y == 897.0f);
    BotSnapshot atDock = Bot(2991, 1940.0f, 898.0f, 650.0f);
    LogQuest(atDock, 95349, 2);
    QuestStep const hop = planner.Next(atDock, memory, 2000);
    CHECK(hop.Kind == QuestStepKind::Hop && hop.Map == 1 && hop.Dest.X == -658.0f);
}

// Only 2991 to 0 or 1: a hop from another map, or to another map, is unknown to the planner even with the key on.
void OnlyTheSkyborneMapPairsAreUsed()
{
    CHECK(HopAllowed(2991, 0) && HopAllowed(2991, 1));
    CHECK(!HopAllowed(2991, 530) && !HopAllowed(0, 1) && !HopAllowed(1, 0) && !HopAllowed(0, 2991));
    QuestPlanner const planner(Parse("start S\nanchor 0 0 0 0\nquest 7 giver 0 0 0 ender 5 5 5\nhop 7 1 5 5 5\n"
                                     "start T\nanchor 2991 100 100 0\nquest 8 giver 100 100 0 ender 5 5 5\nhop 8 530 5 5 5\n"),
                               nullptr, On());
    QuestMemory memory;
    CHECK(planner.Next(Bot(0, 0.0f, 0.0f, 0.0f), memory, 1000).Kind == QuestStepKind::None);
    CHECK(planner.Next(Bot(2991, 100.0f, 100.0f, 0.0f), memory, 1000).Kind == QuestStepKind::None);
    BotSnapshot logged = Bot(0, 0.0f, 0.0f, 0.0f);
    LogQuest(logged, 7, 2);
    CHECK(planner.Next(logged, memory, 1000).Kind == QuestStepKind::None);
    CHECK(!planner.ExitChain(7) && !planner.ExitChain(8));
}

// A refused hop blocks the quest: an hour, then six hours, then for good. Meanwhile the bot gets no hop (it grinds).
void ARefusedHopBlocksTheQuestLongerEachTime()
{
    QuestPlanner const planner(Parse(kData), nullptr, On());
    QuestMemory memory;
    memory.Done.insert(93089);
    memory.Done.insert(92000);
    BotSnapshot dock = Bot(2991, 1991.9f, 525.0f, 658.0f);
    LogQuest(dock, 94946, 2);
    std::uint64_t now = 1000;
    CHECK(planner.NoteHopRefused(memory, now) == 0); // nothing pending
    std::uint64_t const blocks[] = { kHopRefusedBlockMs, kBlockAfterAcceptRefusal3Ms };
    for (std::uint64_t block : blocks)
    {
        QuestStep const hop = planner.Next(dock, memory, now);
        CHECK(hop.Kind == QuestStepKind::Hop);
        planner.NoteHopSent(memory, 94946, 0, now);
        planner.NoteHopRefused(memory, now + 500);
        CHECK(memory.HopPending == 0);
        CHECK(planner.Next(dock, memory, now + 1000).Kind != QuestStepKind::Hop);
        CHECK(planner.Next(dock, memory, now + 500 + block - 1).Kind != QuestStepKind::Hop);
        now = now + 500 + block + kQuestActionGapMs;
    }
    QuestStep const third = planner.Next(dock, memory, now);
    CHECK(third.Kind == QuestStepKind::Hop);
    planner.NoteHopSent(memory, 94946, 0, now);
    CHECK(planner.NoteHopRefused(memory, now + 500) == kHopRefusalsForGood);
    CHECK(planner.Next(dock, memory, now + 30ull * 24 * 3600000).Kind != QuestStepKind::Hop); // for good
}

// A hop the core never answers (the bot stays on 2991) counts as refused after kHopWaitMs.
void ASilentHopCountsAsRefused()
{
    QuestPlanner const planner(Parse(kData), nullptr, On());
    QuestMemory memory;
    BotSnapshot dock = Bot(2991, 1991.9f, 525.0f, 658.0f);
    LogQuest(dock, 94946, 2);
    CHECK(planner.Next(dock, memory, 1000).Kind == QuestStepKind::Hop);
    planner.NoteHopSent(memory, 94946, 0, 1000);
    CHECK(planner.CheckHop(dock, memory, 1000 + kHopWaitMs - 1) == HopCheck::None);
    CHECK(planner.CheckHop(dock, memory, 1000 + kHopWaitMs) == HopCheck::NoMove);
    CHECK(memory.HopRefusals[94946] == 1);
    CHECK(planner.Next(dock, memory, 1000 + kHopWaitMs + 5000).Kind != QuestStepKind::Hop);
}

// The exit chain is taken when grey: a level 25 Skyborne may still leave. Other quests of the same level are skipped as before.
void TheExitChainIsTakenWhenGrey()
{
    QuestPlanner const planner(Parse(kData), nullptr, On());
    QuestMemory memory;
    BotSnapshot high = Bot(2991, 2118.8f, 550.3f, 735.3f, 25);
    QuestStep const first = planner.Next(high, memory, 1000);
    CHECK(first.Kind == QuestStepKind::Accept && first.Quest == 93089);
    memory.Done.insert(93089);
    BotSnapshot atGiver = Bot(2991, 1991.9f, 525.0f, 658.0f, 25);
    QuestStep const exit = planner.Next(atGiver, memory, 2000);
    CHECK(exit.Kind == QuestStepKind::Accept && exit.Quest == 94946); // 92000 (level 13, same giver) is grey and skipped

    QuestPlanner const off(Parse(kData));
    QuestMemory plain;
    CHECK(off.Next(high, plain, 1000).Kind == QuestStepKind::None);
}

// ---- policy ----

PolicyOptions Options(std::vector<std::string>& events)
{
    PolicyOptions options;
    options.AggressiveStart = true;
    options.WanderWhenIdle = false;
    options.Quests = Parse(kData);
    options.SkyborneExitQuests = true;
    options.ServiceEveryMs = 600000; // the module's default ServiceEverySec (600); the urgent visit uses the same vendor service
    options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
    return options;
}

bool Logged(std::vector<std::string> const& events, char const* prefix)
{
    for (std::string const& event : events)
        if (event.rfind(prefix, 0) == 0)
            return true;
    return false;
}

// At the dock the policy sends kTeleport to map 0 (spell_id = map) and logs the start; on map 0 it logs the landing and sends the hand-in.
void PolicySendsTheHopTeleportAndHandsInAfterIt()
{
    std::vector<std::string> events;
    BotPolicy policy(Options(events));
    BotSnapshot dock = Bot(2991, 1991.9f, 525.0f, 658.0f);
    LogQuest(dock, 94946, 2);
    auto const actions = policy.Decide(View(dock), 1000);
    BotAction const* teleport = Find(actions, ActionType::kTeleport);
    CHECK(teleport && teleport->spell_id == 0 && teleport->dest_x == 448.96f && teleport->dest_y == 439.35f && teleport->dest_z == 111.33f);
    CHECK(Logged(events, "skyborne_hop_start: bot=10 quest=94946"));
    CHECK(!Find(policy.Decide(View(dock), 2000), ActionType::kTeleport)); // one teleport, then the wait

    BotSnapshot landed = Bot(0, 448.96f, 439.35f, 111.33f);
    LogQuest(landed, 94946, 2);
    auto const after = policy.Decide(View(landed), 5000);
    CHECK(Logged(events, "skyborne_hop_done: bot=10"));
    CHECK(Sends(after, kQuestHandIn, 94946));
}

// The core refuses the teleport (Bot.Actions.Teleport = 0): an event, and no other hop for an hour (not every tick).
void PolicyBlocksTheQuestWhenTheTeleportIsRefused()
{
    std::vector<std::string> events;
    BotPolicy policy(Options(events));
    BotSnapshot dock = Bot(2991, 1991.9f, 525.0f, 658.0f);
    LogQuest(dock, 94946, 2);
    CHECK(Find(policy.Decide(View(dock), 1000), ActionType::kTeleport));
    dock.last_action_sequence = 1;
    dock.last_action_type = std::uint8_t(ActionType::kTeleport);
    dock.last_action_status = BotActionStatus::kRefused;
    policy.Decide(View(dock), 2000);
    CHECK(Logged(events, "skyborne_hop_refused: bot=10 quest=94946"));
    int teleports = 0;
    for (std::uint64_t now = 3000; now < 3000 + kHopRefusedBlockMs - 10000; now += 60000)
        teleports += Find(policy.Decide(View(dock), now), ActionType::kTeleport) ? 1 : 0;
    CHECK(teleports == 0);
}

// Key off: the policy never teleports a Skyborne with the exit quest in its log.
void PolicyKeyOffSendsNoHop()
{
    std::vector<std::string> events;
    PolicyOptions options = Options(events);
    options.SkyborneExitQuests = false;
    BotPolicy policy(options);
    BotSnapshot dock = Bot(2991, 1991.9f, 525.0f, 658.0f);
    LogQuest(dock, 94946, 2);
    for (std::uint64_t now = 1000; now < 200000; now += 20000)
        CHECK(!Find(policy.Decide(View(dock), now), ActionType::kTeleport));
    CHECK(!Logged(events, "skyborne_hop_"));
}

BotSnapshot FullBagsAtGiver(std::uint32_t quest)
{
    BotSnapshot bot = quest == 93089 ? Bot(2991, 2118.8f, 550.3f, 735.3f) : Bot(2991, 1991.9f, 525.0f, 658.0f);
    bot.bag_free_slots = 0;
    bot.vendor_valid = 1;
    bot.vendor_x = bot.pos_x + 2.0f;
    bot.vendor_y = bot.pos_y;
    bot.vendor_z = bot.pos_z;
    return bot;
}

// SkyborneExitMakeRoom: full bags at the giver of 93089 (the quest before the exit): the vendor first, not the accept. Off: the accept as today.
void FullBagsVisitAVendorBeforeTheExitChain()
{
    std::vector<std::string> events;
    PolicyOptions options = Options(events);
    options.SkyborneExitMakeRoom = true;
    BotPolicy policy(options);
    auto const first = policy.Decide(View(FullBagsAtGiver(93089)), 1000);
    CHECK(!Sends(first, kQuestAccept, 93089));
    CHECK(Find(first, ActionType::kVendor));
    CHECK(Logged(events, "skyborne_make_room: bot=10 quest=93089"));
    // After the visit (once per ten minutes) the accept is sent even with the bags still full.
    bool accepted = false;
    for (std::uint64_t now = 5000; now < 5000 + 120000 && !accepted; now += 5000)
        accepted = Sends(policy.Decide(View(FullBagsAtGiver(93089)), now), kQuestAccept, 93089);
    CHECK(accepted);

    std::vector<std::string> offEvents;
    PolicyOptions off = Options(offEvents);
    BotPolicy plain(off);
    CHECK(Sends(plain.Decide(View(FullBagsAtGiver(93089)), 1000), kQuestAccept, 93089));
    CHECK(!Logged(offEvents, "skyborne_make_room"));
}

// An accept of an exit-chain quest refused for full bags (reason 9): the vendor visit, and the quest is tried again after it.
void ABagsFullRefusalOfTheExitChainSendsTheBotToAVendor()
{
    std::vector<std::string> events;
    PolicyOptions options = Options(events);
    options.SkyborneExitMakeRoom = true;
    BotPolicy policy(options);
    BotSnapshot bot = FullBagsAtGiver(93089);
    bot.bag_free_slots = 3; // room by the count, but the core says the bags are full (a quest item that does not stack)
    CHECK(Sends(policy.Decide(View(bot), 1000), kQuestAccept, 93089));
    bot.last_action_sequence = 1;
    bot.last_action_type = std::uint8_t(ActionType::kQuest);
    bot.last_action_status = BotActionStatus::kRefused;
    bot.results[0].type = std::uint8_t(ActionType::kQuest);
    bot.results[0].status = std::uint8_t(BotActionStatus::kRefused);
    bot.results[0].reason = std::uint16_t(forever_module::ActionReason::kBagsFull);
    bot.results[0].sequence = 1;
    auto const refused = policy.Decide(View(bot), 5000);
    CHECK(Logged(events, "skyborne_make_room: bot=10 quest=93089 level=15 reason=refused_9"));
    CHECK(Find(refused, ActionType::kVendor));
    // The vendor visit lifted the block: the accept comes again well before the minute of a bags-full block.
    bool again = false;
    for (std::uint64_t now = 9000; now < 40000 && !again; now += 3000)
        again = Sends(policy.Decide(View(bot), now), kQuestAccept, 93089);
    CHECK(again);
}

} // namespace

int main()
{
    ParserReadsTheHopRecord();
    ParserDropsBadHopLines();
    TheShippedSideFilesParseWithTheirHop();
    KeyOffIgnoresTheExitQuest();
    KeyOnWalksHopsAndHandsIn();
    TheHopComesBeforeTheStuckRule();
    ADockIsWalkedToInsteadOfTheGiver();
    OnlyTheSkyborneMapPairsAreUsed();
    ARefusedHopBlocksTheQuestLongerEachTime();
    ASilentHopCountsAsRefused();
    TheExitChainIsTakenWhenGrey();
    PolicySendsTheHopTeleportAndHandsInAfterIt();
    PolicyBlocksTheQuestWhenTheTeleportIsRefused();
    PolicyKeyOffSendsNoHop();
    FullBagsVisitAVendorBeforeTheExitChain();
    ABagsFullRefusalOfTheExitChainSendsTheBotToAVendor();
    if (failures == 0)
        std::printf("skyborne_exit_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
