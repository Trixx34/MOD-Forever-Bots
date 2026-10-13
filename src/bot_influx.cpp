#include "bot_influx.h"

#include <algorithm>
#include <array>

using forever_module::BotSnapshot;
using forever_module::QuestEntry;
using influx_stats::Point;

namespace {

constexpr std::uint8_t kQuestComplete = 2;

std::int64_t Int(std::uint64_t value)
{
    return static_cast<std::int64_t>(value);
}

} // namespace

std::string BotInfluxStats::ClassName(std::uint32_t classId)
{
    static char const* const names[] = {"", "Warrior", "Paladin", "Hunter", "Rogue", "Priest", "Death Knight", "Shaman", "Mage",
                                        "Warlock", "Monk", "Druid", "Demon Hunter", "Evoker"};
    if (classId >= 1 && classId < sizeof(names) / sizeof(names[0]))
        return names[classId];
    return "class" + std::to_string(classId);
}

std::string BotInfluxStats::LevelBandLabel(std::uint32_t band)
{
    if (band + 1 >= kLevelBands)
        return std::to_string((kLevelBands - 1) * 10) + "+";
    return std::to_string(band * 10) + "-" + std::to_string(band * 10 + 9);
}

void BotInfluxStats::OnEvent(std::string const& kind, std::uint64_t /*botGuid*/)
{
    if (kind == "death")
        ++_deaths;
    else if (kind == "fight_start")
        ++_fights;
    else if (kind == "stuck")
        ++_stuckEpisodes;
    else if (kind == "level")
        ++_levelUps;
}

void BotInfluxStats::Observe(forever_module::WorldTickSnapshotView const& view, std::uint64_t nowMs)
{
    // Quests handed in: a quest that was complete and has left the log.
    for (BotSnapshot const& bot : view.bots)
    {
        std::unordered_set<std::uint32_t>& complete = _completeQuests[bot.guid];
        std::unordered_set<std::uint32_t> present;
        for (std::size_t i = 0; i < bot.quest_count && i < forever_module::kMaxQuestLog; ++i)
        {
            QuestEntry const& quest = bot.quests[i];
            if (quest.quest_id == 0)
                continue;
            present.insert(quest.quest_id);
            if (quest.status == kQuestComplete)
                complete.insert(quest.quest_id);
        }
        for (auto it = complete.begin(); it != complete.end();)
        {
            if (present.count(*it) == 0)
            {
                ++_questsDone;
                it = complete.erase(it);
            }
            else
                ++it;
        }
    }

    if (!_started)
    {
        _started = true;
        _nextSampleMs = nowMs + _options.SampleEveryMs;
        return;
    }
    if (nowMs < _nextSampleMs)
        return;
    _nextSampleMs = nowMs + _options.SampleEveryMs;
    Emit(view);
}

void BotInfluxStats::Emit(forever_module::WorldTickSnapshotView const& view)
{
    std::int64_t const now = _options.NowNs ? _options.NowNs() : 0;

    std::uint64_t alive = 0, dead = 0, inCombat = 0, moving = 0, inGroup = 0;
    std::array<std::uint64_t, 32> byClass{};
    std::array<std::uint64_t, kLevelBands> byBand{};
    for (BotSnapshot const& bot : view.bots)
    {
        if (bot.health == 0)
        {
            ++dead;
            continue;
        }
        ++alive;
        inCombat += bot.in_combat ? 1 : 0;
        moving += bot.moving ? 1 : 0;
        inGroup += bot.group_size > 1 ? 1 : 0;
        std::uint32_t const classId = _options.ClassOf ? _options.ClassOf(bot.guid) : 0;
        if (classId != 0 && classId < byClass.size())
            ++byClass[classId];
        ++byBand[std::min<std::uint32_t>(bot.level / 10, kLevelBands - 1)];
    }

    Point bots;
    bots.Measurement = "bots";
    bots.Fields = {{"controlled", Int(view.bots.size())}, {"alive", Int(alive)}, {"dead", Int(dead)},
                   {"in_combat", Int(inCombat)}, {"moving", Int(moving)}, {"in_group", Int(inGroup)}};
    bots.TimestampNs = now;
    _sink(std::move(bots));

    // Every class that has a bot, or has had one: a class that dropped to zero shows as zero, not as a gap.
    for (std::uint32_t classId = 1; classId < byClass.size(); ++classId)
    {
        if (byClass[classId] == 0 && _seenClasses.count(classId) == 0)
            continue;
        _seenClasses.insert(classId);
        Point point;
        point.Measurement = "bots_by_class";
        point.Tags = {{"class", ClassName(classId)}};
        point.Fields = {{"online", Int(byClass[classId])}};
        point.TimestampNs = now;
        _sink(std::move(point));
    }

    for (std::uint32_t band = 0; band < kLevelBands; ++band)
    {
        Point point;
        point.Measurement = "bots_by_level";
        point.Tags = {{"band", LevelBandLabel(band)}};
        point.Fields = {{"online", Int(byBand[band])}};
        point.TimestampNs = now;
        _sink(std::move(point));
    }

    Point events;
    events.Measurement = "bot_events";
    events.Fields = {{"deaths", Int(_deaths)}, {"fights", Int(_fights)}, {"stuck_episodes", Int(_stuckEpisodes)},
                     {"level_ups", Int(_levelUps)}, {"quests_done", Int(_questsDone)}};
    events.TimestampNs = now;
    _sink(std::move(events));
}
