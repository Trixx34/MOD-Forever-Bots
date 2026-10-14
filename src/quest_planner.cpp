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
    if (Distance2d(bot, where) > kQuestReachYards)
    {
        step.Kind = QuestStepKind::WalkTo;
        return step;
    }
    step.Kind = nowMs >= memory.NextActionMs ? actKind : QuestStepKind::Wait;
    return step;
}

} // namespace

QuestPlanner::QuestPlanner(std::shared_ptr<QuestData const> data) : _data(std::move(data))
{
    if (!_data)
        return;
    for (std::size_t i = 0; i < _data->Starts.size(); ++i)
        for (std::size_t q = 0; q < _data->Starts[i].Quests.size(); ++q)
            _index.try_emplace(_data->Starts[i].Quests[q].Id, std::make_pair(i, q));
}

QuestDef const* QuestPlanner::Lookup(std::uint32_t quest) const
{
    auto const at = _index.find(quest);
    return at == _index.end() ? nullptr : &_data->Starts[at->second.first].Quests[at->second.second];
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

bool QuestPlanner::Eligible(QuestDef const& quest, BotSnapshot const& bot, QuestBotInfo const& info)
{
    std::uint32_t const level = bot.level;
    if (level != 0)
    {
        if (quest.MinLevel != 0 && level < quest.MinLevel)
            return false;
        if (quest.QuestLevel != 0 && level > quest.QuestLevel + kGreyQuestMargin)
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
    return memory.Done.count(quest) || InLog(bot, quest) || Blocked(memory, quest, nowMs);
}

QuestStep QuestPlanner::Next(BotSnapshot const& bot, QuestMemory& memory, std::uint64_t nowMs, QuestBotInfo const& info) const
{
    QuestStep none;
    if (!Enabled())
        return none;

    // A hand-in that was sent and whose quest is out of the log is done.
    if (memory.PendingHandIn != 0 && !InLog(bot, memory.PendingHandIn))
    {
        memory.Done.insert(memory.PendingHandIn);
        memory.PendingHandIn = 0;
    }

    // The quests in the log that the data knows, whichever start they belong to.
    std::vector<QuestDef const*> logged;
    for (std::size_t i = 0; i < bot.quest_count && i < forever_module::kMaxQuestLog; ++i)
        if (QuestDef const* def = Lookup(bot.quests[i].quest_id))
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
            at->second = QuestMemory::ProgressInfo{ sum, nowMs };
    }

    // 1. A complete quest: hand it in.
    for (QuestDef const* quest : logged)
    {
        QuestEntry const* entry = InLog(bot, quest->Id);
        if (entry->status == 2 && !Blocked(memory, quest->Id, nowMs))
            return WalkOrAct(bot, memory, quest->Ender, QuestStepKind::HandIn, quest->Id, nowMs);
    }

    // 2. A quest in the log with a target short: hunt it, or give it up when nothing has moved for a long time.
    for (QuestDef const* quest : logged)
    {
        QuestEntry const* entry = InLog(bot, quest->Id);
        if (entry->status != 1)
            continue;
        if (nowMs - memory.Progress[quest->Id].SinceMs > kStuckMs)
        {
            QuestStep step;
            step.Kind = nowMs >= memory.NextActionMs ? QuestStepKind::Abandon : QuestStepKind::Wait;
            step.Quest = quest->Id;
            return step;
        }
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
            std::size_t& index = memory.AreaIndex[quest->Id];
            index %= quest->Areas.size();
            if (Distance2d(bot, quest->Areas[index]) <= kAreaReachYards)
                index = (index + 1) % quest->Areas.size();
            step.Dest = quest->Areas[index];
        }
        else
            step.Dest = QuestPoint{ bot.pos_x, bot.pos_y, bot.pos_z };
        return step;
    }

    // 3. A new quest to accept, while the log has room. The bot's start is the nearest one on its map that has a quest it can take now; it is
    // looked at again whenever the log is empty (a bot that has finished a zone moves on to the next), and also when the old start has nothing left.
    if (bot.quest_count >= kMaxAcceptedQuests)
        return none;

    auto const pick = [&](QuestStartDef const& start, bool needPrevDone) -> QuestDef const*
    {
        for (QuestDef const& quest : start.Quests)
        {
            if (Taken(memory, bot, quest.Id, nowMs) || !Eligible(quest, bot, info))
                continue;
            bool const prevKnown = quest.PrevQuest == 0 || memory.Done.count(quest.PrevQuest) || InLog(bot, quest.PrevQuest);
            if (needPrevDone == prevKnown)
                return &quest;
        }
        return nullptr;
    };

    // The current start first (it keeps a bot working its zone), then the starts of its map by distance.
    std::vector<std::size_t> order;
    if (memory.StartIndex >= 0)
        order.push_back(std::size_t(memory.StartIndex));
    std::vector<std::pair<float, std::size_t>> byDistance;
    for (std::size_t i = 0; i < _data->Starts.size(); ++i)
    {
        QuestStartDef const& start = _data->Starts[i];
        if (!start.HasAnchor || start.Map != bot.map_id || (memory.StartIndex >= 0 && i == std::size_t(memory.StartIndex)))
            continue;
        float const dist = Distance2d(bot, start.Anchor);
        // A start made for the whole world (its quests carry levels) may be walked to from anywhere on the map; a starter-zone start only from nearby.
        bool world = false;
        for (QuestDef const& quest : start.Quests)
            world = world || quest.QuestLevel != 0;
        if (world || dist <= kStartRadiusYards)
            byDistance.emplace_back(dist, i);
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
                return WalkOrAct(bot, memory, quest->Giver, QuestStepKind::Accept, quest->Id, nowMs);
            }
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

bool QuestPlanner::NoteRefused(QuestMemory& memory, std::uint64_t nowMs) const
{
    if (memory.LastActionQuest == 0)
        return false;
    bool const handIn = memory.PendingHandIn == memory.LastActionQuest;
    // The block grows with every refusal of the same quest: a minute, ten minutes, then an hour (done long ago, a prerequisite missing, not for
    // this bot). A refused hand-in is most often a full bag, which the bot fixes at a vendor and which says nothing against the quest, so a
    // hand-in is blocked a minute for its first two refusals.
    std::uint32_t const times = ++memory.Refusals[memory.LastActionQuest];
    std::uint32_t const step = handIn && times > 1 ? times - 1 : times; // a hand-in's second refusal counts as the first
    std::uint64_t const block = step == 1 ? kBlockAfterRefusalMs : step == 2 ? 10 * kBlockAfterRefusalMs : kBlockAfterAbandonMs;
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
