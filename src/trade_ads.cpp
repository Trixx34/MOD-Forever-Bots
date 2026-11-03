// SPDX-License-Identifier: GPL-2.0-or-later
#include "trade_ads.h"

#include "advisor_text.h"
#include "bot_random.h"

namespace trade_ads {

char const* ProfessionName(std::uint32_t line)
{
    switch (line)
    {
        case 164: return "Blacksmithing";
        case 165: return "Leatherworking";
        case 171: return "Alchemy";
        case 185: return "Cooking";
        case 197: return "Tailoring";
        case 202: return "Engineering";
        case 333: return "Enchanting";
        case 755: return "Jewelcrafting";
        case 773: return "Inscription";
        default: return nullptr;
    }
}

char const* ProfessionItem(std::uint32_t line)
{
    switch (line)
    {
        case 164: return "armor and weapons";
        case 165: return "leg armor and armor kits";
        case 171: return "potions and elixirs";
        case 185: return "food";
        case 197: return "bags and cloth armor";
        case 202: return "gadgets and goggles";
        case 333: return "enchants";
        case 755: return "gems and rings";
        case 773: return "glyphs";
        default: return "goods";
    }
}

std::string Next(Settings const& settings, State& state, std::vector<std::string> const& templates, std::uint32_t const skillLine[2],
                 std::uint32_t const skillValue[2], std::uint64_t seed, std::uint64_t bot, std::uint64_t nowMs)
{
    if (!settings.Enabled || templates.empty() || settings.IntervalMin == 0)
        return {};
    int pick = -1;
    for (int i = 0; i < 2; ++i)
        if (ProfessionName(skillLine[i]) != nullptr && skillValue[i] >= settings.MinSkill && (pick < 0 || skillValue[i] > skillValue[pick]))
            pick = i;
    if (pick < 0)
        return {};
    std::uint64_t const interval = std::uint64_t(settings.IntervalMin) * 60000;
    if (state.NextMs == 0)
    {
        state.NextMs = nowMs + bot_random::Draw(seed, bot, 0xA1, state.Counter) % interval + 1;
        return {};
    }
    if (nowMs < state.NextMs)
        return {};
    std::uint32_t const n = state.Counter++;
    state.NextMs = nowMs + interval;
    std::string line = templates[bot_random::Below(bot_random::Draw(seed, bot, 0xA2, n), std::uint32_t(templates.size()))];
    line = advisor::Replace(std::move(line), "profession", ProfessionName(skillLine[pick]));
    line = advisor::Replace(std::move(line), "item", ProfessionItem(skillLine[pick]));
    return line.size() > 255 ? line.substr(0, 255) : line;
}

}  // namespace trade_ads
