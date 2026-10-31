// SPDX-License-Identifier: GPL-2.0-or-later
#include "chat_commands.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <vector>

namespace {

constexpr std::uint32_t kWarrior = 1, kPaladin = 2, kHunter = 3, kRogue = 4, kPriest = 5, kShaman = 7, kMage = 8, kWarlock = 9, kDruid = 11;

std::uint32_t Bit(std::uint32_t classId)
{
    return 1u << classId;
}

std::string Lower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

std::vector<std::string> Words(std::string_view text)
{
    std::vector<std::string> out;
    std::string current;
    for (char c : text)
    {
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            if (!current.empty())
                out.push_back(std::move(current)), current.clear();
        }
        else
            current.push_back(c);
    }
    if (!current.empty())
        out.push_back(std::move(current));
    return out;
}

// One selector part (no commas): the class mask it names, or 0 for a word that is not a selector.
std::uint32_t PartMask(std::string const& part)
{
    std::string word = part;
    // Plural: "warriors", "priests" (not "dps", which is a word of its own).
    if (word.size() > 3 && word.back() == 's' && word != "dps")
        word.pop_back();
    if (word == "tank") return Bit(kWarrior);
    if (word == "healer") return Bit(kPriest) | Bit(kPaladin) | Bit(kShaman) | Bit(kDruid);
    if (word == "dps") return Bit(kHunter) | Bit(kRogue) | Bit(kMage) | Bit(kWarlock);
    if (word == "warrior") return Bit(kWarrior);
    if (word == "paladin") return Bit(kPaladin);
    if (word == "hunter") return Bit(kHunter);
    if (word == "rogue") return Bit(kRogue);
    if (word == "priest") return Bit(kPriest);
    if (word == "shaman") return Bit(kShaman);
    if (word == "mage") return Bit(kMage);
    if (word == "warlock") return Bit(kWarlock);
    if (word == "druid") return Bit(kDruid);
    return 0;
}

} // namespace

bool IsOrderVerb(ChatVerb verb)
{
    switch (verb)
    {
        case ChatVerb::Stop:
        case ChatVerb::Pull:
        case ChatVerb::Heal:
        case ChatVerb::Passive:
        case ChatVerb::Aggressive:
        case ChatVerb::Summon:
        case ChatVerb::Revive:
        case ChatVerb::Mount:
        case ChatVerb::Dismount:
        case ChatVerb::Train:
        case ChatVerb::Lead:
        case ChatVerb::LeadOff:
            return true;
        default:
            return false;
    }
}

bool IsControlVerb(ChatVerb verb)
{
    switch (verb)
    {
        case ChatVerb::Role:
        case ChatVerb::Stance:
        case ChatVerb::Focus:
        case ChatVerb::FocusOff:
        case ChatVerb::Distance:
            return true;
        default:
            return false;
    }
}

char const* ChatVerbName(ChatVerb verb)
{
    switch (verb)
    {
        case ChatVerb::Follow: return "follow";
        case ChatVerb::FollowOff: return "follow off";
        case ChatVerb::Stay: return "stay";
        case ChatVerb::StayOff: return "stay off";
        case ChatVerb::Stop: return "stop";
        case ChatVerb::GotoHere: return "goto here";
        case ChatVerb::Release: return "release";
        case ChatVerb::Status: return "status";
        case ChatVerb::Pull: return "pull";
        case ChatVerb::Heal: return "heal";
        case ChatVerb::Passive: return "passive";
        case ChatVerb::Aggressive: return "aggressive";
        case ChatVerb::Summon: return "summon";
        case ChatVerb::Revive: return "revive";
        case ChatVerb::Mount: return "mount";
        case ChatVerb::Dismount: return "dismount";
        case ChatVerb::Train: return "train";
        case ChatVerb::Role: return "role";
        case ChatVerb::Stance: return "stance";
        case ChatVerb::Focus: return "focus";
        case ChatVerb::FocusOff: return "focus off";
        case ChatVerb::Lead: return "lead";
        case ChatVerb::LeadOff: return "lead off";
        case ChatVerb::Distance: return "distance";
        case ChatVerb::Help: return "help";
        case ChatVerb::None: break;
    }
    return "none";
}

std::vector<std::string> ChatHelpLines(bool orders, bool control)
{
    std::vector<std::string> lines;
    lines.push_back("Selector (optional, first word): all | tank | healer | dps | <class> (also plural), comma-joined for a union. Default: all.");
    lines.push_back("follow [off] - walk after the speaker, or stop.");
    lines.push_back("stay [off] - hold position (fights only what attacks it), or stop holding.");
    lines.push_back("goto here - walk to where the speaker stands (party chat only: needs the speaker's position).");
    lines.push_back("release - a dead bot releases its spirit now.");
    lines.push_back("status - one line per bot, whispered back.");
    lines.push_back("help - this list.");
    if (orders)
    {
        lines.push_back("stop - drop follow, goto and stay.");
        lines.push_back("pull - the tank attacks the speaker's selected target (party chat only: needs a target).");
        lines.push_back("heal - a healer casts its heal on the speaker's selected friend (party chat only: needs a target).");
        lines.push_back("passive - fights only what attacks the bot.");
        lines.push_back("aggressive - back to normal (fights what the group fights or meets).");
        lines.push_back("summon - the bots walk (teleport) next to the speaker.");
        lines.push_back("revive - a dead bot is resurrected at 35% health next to the speaker.");
        lines.push_back("mount / dismount - the fastest ground mount the bot knows.");
        lines.push_back("train - walk to the speaker, who stands at a class or profession trainer, and learn what it teaches (party chat only: needs the speaker's position).");
        lines.push_back("lead [off] - \"tank lead\": in a known dungeon the tank bot walks the group through it and the rest follow it instead of the speaker. \"lead off\" or stop ends it.");
    }
    if (control)
    {
        lines.push_back("role tank|healer|dps|auto - overrides what the class gives.");
        lines.push_back("stance aggressive|defensive|passive - aggressive pulls, defensive only fights what attacks it or the group, passive only what attacks the bot.");
        lines.push_back("focus [off] - the speaker's selected target is attacked first in a fight (party chat only: needs a target).");
        lines.push_back("distance <yards>|default - how far from the speaker a following bot stops (3 to 40 yards).");
    }
    return lines;
}

bool ParseChatSelector(std::string_view token, ChatSelector& out)
{
    std::string const text = Lower(token);
    if (text.empty())
        return false;
    if (text == "all")
    {
        out = ChatSelector{};
        return true;
    }
    std::uint32_t mask = 0;
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t const comma = text.find(',', start);
        std::string const part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        std::uint32_t const partMask = PartMask(part);
        if (partMask == 0)
            return false;
        mask |= partMask;
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    out.All = false;
    out.ClassMask = mask;
    return true;
}

std::optional<ChatCommand> ParseChatCommand(std::string_view text, bool orders, bool control)
{
    std::vector<std::string> words = Words(text);
    if (words.empty() || words.size() > 4)
        return std::nullopt;
    for (std::string& word : words)
        word = Lower(word);

    ChatCommand command;
    std::size_t at = 0;
    if (words.size() >= 2)
    {
        ChatSelector selector;
        if (ParseChatSelector(words[0], selector))
        {
            command.Selector = selector;
            at = 1;
        }
    }

    std::string const& verb = words[at];
    std::size_t const args = words.size() - at - 1;
    std::string const arg = args >= 1 ? words[at + 1] : std::string();
    auto const finish = [&](ChatVerb result) -> std::optional<ChatCommand>
    {
        if ((IsOrderVerb(result) && !orders) || (IsControlVerb(result) && !control))
            return std::nullopt;
        command.Verb = result;
        return command;
    };

    if (verb == "follow") return args == 0 ? finish(ChatVerb::Follow) : (args == 1 && arg == "off" ? finish(ChatVerb::FollowOff) : std::nullopt);
    if (verb == "stay") return args == 0 ? finish(ChatVerb::Stay) : (args == 1 && arg == "off" ? finish(ChatVerb::StayOff) : std::nullopt);
    if (verb == "role")
    {
        if (args == 1 && (arg == "tank" || arg == "healer" || arg == "dps" || arg == "auto"))
        {
            command.Arg = arg;
            return finish(ChatVerb::Role);
        }
        return std::nullopt;
    }
    if (verb == "stance")
    {
        if (args == 1 && (arg == "aggressive" || arg == "defensive" || arg == "passive"))
        {
            command.Arg = arg;
            return finish(ChatVerb::Stance);
        }
        return std::nullopt;
    }
    if (verb == "lead") return args == 0 ? finish(ChatVerb::Lead) : (args == 1 && arg == "off" ? finish(ChatVerb::LeadOff) : std::nullopt);
    if (verb == "focus") return args == 0 ? finish(ChatVerb::Focus) : (args == 1 && arg == "off" ? finish(ChatVerb::FocusOff) : std::nullopt);
    if (verb == "distance")
    {
        if (args != 1)
            return std::nullopt;
        if (arg == "default")
        {
            command.Arg = arg;
            return finish(ChatVerb::Distance);
        }
        float yards = 0.0f;
        auto const parsed = std::from_chars(arg.data(), arg.data() + arg.size(), yards);
        if (parsed.ec != std::errc{} || parsed.ptr != arg.data() + arg.size() || yards < 3.0f || yards > 40.0f)
            return std::nullopt;
        command.Value = yards;
        return finish(ChatVerb::Distance);
    }
    if (verb == "goto") return args == 1 && arg == "here" ? finish(ChatVerb::GotoHere) : std::nullopt;
    if (args != 0)
        return std::nullopt;
    if (verb == "release") return finish(ChatVerb::Release);
    if (verb == "status") return finish(ChatVerb::Status);
    if (verb == "stop") return finish(ChatVerb::Stop);
    if (verb == "pull") return finish(ChatVerb::Pull);
    if (verb == "heal") return finish(ChatVerb::Heal);
    if (verb == "passive") return finish(ChatVerb::Passive);
    if (verb == "aggressive") return finish(ChatVerb::Aggressive);
    if (verb == "summon") return finish(ChatVerb::Summon);
    if (verb == "revive") return finish(ChatVerb::Revive);
    if (verb == "mount") return finish(ChatVerb::Mount);
    if (verb == "dismount") return finish(ChatVerb::Dismount);
    if (verb == "train") return finish(ChatVerb::Train);
    if (verb == "help") return finish(ChatVerb::Help);
    return std::nullopt;
}

void ChatSpreadOffset(std::uint32_t index, float& dx, float& dy)
{
    // A spiral: the first bot beside the speaker, the others on widening turns.
    float const angle = float(index) * 2.4f;
    float const radius = 2.5f + 0.9f * float(index);
    dx = std::cos(angle) * radius;
    dy = std::sin(angle) * radius;
}
