#include "quest_data.h"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace
{

bool ReadPoint(std::istringstream& line, QuestPoint& out)
{
    return bool(line >> out.X >> out.Y >> out.Z);
}

QuestDef* FindQuest(QuestStartDef& start, std::uint32_t id)
{
    for (QuestDef& quest : start.Quests)
        if (quest.Id == id)
            return &quest;
    return nullptr;
}

} // namespace

QuestData ParseQuestData(std::istream& in, std::vector<std::string>& warnings)
{
    QuestData data;
    std::string text;
    std::size_t lineNumber = 0;
    while (std::getline(in, text))
    {
        ++lineNumber;
        if (std::size_t const hash = text.find('#'); hash != std::string::npos)
            text.erase(hash);
        std::istringstream line(text);
        std::string keyword;
        if (!(line >> keyword))
            continue;

        auto const drop = [&](char const* why)
        {
            warnings.push_back("quest file line " + std::to_string(lineNumber) + ": " + why + "; ignored.");
        };

        if (keyword == "start")
        {
            std::string name;
            line >> name;
            data.Starts.emplace_back();
            data.Starts.back().Name = name;
            continue;
        }
        if (data.Starts.empty())
        {
            drop("a record before the first 'start'");
            continue;
        }
        QuestStartDef& start = data.Starts.back();

        if (keyword == "anchor")
        {
            if (line >> start.Map && ReadPoint(line, start.Anchor))
                start.HasAnchor = true;
            else
                drop("anchor needs <map> <x> <y> <z>");
        }
        else if (keyword == "quest")
        {
            QuestDef quest;
            std::string giver, ender;
            if (line >> quest.Id >> giver && giver == "giver" && ReadPoint(line, quest.Giver) && line >> ender && ender == "ender" &&
                ReadPoint(line, quest.Ender) && !FindQuest(start, quest.Id))
                start.Quests.push_back(std::move(quest));
            else
                drop("quest needs a new id, 'giver <x> <y> <z>' and 'ender <x> <y> <z>'");
        }
        else if (keyword == "hunt")
        {
            std::uint32_t id = 0;
            QuestObjectiveDef objective;
            std::string entries;
            QuestDef* quest = nullptr;
            if (line >> id >> objective.Position >> objective.Need >> entries && (quest = FindQuest(start, id)) &&
                objective.Position < 4 && objective.Need > 0)
            {
                std::istringstream list(entries);
                std::string item;
                while (std::getline(list, item, ','))
                    if (!item.empty())
                        objective.Entries.push_back(std::uint32_t(std::strtoul(item.c_str(), nullptr, 10)));
                if (objective.Entries.empty())
                    drop("hunt needs at least one creature entry");
                else
                    quest->Objectives.push_back(std::move(objective));
            }
            else
                drop("hunt needs <quest> <position 0..3> <need> <entries>, for a quest defined above");
        }
        else if (keyword == "area")
        {
            std::uint32_t id = 0;
            QuestPoint point;
            QuestDef* quest = nullptr;
            if (line >> id && (quest = FindQuest(start, id)) && ReadPoint(line, point))
                quest->Areas.push_back(point);
            else
                drop("area needs <quest> <x> <y> <z>, for a quest defined above");
        }
        else if (keyword == "req")
        {
            std::uint32_t id = 0;
            QuestDef* quest = nullptr;
            if (!(line >> id) || !(quest = FindQuest(start, id)))
            {
                drop("req needs a quest defined above");
                continue;
            }
            std::string key;
            bool ok = true;
            while (ok && (line >> key))
            {
                // A race or class mask may be wider than 32 bits (all bits set, -1 as unsigned 64): it is cut to the 32 bits the bot has race and
                // class ids for, and a value too large to read at all means any race or class. Other values must fit in 32 bits.
                std::string token;
                if (!(line >> token))
                {
                    ok = false;
                    break;
                }
                std::uint64_t wide = 0;
                auto const parsed = std::from_chars(token.data(), token.data() + token.size(), wide);
                bool const isMask = key == "races" || key == "classes";
                if (parsed.ec == std::errc::result_out_of_range && isMask)
                    wide = 0;
                else if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || (!isMask && wide > 0xFFFFFFFFull))
                {
                    ok = false;
                    break;
                }
                std::uint32_t const value = std::uint32_t(wide & 0xFFFFFFFFull);
                if (key == "minlevel")
                    quest->MinLevel = value;
                else if (key == "level")
                    quest->QuestLevel = value;
                else if (key == "prev")
                    quest->PrevQuest = value;
                else if (key == "races")
                    quest->RaceMask = value;
                else if (key == "classes")
                    quest->ClassMask = value;
                else
                    ok = false;
            }
            if (!ok)
                drop("req takes 'minlevel|level|prev|races|classes <number>' pairs");
        }
        else
            drop("unknown record");
    }
    return data;
}

QuestData LoadQuestData(std::string const& path, std::vector<std::string>& warnings)
{
    std::ifstream file(path);
    if (!file)
    {
        warnings.push_back("quest file " + path + " cannot be read; quests are off.");
        return {};
    }
    return ParseQuestData(file, warnings);
}
