// SPDX-License-Identifier: GPL-2.0-or-later
// main_detail helpers: paths, names, text and dungeon routes (declared in main_internal.h).
#include "main_internal.h"

namespace main_detail {

std::string BesideTheModule(std::string const& path)
{
    if (path.empty() || path.front() == '/')
        return path;
    std::error_code ec;
    std::filesystem::path const exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
    {
        std::filesystem::path const candidate = exe.parent_path() / path;
        if (std::filesystem::exists(candidate, ec))
            return candidate.string();
    }
    return path;
}

char const* ActionTypeName(forever_module::ActionType type)
{
    switch (type)
    {
        case forever_module::ActionType::kCastSpell: return "cast";
        case forever_module::ActionType::kMoveTo: return "move";
        case forever_module::ActionType::kAttackTarget: return "attack";
        case forever_module::ActionType::kFollow: return "follow";
        case forever_module::ActionType::kStop: return "stop";
        case forever_module::ActionType::kRest: return "rest";
        case forever_module::ActionType::kVendor: return "vendor";
        case forever_module::ActionType::kTrain: return "train";
        case forever_module::ActionType::kGroup: return "group";
        case forever_module::ActionType::kLoot: return "loot";
        case forever_module::ActionType::kQuest: return "quest";
        case forever_module::ActionType::kTeleport: return "teleport";
        case forever_module::ActionType::kGrant: return "grant";
        case forever_module::ActionType::kAutoGear: return "autogear";
        case forever_module::ActionType::kReleaseSpirit: return "release";
        case forever_module::ActionType::kResurrect: return "resurrect";
        case forever_module::ActionType::kReclaimCorpse: return "reclaim";
        case forever_module::ActionType::kConsume: return "consume";
        case forever_module::ActionType::kSkillUp: return "skillup";
        case forever_module::ActionType::kCraft: return "craft";
        case forever_module::ActionType::kGather: return "gather";
        case forever_module::ActionType::kSummon: return "summon";
        case forever_module::ActionType::kRevive: return "revive";
        case forever_module::ActionType::kMount: return "mount";
        case forever_module::ActionType::kPetCommand: return "pet";
        case forever_module::ActionType::kTaxi: return "taxi";
        case forever_module::ActionType::kUseObject: return "useobject";
        case forever_module::ActionType::kSellToVendor: return "sell";
        default: break;
    }
    return "action";
}

std::string JoinWords(std::vector<std::string> const& words, std::size_t from)
{
    std::string out;
    for (std::size_t i = from; i < words.size(); ++i)
        out += (out.empty() ? "" : " ") + words[i];
    return out;
}

std::string LowerCopy(std::string text)
{
    return text_case::ToLower(std::move(text));
}

std::map<std::uint32_t, PolicyOptions::DungeonRoute> DungeonRoutes(std::vector<Dungeon> const& dungeons)
{
    std::map<std::uint32_t, PolicyOptions::DungeonRoute> routes;
    for (Dungeon const& dungeon : dungeons)
    {
        PolicyOptions::DungeonRoute route;
        for (std::size_t i = 0; i < dungeon.Route.size(); ++i)
        {
            route.Route.push_back({ dungeon.Route[i].X, dungeon.Route[i].Y, dungeon.Route[i].Z });
            if (dungeon.Route[i].BossEntry != 0)
                route.BossAt[i] = dungeon.Route[i].BossEntry;
        }
        route.Events = dungeon.Events;
        route.Id = dungeon.Id;
        route.Name = dungeon.Name;
        route.MinLevel = dungeon.MinLevel;
        route.MaxLevel = dungeon.MaxLevel;
        // The instance entrance (the dungeon file's entry: map x y z), where a ghost of the party is revived.
        route.HasEntry = true;
        route.Entry = { dungeon.Entry[1], dungeon.Entry[2], dungeon.Entry[3] };
        // The bosses' avoid lists (bosses[].strategy.avoid_spells and avoid): the spells to step out of and the fixed places to stay out of.
        for (DungeonBoss const& boss : dungeon.Bosses)
        {
            route.AvoidSpells.insert(route.AvoidSpells.end(), boss.Strategy.AvoidSpells.begin(), boss.Strategy.AvoidSpells.end());
            for (AvoidZone const& zone : boss.Strategy.Avoid)
                route.AvoidZones.push_back(ground_avoid::Circle{ zone.X, zone.Y, zone.Radius });
        }
        routes[dungeon.MapId()] = std::move(route);
    }
    return routes;
}

std::string ActionsPerTick(std::uint64_t actions, std::uint64_t ticks)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", ticks ? double(actions) / double(ticks) : 0.0);
    return text;
}

} // namespace main_detail
