#include "quest_planner.h"

#include <cmath>
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
    if (Distance2d(bot, where) > kQuestReachYards)
    {
        step.Kind = QuestStepKind::WalkTo;
        return step;
    }
    step.Kind = nowMs >= memory.NextActionMs ? actKind : QuestStepKind::Wait;
    return step;
}

} // namespace

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

QuestStep QuestPlanner::Next(BotSnapshot const& bot, QuestMemory& memory, std::uint64_t nowMs) const
{
    QuestStep none;
    if (!Enabled())
        return none;
    if (memory.StartIndex == -2 || (memory.StartIndex == -1 && bot.quest_count == 0 && nowMs % 60000 < 1000))
        memory.StartIndex = FindStart(bot);
    if (memory.StartIndex < 0)
        return none;
    QuestStartDef const& start = _data->Starts[std::size_t(memory.StartIndex)];

    // A hand-in that was sent and whose quest is out of the log is done.
    if (memory.PendingHandIn != 0 && !InLog(bot, memory.PendingHandIn))
    {
        memory.Done.insert(memory.PendingHandIn);
        memory.PendingHandIn = 0;
    }

    // Progress bookkeeping for the stuck rule: the total of a quest's objective counts, and since when it has been that.
    for (QuestDef const& quest : start.Quests)
    {
        QuestEntry const* entry = InLog(bot, quest.Id);
        if (!entry)
        {
            memory.Progress.erase(quest.Id);
            continue;
        }
        std::uint32_t sum = 0;
        for (std::uint16_t count : entry->counts)
            sum += count;
        auto [at, inserted] = memory.Progress.try_emplace(quest.Id);
        if (inserted || at->second.Sum != sum)
            at->second = QuestMemory::ProgressInfo{ sum, nowMs };
    }

    // 1. A complete quest: hand it in.
    for (QuestDef const& quest : start.Quests)
    {
        QuestEntry const* entry = InLog(bot, quest.Id);
        if (entry && entry->status == 2 && !Blocked(memory, quest.Id, nowMs))
            return WalkOrAct(bot, memory, quest.Ender, QuestStepKind::HandIn, quest.Id, nowMs);
    }

    // 2. A quest in the log with a target short: hunt it, or give it up when nothing has moved for a long time.
    for (QuestDef const& quest : start.Quests)
    {
        QuestEntry const* entry = InLog(bot, quest.Id);
        if (!entry || entry->status != 1)
            continue;
        if (nowMs - memory.Progress[quest.Id].SinceMs > kStuckMs)
        {
            QuestStep step;
            step.Kind = nowMs >= memory.NextActionMs ? QuestStepKind::Abandon : QuestStepKind::Wait;
            step.Quest = quest.Id;
            return step;
        }
        QuestStep step;
        step.Quest = quest.Id;
        for (QuestObjectiveDef const& objective : quest.Objectives)
        {
            if (objective.Position < forever_module::kMaxQuestObjectives && entry->counts[objective.Position] >= objective.Need)
                continue;
            step.Entries.insert(step.Entries.end(), objective.Entries.begin(), objective.Entries.end());
        }
        if (step.Entries.empty())
            continue; // nothing known to hunt for this quest (or all met): the next rules
        step.Kind = QuestStepKind::Hunt;
        if (!quest.Areas.empty())
        {
            std::size_t& index = memory.AreaIndex[quest.Id];
            index %= quest.Areas.size();
            if (Distance2d(bot, quest.Areas[index]) <= kAreaReachYards)
                index = (index + 1) % quest.Areas.size();
            step.Dest = quest.Areas[index];
        }
        else
            step.Dest = QuestPoint{ bot.pos_x, bot.pos_y, bot.pos_z };
        return step;
    }

    // 3. The next quest of the start to accept.
    for (QuestDef const& quest : start.Quests)
    {
        if (memory.Done.count(quest.Id) || InLog(bot, quest.Id) || Blocked(memory, quest.Id, nowMs))
            continue;
        return WalkOrAct(bot, memory, quest.Giver, QuestStepKind::Accept, quest.Id, nowMs);
    }
    return none;
}

void QuestPlanner::NoteSent(QuestMemory& memory, std::uint32_t operation, std::uint32_t quest, std::uint64_t nowMs) const
{
    memory.NextActionMs = nowMs + kQuestActionGapMs;
    memory.LastActionQuest = quest;
    if (operation == kQuestHandIn)
        memory.PendingHandIn = quest;
    if (operation == kQuestAbandon)
    {
        memory.BlockedUntilMs[quest] = nowMs + kBlockAfterAbandonMs;
        memory.Progress.erase(quest);
    }
}

void QuestPlanner::NoteRefused(QuestMemory& memory, std::uint64_t nowMs) const
{
    if (memory.LastActionQuest == 0)
        return;
    memory.BlockedUntilMs[memory.LastActionQuest] = nowMs + kBlockAfterRefusalMs;
    if (memory.PendingHandIn == memory.LastActionQuest)
        memory.PendingHandIn = 0;
}
