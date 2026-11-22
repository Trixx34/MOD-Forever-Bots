// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the second step of the Alliance Skyborne exit (doc/skyborne-alliance-94947.md): 94947 "Take the Skyborne Portal to Stormwind", whose one
// objective (type 14, a criteria tree of "cast spell 1284041") is met by using the portal object 631299 in Dalaran. The 'use' record of the quest data, the
// planner's walk to the object and UseObject step, the move it causes, the fallback teleport after uses that do nothing, and the policy's kUseObject. No core.
#include "bot_policy.h"
#include "quest_data.h"
#include "quest_planner.h"

#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
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

// The Alliance exit as in data/world_quests_alliance.txt, cut down: the hop to Dalaran, the portal quest in Dalaran, and the quest after it in Stormwind.
char const* kData =
    "start W2991\n"
    "anchor 2991 1991.8 532.8 658.3\n"
    "quest 94946 giver 1991.9 525.0 658.0 ender 449.0 439.0 111.3\n"
    "req 94946 level 13 minlevel 7\n"
    "hop 94946 0 448.96 439.35 111.33\n"
    "start W0_0_0\n"
    "anchor 0 34.9 255.9 42.0\n"
    "quest 94947 giver 449.0 439.0 111.3 ender -8440.0 329.4 122.6\n"
    "req 94947 level 13 minlevel 7 prev 94946\n"
    "use 94947 631299 450.66 446.31 110.75 land -8999.80 856.45 29.62\n"
    "start W0_SW\n"
    "anchor 0 -8392.8 356.6 122.5\n"
    "quest 98021 giver -8440.0 329.4 122.6 ender -10508.8 1045.2 60.7\n"
    "req 98021 level 13 prev 94947\n";

constexpr std::uint32_t kPortal = 631299;

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

BotSnapshot Bot(float x, float y, float z, std::uint32_t level = 15)
{
    BotSnapshot bot;
    bot.guid = 10;
    bot.health = 1000;
    bot.max_health = 1000;
    bot.level = level;
    bot.map_id = 0;
    bot.pos_x = x;
    bot.pos_y = y;
    bot.pos_z = z;
    bot.bag_free_slots = 10;
    return bot;
}

BotSnapshot AtGiver() { return Bot(448.96f, 439.35f, 111.33f); }           // where the hop to Dalaran lands, beside Denaaris Stargale
BotSnapshot AtPortal() { return Bot(450.0f, 445.0f, 110.75f); }            // within reach of the portal object
BotSnapshot Landed() { return Bot(-8999.8f, 856.45f, 29.62f); }            // where the portal's teleport puts the bot in Stormwind

void LogQuest(BotSnapshot& bot, std::uint32_t quest, std::uint8_t status)
{
    forever_module::QuestEntry& entry = bot.quests[bot.quest_count++];
    entry.quest_id = quest;
    entry.status = status;
}

BotSnapshot With(BotSnapshot bot, std::uint32_t quest, std::uint8_t status)
{
    LogQuest(bot, quest, status);
    return bot;
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

bool Logged(std::vector<std::string> const& events, char const* prefix)
{
    for (std::string const& event : events)
        if (event.rfind(prefix, 0) == 0)
            return true;
    return false;
}

// ---- parser ----

void ParserReadsTheUseRecord()
{
    std::vector<std::string> warnings;
    auto const data = Parse(kData, &warnings);
    CHECK(warnings.empty());
    QuestDef const& portal = data->Starts[1].Quests[0];
    CHECK(portal.Id == 94947 && portal.HasUse && portal.UseEntry == kPortal && portal.UseMap == 0);
    CHECK(portal.Use.X == 450.66f && portal.Use.Y == 446.31f && portal.Use.Z == 110.75f);
    CHECK(portal.HasLand && portal.Land.X == -8999.80f && portal.Land.Y == 856.45f && portal.Land.Z == 29.62f);
    CHECK(!portal.HasHop && !data->Starts[0].Quests[0].HasUse);

    auto const plain = Parse("start S\nanchor 0 0 0 0\nquest 1 giver 0 0 0 ender 1 1 1\nuse 1 77 5 6 7\n", &warnings);
    CHECK(warnings.empty() && plain->Starts[0].Quests[0].HasUse && !plain->Starts[0].Quests[0].HasLand && plain->Starts[0].Quests[0].UseEntry == 77);
}

void ParserDropsBadUseLines()
{
    std::vector<std::string> warnings;
    auto const data = Parse("start S\n"
                            "use 1 77 1 2 3\n"                      // no anchor yet
                            "anchor 0 0 0 0\n"
                            "quest 1 giver 0 0 0 ender 1 1 1\n"
                            "use 2 77 1 2 3\n"                      // no such quest
                            "use 1 0 1 2 3\n"                       // no object entry
                            "use 1 77 1 2\n"                        // no z
                            "use 1 77 1 2 3 land 4 5\n"             // land without z
                            "use 1 77 1 2 3 dock 4 5 6\n"           // not 'land'
                            "use 1 77 1 2 3 land 4 5 6 7\n",        // something after the land point
                            &warnings);
    CHECK(warnings.size() == 7);
    CHECK(!data->Starts[0].Quests[0].HasUse);
    for (std::string const& warning : warnings)
        CHECK(warning.find("use") != std::string::npos || warning.find("before the first") != std::string::npos);
}

// The shipped side files: the Alliance file has 94947 with its portal, in a start on map 0, after 94946; the Horde file has no 'use' record (its step after
// the hop, 95350 to Orgrimmar, has no objective and is in the data already).
void TheShippedSideFilesCarryThePortalQuest()
{
#ifdef FOREVER_BOTS_DATA_DIR
    std::vector<std::string> warnings;
    QuestData const alliance = LoadQuestData(std::string(FOREVER_BOTS_DATA_DIR) + "/world_quests_alliance.txt", warnings);
    CHECK(warnings.empty());
    int uses = 0;
    for (QuestStartDef const& start : alliance.Starts)
        for (QuestDef const& def : start.Quests)
            if (def.HasUse)
            {
                ++uses;
                CHECK(def.Id == 94947 && start.Map == 0 && def.UseMap == 0 && def.UseEntry == kPortal && def.PrevQuest == 94946 && def.HasLand);
                CHECK(def.Ender.X == -8440.0f && def.Ender.Y == 329.4f);
            }
    CHECK(uses == 1);
    QuestPlanner const planner(std::make_shared<QuestData>(alliance), nullptr, On());
    CHECK(planner.ExitChain(94946) && planner.ExitChain(94947));

    QuestData const horde = LoadQuestData(std::string(FOREVER_BOTS_DATA_DIR) + "/world_quests_horde.txt", warnings);
    CHECK(warnings.empty());
    bool earthenRingNext = false;
    for (QuestStartDef const& start : horde.Starts)
        for (QuestDef const& def : start.Quests)
        {
            CHECK(!def.HasUse);
            earthenRingNext = earthenRingNext || (def.Id == 95350 && def.PrevQuest == 95349 && start.Map == 1);
        }
    CHECK(earthenRingNext);
#endif
}

// ---- planner ----

// Key off: 94947 is unknown (as when it was not in the data): not taken in Dalaran, and in the log it gets no step.
void KeyOffIgnoresThePortalQuest()
{
    QuestPlanner const planner(Parse(kData));
    QuestMemory memory;
    memory.Done.insert(94946);
    CHECK(planner.Next(AtGiver(), memory, 1000).Kind == QuestStepKind::None);
    CHECK(planner.Next(With(AtPortal(), 94947, 1), memory, 1000).Kind == QuestStepKind::None);
    CHECK(!planner.ExitChain(94947));
}

// A 'use' quest that does not follow a usable hop is unknown even with the key on: the record is for the exit chain only.
void AUseQuestNotAfterAHopIsUnknown()
{
    QuestPlanner const planner(Parse("start S\nanchor 0 0 0 0\nquest 5 giver 0 0 0 ender 1 1 1\nquest 6 giver 0 0 0 ender 1 1 1\nreq 6 prev 5\n"
                                     "use 6 77 2 2 0 land 300 300 0\n"),
                               nullptr, On());
    QuestMemory memory;
    memory.Done.insert(5);
    CHECK(planner.Next(Bot(0.0f, 0.0f, 0.0f), memory, 1000).Kind == QuestStepKind::None);
    CHECK(planner.Next(With(Bot(2.0f, 2.0f, 0.0f), 6, 1), memory, 1000).Kind == QuestStepKind::None);
    CHECK(!planner.ExitChain(6));
}

// Key on: in Dalaran after the hop, the accept; the walk to the portal, the use, the wait, the move to Stormwind, the hand-in at Bolvar, then 98021.
void KeyOnAcceptsUsesThePortalAndHandsIn()
{
    QuestPlanner const planner(Parse(kData), nullptr, On());
    CHECK(planner.ExitChain(94947));
    QuestMemory memory;
    memory.Done.insert(94946);
    QuestStep const accept = planner.Next(AtGiver(), memory, 1000);
    CHECK(accept.Kind == QuestStepKind::Accept && accept.Quest == 94947 && accept.HopLeg);
    planner.NoteSent(memory, kQuestAccept, 94947, 1000);

    // In the log at the giver: the portal is 7 yards away, walk to it (a hop leg: LevelSafeWalks leaves it alone in Alterac).
    QuestStep const walk = planner.Next(With(AtGiver(), 94947, 1), memory, 5000);
    CHECK(walk.Kind == QuestStepKind::WalkTo && walk.Quest == 94947 && walk.HopLeg && walk.Dest.X == 450.66f && walk.Dest.Y == 446.31f);

    // At the portal: use it.
    BotSnapshot const at = With(AtPortal(), 94947, 1);
    QuestStep const use = planner.Next(at, memory, 6000);
    CHECK(use.Kind == QuestStepKind::UseObject && use.Quest == 94947 && use.Object == kPortal && use.Dest.X == 450.66f && use.HopLeg);
    planner.NoteUseSent(memory, 94947, at, 6000);
    CHECK(planner.Next(at, memory, 7000).Kind == QuestStepKind::Wait);
    CHECK(planner.CheckUse(at, memory, 7000) == UseCheck::None);

    // The portal moved the bot to Stormwind with the objective met: the walk to Bolvar and the hand-in.
    BotSnapshot const landed = With(Landed(), 94947, 2);
    CHECK(planner.CheckUse(landed, memory, 8000) == UseCheck::Moved);
    CHECK(memory.UsePending == 0);
    QuestStep const toBolvar = planner.Next(landed, memory, 9000);
    CHECK(toBolvar.Kind == QuestStepKind::WalkTo && toBolvar.Quest == 94947 && toBolvar.Dest.X == -8440.0f && toBolvar.HopLeg);
    BotSnapshot const atBolvar = With(Bot(-8441.0f, 330.0f, 122.6f), 94947, 2);
    QuestStep const handIn = planner.Next(atBolvar, memory, 60000);
    CHECK(handIn.Kind == QuestStepKind::HandIn && handIn.Quest == 94947);
    planner.NoteSent(memory, kQuestHandIn, 94947, 60000);

    QuestStep const next = planner.Next(Bot(-8441.0f, 330.0f, 122.6f), memory, 70000);
    CHECK(memory.Done.count(94947) == 1);
    CHECK(next.Kind == QuestStepKind::Accept && next.Quest == 98021);
}

// Complete but still in Dalaran (the criterion counted, the teleport did not happen): the portal again, not a walk across the continent to Bolvar.
void ACompleteQuestInDalaranUsesThePortalNotTheRoad()
{
    QuestPlanner const planner(Parse(kData), nullptr, On());
    QuestMemory memory;
    QuestStep const step = planner.Next(With(AtPortal(), 94947, 2), memory, 1000);
    CHECK(step.Kind == QuestStepKind::UseObject && step.Quest == 94947);
}

// Uses that do nothing (silent, or refused by the core): after kUseTriesBeforeHop the fallback teleport to the land point on map 0; it has landed only near it.
void UsesThatDoNothingEndInTheFallbackTeleport()
{
    QuestPlanner const planner(Parse(kData), nullptr, On());
    QuestMemory memory;
    BotSnapshot const at = With(AtPortal(), 94947, 1);
    std::uint64_t now = 1000;
    CHECK(planner.Next(at, memory, now).Kind == QuestStepKind::UseObject);
    planner.NoteUseSent(memory, 94947, at, now);
    CHECK(planner.CheckUse(at, memory, now + kUseWaitMs - 1) == UseCheck::None);
    CHECK(planner.CheckUse(at, memory, now + kUseWaitMs) == UseCheck::NoMove);
    CHECK(memory.UseFails[94947] == 1 && memory.UsePending == 0);
    now += kUseWaitMs + 1000;
    CHECK(planner.Next(at, memory, now).Kind == QuestStepKind::UseObject); // a second try first
    planner.NoteUseSent(memory, 94947, at, now);
    CHECK(planner.NoteUseRefused(memory) == 2); // the core refused it (Bot.Actions.UseObject off)
    CHECK(planner.NoteUseRefused(memory) == 0); // nothing pending any more
    now += kQuestActionGapMs;
    QuestStep const hop = planner.Next(at, memory, now);
    CHECK(hop.Kind == QuestStepKind::Hop && hop.Quest == 94947 && hop.Map == 0 && hop.Dest.X == -8999.80f && hop.Dest.Y == 856.45f && hop.HopLeg);
    planner.NoteHopSent(memory, 94947, 0, now, true, hop.Dest);
    CHECK(memory.HopSameMap);
    CHECK(planner.Next(at, memory, now + 1000).Kind == QuestStepKind::Wait);
    CHECK(planner.CheckHop(at, memory, now + 1000) == HopCheck::None); // on map 0 already, but not at the land point
    CHECK(planner.CheckHop(Landed(), memory, now + 2000) == HopCheck::Done);

    // A fallback teleport the core never answers counts as a refused hop (the quest is blocked).
    QuestMemory silent;
    silent.UseFails[94947] = kUseTriesBeforeHop;
    CHECK(planner.Next(at, silent, 1000).Kind == QuestStepKind::Hop);
    planner.NoteHopSent(silent, 94947, 0, 1000, true, QuestPoint{ -8999.8f, 856.45f, 29.62f });
    CHECK(planner.CheckHop(at, silent, 1000 + kHopWaitMs) == HopCheck::NoMove);
    CHECK(planner.Next(at, silent, 1000 + kHopWaitMs + 5000).Kind != QuestStepKind::Hop);
}

// Without a land point there is no fallback: the quest is left alone for a while after the failed uses.
void NoLandPointNoFallback()
{
    QuestPlanner const planner(Parse("start A\nanchor 2991 0 0 0\nquest 1 giver 0 0 0 ender 449 439 111\nhop 1 0 449 439 111\n"
                                     "start B\nanchor 0 449 439 111\nquest 2 giver 449 439 111 ender -8440 329 122\nreq 2 prev 1\nuse 2 77 450 446 110\n"),
                               nullptr, On());
    QuestMemory memory;
    memory.UseFails[2] = kUseTriesBeforeHop;
    BotSnapshot const at = With(Bot(450.0f, 445.0f, 110.0f), 2, 1);
    CHECK(planner.Next(at, memory, 1000).Kind != QuestStepKind::Hop);
    CHECK(memory.BlockedUntilMs[2] == 1000 + kHopRefusedBlockMs);
    CHECK(planner.Next(at, memory, 1000 + kHopRefusedBlockMs).Kind == QuestStepKind::UseObject);
}

// The way to the portal is not being stuck: no abandon while the bot is at the object.
void ThePortalComesBeforeTheStuckRule()
{
    QuestPlanner const planner(Parse(kData), nullptr, On());
    QuestMemory memory;
    BotSnapshot const at = With(AtGiver(), 94947, 1);
    for (std::uint64_t now = 0; now <= kStuckMs * 2; now += 60000)
        CHECK(planner.Next(at, memory, now).Kind == QuestStepKind::WalkTo);
}

// The portal quest is taken when grey (a level 25 Skyborne in Dalaran must still get out).
void ThePortalQuestIsTakenWhenGrey()
{
    QuestPlanner const planner(Parse(kData), nullptr, On());
    QuestMemory memory;
    memory.Done.insert(94946);
    QuestStep const accept = planner.Next(Bot(448.96f, 439.35f, 111.33f, 25), memory, 1000);
    CHECK(accept.Kind == QuestStepKind::Accept && accept.Quest == 94947);
}

// With 94946 not known done (a restart forgot it), only a Skyborne bot (race 95 or 96, or unknown) tries 94947; another Alliance bot passing by does not.
void OnlySkyborneTryThePortalQuestWithoutItsPrev()
{
    QuestPlanner const planner(Parse(kData), nullptr, On());
    QuestMemory human;
    CHECK(planner.Next(AtGiver(), human, 1000, QuestBotInfo{ 1, 1 }).Kind == QuestStepKind::None);
    QuestMemory skyborne;
    QuestStep const accept = planner.Next(AtGiver(), skyborne, 1000, QuestBotInfo{ 1, kRaceSkyborneAlliance });
    CHECK(accept.Kind == QuestStepKind::Accept && accept.Quest == 94947);
}

// ---- policy ----

PolicyOptions Options(std::vector<std::string>& events)
{
    PolicyOptions options;
    options.AggressiveStart = true;
    options.WanderWhenIdle = false;
    options.Quests = Parse(kData);
    options.SkyborneExitQuests = true;
    options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
    return options;
}

// At the portal the policy sends kUseObject (spell_id = the object's entry, dest = its place) and logs it; the move logs the portal done and the walk to Bolvar.
void PolicyUsesThePortal()
{
    std::vector<std::string> events;
    BotPolicy policy(Options(events));
    auto const actions = policy.Decide(View(With(AtPortal(), 94947, 1)), 1000);
    BotAction const* use = Find(actions, ActionType::kUseObject);
    CHECK(use && use->spell_id == kPortal && use->target_guid == 0 && use->dest_x == 450.66f && use->dest_y == 446.31f && use->dest_z == 110.75f);
    CHECK(Logged(events, "skyborne_portal_use: bot=10 quest=94947 object=631299"));
    CHECK(!Find(policy.Decide(View(With(AtPortal(), 94947, 1)), 2000), ActionType::kUseObject)); // one use, then the wait

    auto const after = policy.Decide(View(With(Landed(), 94947, 2)), 3000);
    CHECK(Logged(events, "skyborne_portal_done: bot=10 quest=94947"));
    // The walk to Bolvar starts (a long walk: the bot mounts first, or moves).
    BotAction const* walk = Find(after, ActionType::kMoveTo);
    CHECK(Find(after, ActionType::kMount) || (walk && walk->dest_x > -8999.0f && walk->dest_y < 856.0f));
    CHECK(!Find(after, ActionType::kUseObject) && !Find(after, ActionType::kTeleport));
}

// The core refuses the use twice (Bot.Actions.UseObject = 0): two events, then the fallback teleport to the land point on map 0, and its landing.
void PolicyFallsBackToTheTeleportAfterRefusedUses()
{
    std::vector<std::string> events;
    BotPolicy policy(Options(events));
    BotSnapshot bot = With(AtPortal(), 94947, 1);
    std::uint64_t now = 1000;
    std::uint32_t sequence = 0;
    for (int refusal = 1; refusal <= int(kUseTriesBeforeHop); ++refusal)
    {
        bool used = false;
        for (int tick = 0; tick < 10 && !used; ++tick, now += 1000)
            used = Find(policy.Decide(View(bot), now), ActionType::kUseObject) != nullptr;
        CHECK(used);
        bot.last_action_sequence = ++sequence;
        bot.last_action_type = std::uint8_t(ActionType::kUseObject);
        bot.last_action_status = BotActionStatus::kRefused;
        CHECK(!Find(policy.Decide(View(bot), now), ActionType::kTeleport) || refusal == int(kUseTriesBeforeHop));
        now += 1000;
    }
    CHECK(Logged(events, "skyborne_portal_refused: bot=10 quest=94947 level=15 map=0 reason=use_refused"));
    BotAction teleport;
    bool teleported = false;
    for (int tick = 0; tick < 10 && !teleported; ++tick, now += 1000)
    {
        auto const actions = policy.Decide(View(bot), now);
        if (BotAction const* found = Find(actions, ActionType::kTeleport))
        {
            teleport = *found;
            teleported = true;
        }
    }
    CHECK(teleported && teleport.spell_id == 0 && teleport.dest_x == -8999.80f && teleport.dest_y == 856.45f);
    CHECK(Logged(events, "skyborne_portal_fallback: bot=10 quest=94947"));
    CHECK(!Logged(events, "skyborne_hop_start"));
    policy.Decide(View(With(Landed(), 94947, 1)), now + 1000);
    CHECK(Logged(events, "skyborne_hop_done: bot=10"));
}

// Key off: no use, no teleport for a bot with 94947 in its log at the portal.
void PolicyKeyOffDoesNothingAtThePortal()
{
    std::vector<std::string> events;
    PolicyOptions options = Options(events);
    options.SkyborneExitQuests = false;
    BotPolicy policy(options);
    for (std::uint64_t now = 1000; now < 200000; now += 20000)
    {
        auto const actions = policy.Decide(View(With(AtPortal(), 94947, 1)), now);
        CHECK(!Find(actions, ActionType::kUseObject) && !Find(actions, ActionType::kTeleport));
    }
    CHECK(!Logged(events, "skyborne_portal_"));
}

} // namespace

int main()
{
    ParserReadsTheUseRecord();
    ParserDropsBadUseLines();
    TheShippedSideFilesCarryThePortalQuest();
    KeyOffIgnoresThePortalQuest();
    AUseQuestNotAfterAHopIsUnknown();
    KeyOnAcceptsUsesThePortalAndHandsIn();
    ACompleteQuestInDalaranUsesThePortalNotTheRoad();
    UsesThatDoNothingEndInTheFallbackTeleport();
    NoLandPointNoFallback();
    ThePortalComesBeforeTheStuckRule();
    ThePortalQuestIsTakenWhenGrey();
    OnlySkyborneTryThePortalQuestWithoutItsPrev();
    PolicyUsesThePortal();
    PolicyFallsBackToTheTeleportAfterRefusedUses();
    PolicyKeyOffDoesNothingAtThePortal();
    if (failures == 0)
        std::printf("skyborne_portal_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
