// SPDX-License-Identifier: GPL-2.0-or-later
#include "quest_planner.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace
{

using forever_module::BotSnapshot;
using forever_module::QuestEntry;

float Distance2d(BotSnapshot const& bot, QuestPoint const& point)
{
    float const dx = bot.pos_x - point.X;
    float const dy = bot.pos_y - point.Y;
    return std::sqrt(dx * dx + dy * dy);
}

QuestEntry const* InLog(BotSnapshot const& bot, std::uint32_t quest)
{
    for (std::size_t i = 0; i < bot.quest_count && i < forever_module::kMaxQuestLog; ++i)
        if (bot.quests[i].quest_id == quest)
            return &bot.quests[i];
    return nullptr;
}

bool Blocked(QuestMemory const& memory, std::uint32_t quest, std::uint64_t nowMs)
{
    auto const at = memory.BlockedUntilMs.find(quest);
    return at != memory.BlockedUntilMs.end() && nowMs < at->second;
}

QuestStep WalkOrAct(BotSnapshot const& bot, QuestMemory const& memory, QuestPoint const& where, QuestStepKind actKind,
                    std::uint32_t quest, std::uint64_t nowMs)
{
    QuestStep step;
    step.Quest = quest;
    step.Dest = where;
    float const reach = memory.NearEnough.count(quest) ? kQuestNearTryYards : kQuestReachYards;
    if (Distance2d(bot, where) > reach)
    {
        step.Kind = QuestStepKind::WalkTo;
        return step;
    }
    step.Kind = nowMs >= memory.NextActionMs ? actKind : QuestStepKind::Wait;
    return step;
}

// A hop the planner may use: SkyborneExitQuests on and an allowed map pair (2991 to 0 or 1). A quest with any other hop is unknown to the planner.
bool HopUsable(QuestDef const& quest, QuestPlannerOptions const& options)
{
    return quest.HasHop && options.SkyborneExitQuests && HopAllowed(quest.HopFrom, quest.HopMap);
}

// A quest with a hop that may not be used: left out entirely (today's behaviour, when such quests were not in the data at all).
bool HopHidden(QuestDef const& quest, QuestPlannerOptions const& options)
{
    return quest.HasHop && !HopUsable(quest, options);
}

} // namespace

QuestPlanner::QuestPlanner(std::shared_ptr<QuestData const> data, std::shared_ptr<QuestData const> alsoKnown, QuestPlannerOptions options)
    : _options(options), _data(std::move(data)), _alsoKnown(std::move(alsoKnown))
{
    // The list the planner works from first, so a quest in both is the one of that list.
    for (QuestData const* list : { _data.get(), _alsoKnown.get() })
        if (list)
            for (QuestStartDef const& start : list->Starts)
                for (QuestDef const& quest : start.Quests)
                {
                    _index.try_emplace(quest.Id, &quest);
                    if (quest.PrevQuest != 0)
                        _prerequisites.insert(quest.PrevQuest);
                    if (HopUsable(quest, _options))
                    {
                        _exitChain.insert(quest.Id);
                        if (quest.PrevQuest != 0)
                            _exitChain.insert(quest.PrevQuest);
                    }
                }
    // A 'use' quest (94947) is known only right after a usable hop quest (94946): the portal out of the hop's landing place, nothing else.
    for (auto const& [id, quest] : _index)
        if (QuestDef const* prev = quest->HasUse && _options.SkyborneExitQuests ? Lookup(quest->PrevQuest) : nullptr; prev && HopUsable(*prev, _options))
        {
            _useQuests.insert(id);
            _exitChain.insert(id);
        }
}

bool QuestPlanner::Hidden(QuestDef const& quest) const
{
    return HopHidden(quest, _options) || (quest.HasUse && !_useQuests.count(quest.Id));
}

QuestDef const* QuestPlanner::Lookup(std::uint32_t quest) const
{
    auto const at = _index.find(quest);
    return at == _index.end() ? nullptr : at->second;
}

int QuestPlanner::FindStart(BotSnapshot const& bot) const
{
    int best = -1;
    float bestDist = kStartRadiusYards;
    for (std::size_t i = 0; i < _data->Starts.size(); ++i)
    {
        QuestStartDef const& start = _data->Starts[i];
        if (!start.HasAnchor || start.Map != bot.map_id)
            continue;
        float const dist = Distance2d(bot, start.Anchor);
        if (dist <= bestDist)
        {
            bestDist = dist;
            best = int(i);
        }
    }
    return best;
}

bool QuestPlanner::Eligible(QuestDef const& quest, BotSnapshot const& bot, QuestBotInfo const& info) const
{
    if (quest.Group > 1 && !_options.GroupQuests)
        return false; // a quest for a party: only with QuestGroups on
    if (Hidden(quest))
        return false; // a hop quest with SkyborneExitQuests off (or a pair other than 2991 to 0 or 1), or a 'use' quest not after such a hop
    std::uint32_t const level = bot.level;
    if (level != 0)
    {
        if (quest.MinLevel != 0 && level < quest.MinLevel)
            return false;
        // The Skyborne exit chain is the only way off map 2991: it is taken however grey it is (a level 21 Skyborne must still be able to leave).
        if (quest.QuestLevel != 0 && level > quest.QuestLevel + kGreyQuestMargin && !_exitChain.count(quest.Id))
            return false; // grey: little experience left in it
        if (quest.QuestLevel != 0 && quest.QuestLevel > level + kRedQuestMargin)
            return false; // too hard for now
    }
    if (info.Race != 0 && info.Race <= 32 && quest.RaceMask != 0 && (quest.RaceMask & (1u << (info.Race - 1))) == 0)
        return false;
    if (info.ClassId != 0 && info.ClassId <= 32 && quest.ClassMask != 0 && (quest.ClassMask & (1u << (info.ClassId - 1))) == 0)
        return false;
    return true;
}

bool QuestPlanner::Taken(QuestMemory const& memory, BotSnapshot const& bot, std::uint32_t quest, std::uint64_t nowMs)
{
    if (memory.Done.count(quest) || InLog(bot, quest))
        return true;
    // A refused accept for a missing prerequisite is lifted as soon as another quest is done (RetryAfterDone), before its block runs out.
    auto const done = memory.RetryAfterDone.find(quest);
    bool const lifted = done != memory.RetryAfterDone.end() && memory.Done.size() > done->second;
    if (!lifted && Blocked(memory, quest, nowMs))
        return true;
    auto const level = memory.RetryAtLevel.find(quest);
    return level != memory.RetryAtLevel.end() && bot.level < level->second;
}

QuestStep QuestPlanner::Next(BotSnapshot const& bot, QuestMemory& memory, std::uint64_t nowMs, QuestBotInfo const& info) const
{
    QuestStep none;
    if (!Enabled())
        return none;
    if (memory.List != _data.get())
    {
        // The bot moved to another list (level 60): the start indices of the old one mean nothing here. Everything keyed by quest id (Done,
        // BlockedUntilMs, Refusals, PendingHandIn, RefusedHandIns, Progress, AreaIndex) stays: the quests of the old list that are in the log
        // are still known to the planner (alsoKnown), and AreaIndex is taken modulo the area count.
        memory.List = _data.get();
        memory.StartIndex = -2;
        memory.NoRouteUntilMs.clear();
    }
    memory.WalkingToStart = false;
    memory.LastWalkQuest = 0;
    memory.LastWalkKind = QuestStepKind::None;
    // Remember the step if it is a walk, for NoteNoRoute (a lambda around the returns below would hide the early exits, so each walk sets it).

    // A hand-in that was sent and whose quest is out of the log is done.
    if (memory.PendingHandIn != 0 && !InLog(bot, memory.PendingHandIn))
    {
        memory.Done.insert(memory.PendingHandIn);
        memory.PendingHandIn = 0;
    }

    // The quests in the log that the data knows, whichever start they belong to.
    std::vector<QuestDef const*> logged;
    for (std::size_t i = 0; i < bot.quest_count && i < forever_module::kMaxQuestLog; ++i)
        if (QuestDef const* def = Lookup(bot.quests[i].quest_id); def && !Hidden(*def))
            logged.push_back(def);

    // Progress bookkeeping for the stuck rule: the total of a quest's objective counts, and since when it has been that.
    for (auto it = memory.Progress.begin(); it != memory.Progress.end();)
        it = InLog(bot, it->first) ? std::next(it) : memory.Progress.erase(it);
    for (QuestDef const* quest : logged)
    {
        QuestEntry const* entry = InLog(bot, quest->Id);
        std::uint32_t sum = 0;
        for (std::uint16_t count : entry->counts)
            sum += count;
        auto [at, inserted] = memory.Progress.try_emplace(quest->Id);
        if (inserted || at->second.Sum != sum)
        {
            at->second = QuestMemory::ProgressInfo{ sum, nowMs };
            memory.NoRouteCount.erase(quest->Id); // it moved: the walks work
        }
    }

    // What NoteNoRoute needs to know about a step that walks.
    auto const walking = [&](QuestStep const& step) -> QuestStep
    {
        if (step.Kind == QuestStepKind::WalkTo || step.Kind == QuestStepKind::Hunt)
        {
            memory.LastWalkQuest = step.Quest;
            memory.LastWalkKind = step.Kind;
            memory.LastWalkDest = step.Dest;
        }
        return step;
    };

    // 0. A Skyborne exit quest in the log while the bot is still on the start's map (2991): walk to the dock (or the giver), then hop to the other map.
    // It comes before everything else: its ender is not on this map, and the bot has nothing left to do here. A blocked one (refused hops) is left in the
    // log and the bot goes on as before (grinds).
    auto const awayFromEnder = [&](QuestDef const& quest) { return quest.HasHop && bot.map_id != quest.HopMap; };
    for (QuestDef const* quest : logged)
    {
        if (!quest->HasHop || bot.map_id != quest->HopFrom || Blocked(memory, quest->Id, nowMs))
            continue;
        memory.Progress[quest->Id].SinceMs = nowMs; // the way to the hop is not being stuck (the stuck rule would abandon the quest)
        QuestStep step;
        step.Quest = quest->Id;
        step.HopLeg = true;
        if (memory.HopPending == quest->Id)
        {
            step.Kind = QuestStepKind::Wait; // the teleport is on its way (CheckHop)
            return step;
        }
        QuestPoint const dock = quest->HasDock ? quest->Dock : quest->Giver;
        float const reach = memory.NearEnough.count(quest->Id) ? kQuestNearTryYards : kHopDockYards;
        if (Distance2d(bot, dock) > reach)
        {
            step.Kind = QuestStepKind::WalkTo;
            step.Dest = dock;
            return walking(step);
        }
        if (nowMs < memory.NextActionMs)
        {
            step.Kind = QuestStepKind::Wait;
            return step;
        }
        step.Kind = QuestStepKind::Hop;
        step.Dest = quest->Hop;
        step.Map = quest->HopMap;
        return step;
    }

    // 0b. A 'use' quest in the log while the bot is near its object (94947 in Dalaran, after the hop): walk to the object and use it (the portal moves the bot;
    // its spell is the objective). Complete or not: the ender is where the portal leads. Uses that do nothing: the fallback teleport to the 'land' point.
    auto const atObject = [&](QuestDef const& quest) { return quest.HasUse && bot.map_id == quest.UseMap && Distance2d(bot, quest.Use) <= kPortalAreaYards; };
    for (QuestDef const* quest : logged)
    {
        if (!atObject(*quest) || Blocked(memory, quest->Id, nowMs))
            continue;
        memory.Progress[quest->Id].SinceMs = nowMs; // the way to the object is not being stuck
        QuestStep step;
        step.Quest = quest->Id;
        step.HopLeg = true;
        if (memory.UsePending == quest->Id || memory.HopPending == quest->Id)
        {
            step.Kind = QuestStepKind::Wait; // the use or the teleport is on its way (CheckUse, CheckHop)
            return step;
        }
        auto const fails = memory.UseFails.find(quest->Id);
        if (fails != memory.UseFails.end() && fails->second >= kUseTriesBeforeHop)
        {
            if (!quest->HasLand)
            {
                memory.UseFails.erase(fails);
                memory.BlockedUntilMs[quest->Id] = nowMs + kHopRefusedBlockMs; // no fallback known: left alone, tried again later
                continue;
            }
            step.Kind = nowMs >= memory.NextActionMs ? QuestStepKind::Hop : QuestStepKind::Wait;
            step.Dest = quest->Land;
            step.Map = quest->UseMap;
            return step;
        }
        float const reach = memory.NearEnough.count(quest->Id) ? kQuestNearTryYards : kUseReachYards;
        if (Distance2d(bot, quest->Use) > reach)
        {
            step.Kind = QuestStepKind::WalkTo;
            step.Dest = quest->Use;
            return walking(step);
        }
        step.Kind = nowMs >= memory.NextActionMs ? QuestStepKind::UseObject : QuestStepKind::Wait;
        step.Dest = quest->Use;
        step.Object = quest->UseEntry;
        return step;
    }

    // 1. A complete quest: hand it in. (A hop quest only on the map of its ender; a 'use' quest only away from its object.)
    for (QuestDef const* quest : logged)
    {
        QuestEntry const* entry = InLog(bot, quest->Id);
        if (entry->status == 2 && !Blocked(memory, quest->Id, nowMs) && !awayFromEnder(*quest) && !atObject(*quest))
        {
            QuestStep step = WalkOrAct(bot, memory, quest->Ender, QuestStepKind::HandIn, quest->Id, nowMs);
            step.HopLeg = quest->HasHop || quest->HasUse; // the hand-in right after the hop or the portal
            return walking(step);
        }
    }

    // 1b. A class quest in the log: complete it with its reward and no objectives, wherever the bot is (the GM command `.quest reward`).
    for (QuestDef const* quest : logged)
    {
        if (!quest->Reward || Blocked(memory, quest->Id, nowMs) || awayFromEnder(*quest))
            continue;
        QuestStep step;
        step.Quest = quest->Id;
        step.Dest = QuestPoint{ bot.pos_x, bot.pos_y, bot.pos_z };
        step.Kind = nowMs >= memory.NextActionMs ? QuestStepKind::Reward : QuestStepKind::Wait;
        return step;
    }

    // 2. A quest in the log with a target short: hunt it, or give it up when nothing has moved for a long time.
    for (QuestDef const* quest : logged)
    {
        QuestEntry const* entry = InLog(bot, quest->Id);
        if (entry->status != 1 || awayFromEnder(*quest) || atObject(*quest))
            continue;
        if (quest->Group > 1 && _options.GroupQuests && !Blocked(memory, quest->Id, nowMs))
        {
            // A quest for a party: wait until the bot's party has the size the quest wants (the policy invites partners), give up after the wait.
            if (bot.group_size >= quest->Group)
                memory.GroupWaitSinceMs.erase(quest->Id);
            else
            {
                std::uint64_t const since = memory.GroupWaitSinceMs.try_emplace(quest->Id, nowMs).first->second;
                QuestStep step;
                step.Quest = quest->Id;
                if (nowMs - since > _options.GroupWaitMs)
                {
                    memory.GroupWaitSinceMs.erase(quest->Id);
                    step.Kind = nowMs >= memory.NextActionMs ? QuestStepKind::Abandon : QuestStepKind::Wait;
                    return step;
                }
                memory.Progress[quest->Id].SinceMs = nowMs; // waiting is not being stuck
                step.Kind = QuestStepKind::WaitGroup;
                step.GroupSize = quest->Group;
                step.Dest = QuestPoint{ bot.pos_x, bot.pos_y, bot.pos_z };
                return step;
            }
        }
        if (nowMs - memory.Progress[quest->Id].SinceMs > kStuckMs)
        {
            QuestStep step;
            step.Kind = nowMs >= memory.NextActionMs ? QuestStepKind::Abandon : QuestStepKind::Wait;
            step.Quest = quest->Id;
            return step;
        }
        if (Blocked(memory, quest->Id, nowMs))
            continue; // its walks found no route: left alone (the bot grinds) until the block ends, or the stuck rule above abandons it
        QuestStep step;
        step.Quest = quest->Id;
        for (QuestObjectiveDef const& objective : quest->Objectives)
        {
            if (objective.Position < forever_module::kMaxQuestObjectives && entry->counts[objective.Position] >= objective.Need)
                continue;
            step.Entries.insert(step.Entries.end(), objective.Entries.begin(), objective.Entries.end());
        }
        if (step.Entries.empty())
            continue; // nothing known to hunt for this quest (or all met): the next rules
        step.Kind = QuestStepKind::Hunt;
        if (!quest->Areas.empty())
        {
            auto const known = memory.AreaIndex.find(quest->Id);
            std::size_t& index = memory.AreaIndex[quest->Id];
            if (known == memory.AreaIndex.end() && info.Variety) // path variety: each bot starts at its own hunting area
                index = path_variety::AreaStart(*info.Variety, info.Seed, info.Bot, quest->Id, quest->Areas.size());
            index %= quest->Areas.size();
            if (Distance2d(bot, quest->Areas[index]) <= kAreaReachYards)
                index = (index + 1) % quest->Areas.size();
            step.Dest = quest->Areas[index];
        }
        else
            step.Dest = QuestPoint{ bot.pos_x, bot.pos_y, bot.pos_z };
        return walking(step);
    }

    // 3. A new quest to accept, while the log has room. The bot's start is the nearest one on its map that has a quest it can take now; it is
    // looked at again whenever the log is empty (a bot that has finished a zone moves on to the next), and also when the old start has nothing left.
    if (bot.quest_count >= kMaxAcceptedQuests)
        return none;

    auto const pick = [&](QuestStartDef const& start, bool needPrevDone) -> QuestDef const*
    {
        // Path variety: among the first QuestOrderWindow eligible quests the bot takes the one its own order puts first, and leaves some free-standing quests alone.
        QuestDef const* best = nullptr;
        std::uint64_t bestKey = 0;
        std::uint32_t seen = 0;
        std::uint32_t const window = info.Variety ? std::max<std::uint32_t>(1, info.Variety->QuestOrderWindow) : 1;
        for (QuestDef const& quest : start.Quests)
        {
            if (Taken(memory, bot, quest.Id, nowMs) || !Eligible(quest, bot, info))
                continue;
            bool const prevKnown = quest.PrevQuest == 0 || memory.Done.count(quest.PrevQuest) || InLog(bot, quest.PrevQuest);
            if (needPrevDone != prevKnown)
                continue;
            // A 'use' quest of the Skyborne exit (94947) with its hop quest not known done (a restart forgot it) is tried by Skyborne bots only.
            if (!prevKnown && quest.HasUse && info.Race != 0 && info.Race != kRaceSkyborneAlliance && info.Race != kRaceSkyborneHorde)
                continue;
            if (!info.Variety)
                return &quest;
            // a quest that is the 'prev' of another is never skipped: a skipped chain head would leave the rest of the chain refused
            if (!_prerequisites.count(quest.Id) && path_variety::SkipsQuest(*info.Variety, info.Seed, info.Bot, quest.Id))
                continue;
            std::uint64_t const key = path_variety::QuestOrderKey(*info.Variety, info.Seed, info.Bot, quest.Id);
            if (!best || key < bestKey)
            {
                best = &quest;
                bestKey = key;
            }
            if (++seen >= window)
                break;
        }
        return best;
    };

    // The current start first (it keeps a bot working its zone), then the starts of its map by distance.
    std::vector<std::size_t> order;
    if (memory.StartIndex >= 0 && std::size_t(memory.StartIndex) < _data->Starts.size())
        order.push_back(std::size_t(memory.StartIndex));
    std::vector<std::pair<float, std::size_t>> byDistance;
    for (std::size_t i = 0; i < _data->Starts.size(); ++i)
    {
        QuestStartDef const& start = _data->Starts[i];
        if (!start.HasAnchor || start.Map != bot.map_id || (memory.StartIndex >= 0 && i == std::size_t(memory.StartIndex)))
            continue;
        float dist = Distance2d(bot, start.Anchor);
        // A start made for the whole world (its quests carry levels) may be walked to from across its map, up to kWorldStartRadiusYards: a
        // farther giver is on another continent with no route (seen on the sim). A starter-zone start only from nearby.
        bool world = false;
        for (QuestDef const& quest : start.Quests)
            world = world || quest.QuestLevel != 0;
        if (!(world ? dist <= kWorldStartRadiusYards : dist <= kStartRadiusYards))
            continue;
        // A start whose giver the core found no route to is left alone for a while.
        auto const blocked = memory.NoRouteUntilMs.find(i);
        if (blocked != memory.NoRouteUntilMs.end() && blocked->second > nowMs)
            continue;
        // Path variety: the ranking of the starts is wobbled per bot (the reach test above used the true distance).
        byDistance.emplace_back(info.Variety ? path_variety::StartDistance(*info.Variety, info.Seed, info.Bot, i, dist) : dist, i);
    }
    std::sort(byDistance.begin(), byDistance.end());
    for (auto const& entry : byDistance)
        order.push_back(entry.second);
    if (memory.StartIndex < 0 && order.empty())
        return none;

    // First the quests whose predecessor is known done (or that have none), in every start; then the others.
    for (bool prevDone : { true, false })
        for (std::size_t index : order)
        {
            QuestStartDef const& start = _data->Starts[index];
            if (start.Map != bot.map_id)
                continue;
            if (QuestDef const* quest = pick(start, prevDone))
            {
                memory.StartIndex = int(index);
                // A quest that starts from an item the bot carries needs no walk (the accept is tried where it stands); one that starts from a game
                // object walks to the object's place (the giver point). Both are sent as kQuestStartFrom (operation 5).
                bool const fromItem = quest->StartItem != 0 && quest->StartObject == 0;
                QuestStep step = WalkOrAct(bot, memory, fromItem ? QuestPoint{ bot.pos_x, bot.pos_y, bot.pos_z } : quest->Giver, QuestStepKind::Accept, quest->Id, nowMs);
                step.StartFrom = quest->StartItem != 0 || quest->StartObject != 0;
                step.StartEntry = quest->StartObject;
                step.StartGiver = true;
                step.HopLeg = quest->HasUse; // the accept of the portal quest at the hop's landing place (Dalaran): LevelSafeWalks does not drop it
                memory.WalkingToStart = step.Kind == QuestStepKind::WalkTo;
                return walking(step);
            }
        }
    return none;
}

void QuestPlanner::NoteSent(QuestMemory& memory, std::uint32_t operation, std::uint32_t quest, std::uint64_t nowMs) const
{
    memory.NextActionMs = nowMs + kQuestActionGapMs;
    memory.LastActionQuest = quest;
    memory.LastActionAbandon = operation == kQuestAbandon;
    memory.NearEnough.erase(quest);
    memory.NoRouteCount.erase(quest);
    if (operation == kQuestHandIn || operation == kQuestReward)
        memory.PendingHandIn = quest;
    if (operation == kQuestAbandon)
    {
        memory.BlockedUntilMs[quest] = nowMs + kBlockAfterAbandonMs;
        memory.Progress.erase(quest);
        memory.GroupWaitSinceMs.erase(quest);
    }
}

void QuestPlanner::NoteNoRoute(QuestMemory& memory, std::uint64_t nowMs, BotSnapshot const* bot) const
{
    // A giver or ender that is near but has no route (it stands behind a counter or on an object): the accept or hand-in is tried from here. The
    // core measures the reach from the NPC's edge; when it still says no, the refusal blocks the quest as any other.
    if (bot && memory.LastWalkQuest != 0 && memory.LastWalkKind == QuestStepKind::WalkTo &&
        Distance2d(*bot, memory.LastWalkDest) <= kQuestNearTryYards && memory.NearEnough.insert(memory.LastWalkQuest).second)
    {
        memory.WalkingToStart = false;
        return;
    }
    if (memory.WalkingToStart && memory.StartIndex >= 0)
    {
        memory.NoRouteUntilMs[std::size_t(memory.StartIndex)] = nowMs + kNoRouteBlockMs;
        memory.StartIndex = -2; // looked up again: the nearest start that is not blocked
        memory.WalkingToStart = false;
        return;
    }
    // The walk to a quest's ender or hunting ground. A hunt moves on to the quest's next area; a few answers in a row block the quest.
    if (memory.LastWalkQuest == 0)
        return;
    std::uint32_t const quest = memory.LastWalkQuest;
    if (memory.LastWalkKind == QuestStepKind::Hunt)
        ++memory.AreaIndex[quest];
    if (++memory.NoRouteCount[quest] >= kNoRouteWalksBlock)
    {
        memory.BlockedUntilMs[quest] = nowMs + kWalkStuckBlockMs;
        memory.NoRouteCount.erase(quest);
    }
}

void QuestPlanner::NoteLevelUnsafe(QuestMemory& memory, std::uint32_t quest, QuestStepKind kind, std::uint64_t nowMs) const
{
    if (quest == 0)
        return;
    memory.WalkingToStart = false;
    if (kind == QuestStepKind::Hunt)
    {
        QuestDef const* def = Lookup(quest);
        std::size_t const areas = def ? def->Areas.size() : 0;
        std::uint32_t& skipped = memory.LevelUnsafeAreas[quest];
        if (areas > 1 && ++skipped < areas)
        {
            ++memory.AreaIndex[quest];
            return;
        }
    }
    memory.LevelUnsafeAreas.erase(quest);
    memory.BlockedUntilMs[quest] = nowMs + kLevelUnsafeBlockMs;
}

void QuestPlanner::NoteHopSent(QuestMemory& memory, std::uint32_t quest, std::uint32_t map, std::uint64_t nowMs, bool sameMap, QuestPoint dest) const
{
    memory.HopPending = quest;
    memory.HopPendingMap = map;
    memory.HopSentMs = nowMs;
    memory.HopPendingDest = dest;
    memory.HopSameMap = sameMap;
    memory.NextActionMs = nowMs + kQuestActionGapMs;
}

void QuestPlanner::NoteUseSent(QuestMemory& memory, std::uint32_t quest, BotSnapshot const& bot, std::uint64_t nowMs) const
{
    memory.UsePending = quest;
    memory.UseSentMs = nowMs;
    memory.UseFromMap = bot.map_id;
    memory.UseFrom = QuestPoint{ bot.pos_x, bot.pos_y, bot.pos_z };
    memory.NextActionMs = nowMs + kQuestActionGapMs;
}

std::uint32_t QuestPlanner::NoteUseRefused(QuestMemory& memory) const
{
    if (memory.UsePending == 0)
        return 0;
    std::uint32_t const quest = memory.UsePending;
    memory.UsePending = 0;
    return ++memory.UseFails[quest];
}

UseCheck QuestPlanner::CheckUse(BotSnapshot const& bot, QuestMemory& memory, std::uint64_t nowMs) const
{
    if (memory.UsePending == 0)
        return UseCheck::None;
    if (bot.map_id != memory.UseFromMap || Distance2d(bot, memory.UseFrom) > kPortalMovedYards)
    {
        memory.UseFails.erase(memory.UsePending);
        memory.UsePending = 0;
        return UseCheck::Moved;
    }
    if (nowMs - memory.UseSentMs < kUseWaitMs)
        return UseCheck::None;
    NoteUseRefused(memory);
    return UseCheck::NoMove;
}

std::uint32_t QuestPlanner::NoteHopRefused(QuestMemory& memory, std::uint64_t nowMs) const
{
    if (memory.HopPending == 0)
        return 0;
    std::uint32_t const quest = memory.HopPending;
    memory.HopPending = 0;
    memory.HopPendingMap = 0;
    std::uint32_t const times = ++memory.HopRefusals[quest];
    std::uint64_t const block = times >= kHopRefusalsForGood ? kBlockNeverMs : times == 2 ? kBlockAfterAcceptRefusal3Ms : kHopRefusedBlockMs;
    memory.BlockedUntilMs[quest] = nowMs + block;
    return times;
}

HopCheck QuestPlanner::CheckHop(BotSnapshot const& bot, QuestMemory& memory, std::uint64_t nowMs) const
{
    if (memory.HopPending == 0)
        return HopCheck::None;
    if (bot.map_id == memory.HopPendingMap && (!memory.HopSameMap || Distance2d(bot, memory.HopPendingDest) <= kPortalMovedYards))
    {
        memory.HopRefusals.erase(memory.HopPending);
        memory.HopPending = 0;
        memory.HopPendingMap = 0;
        return HopCheck::Done;
    }
    if (nowMs - memory.HopSentMs < kHopWaitMs)
        return HopCheck::None;
    NoteHopRefused(memory, nowMs);
    return HopCheck::NoMove;
}

bool QuestPlanner::ExitChain(std::uint32_t quest) const
{
    return quest != 0 && _exitChain.count(quest) != 0;
}

void QuestPlanner::NoteNeedsRoom(QuestMemory& memory, std::uint32_t quest) const
{
    if (quest != 0)
        memory.RefusedHandIns.insert(quest); // NoteServiced lifts the block of these after the vendor visit
}

void QuestPlanner::NoteWalkStuck(QuestMemory& memory, std::uint32_t quest, std::uint64_t nowMs) const
{
    if (quest != 0)
        memory.BlockedUntilMs[quest] = nowMs + kWalkStuckBlockMs;
}

bool QuestPlanner::NoteRefused(QuestMemory& memory, std::uint64_t nowMs, std::uint32_t reason, std::uint32_t botLevel) const
{
    if (memory.LastActionQuest == 0)
        return false;
    bool const handIn = memory.PendingHandIn == memory.LastActionQuest;
    std::uint32_t const quest = memory.LastActionQuest;
    memory.RetryAfterDone.erase(quest);
    memory.RetryAtLevel.erase(quest);
    if (!handIn && !memory.LastActionAbandon && reason != 0)
    {
        // The reasons of ActionReason (tier2_layout.h). The refusal is not counted in Refusals: the block does not grow, it is what the reason says.
        std::uint64_t block = 0;
        switch (reason)
        {
            case 1: // level too low: when the bot has a level more
                memory.RetryAtLevel[quest] = botLevel + 1;
                block = kBlockNeverMs;
                break;
            case 2: // level too high
            case 3: // class or race
                block = kBlockNeverMs;
                break;
            case 6: // already done
                block = kBlockNeverMs;
                break;
            case 4: // prerequisite: when another quest is done
                memory.RetryAfterDone[quest] = memory.Done.size();
                block = kBlockPrerequisiteMs;
                break;
            case 5: block = kBlockLogFullMs; break;
            case 7: block = kBlockBagsFullMs; break; // already on the quest: the log catches up in a moment
            case 8: block = kBlockSkillOrReputationMs; break;
            case 9: block = kBlockBagsFullMs; break;
            default: break; // no giver, other: the generic blocks below
        }
        if (block != 0)
        {
            // Level too low is lifted by the level, not by the clock: the block is dropped and RetryAtLevel holds the quest back (Taken).
            if (reason == 1)
                memory.BlockedUntilMs.erase(quest);
            else
                memory.BlockedUntilMs[quest] = nowMs + block;
            return false;
        }
    }
    // The block grows with every refusal of the same quest: a minute, ten minutes, then an hour (done long ago, a prerequisite missing, not for
    // this bot). A refused hand-in is most often a full bag, which the bot fixes at a vendor and which says nothing against the quest, so a
    // hand-in is blocked a minute for its first two refusals.
    std::uint32_t const times = ++memory.Refusals[memory.LastActionQuest];
    std::uint32_t const step = handIn && times > 1 ? times - 1 : times; // a hand-in's second refusal counts as the first
    // An accept the core refused ("cannot take": not eligible, a prerequisite missing, done long ago) is blocked ten minutes at once: longer than a hand-in's
    // minute, but not an hour, which kept bots from quests they could take once the core allowed it. (Without any block the planner walked bots to the
    // giver of such quests again and again, which fed the no-route flood.)
    std::uint64_t const acceptBlock = times == 1 ? kBlockAfterAcceptRefusalMs : times == 2 ? kBlockAfterAcceptRefusal2Ms : kBlockAfterAcceptRefusal3Ms;
    std::uint64_t const block = !handIn && !memory.LastActionAbandon ? acceptBlock : step == 1 ? kBlockAfterRefusalMs : step == 2 ? 10 * kBlockAfterRefusalMs : kBlockAfterAbandonMs;
    memory.BlockedUntilMs[memory.LastActionQuest] = nowMs + block;
    if (handIn)
    {
        memory.PendingHandIn = 0;
        memory.RefusedHandIns.insert(memory.LastActionQuest);
    }
    return handIn;
}

void QuestPlanner::NoteServiced(QuestMemory& memory) const
{
    for (std::uint32_t quest : memory.RefusedHandIns)
        memory.BlockedUntilMs.erase(quest);
    memory.RefusedHandIns.clear();
}
