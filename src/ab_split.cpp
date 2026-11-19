// SPDX-License-Identifier: GPL-2.0-or-later
#include "ab_split.h"

#include <algorithm>
#include <charconv>

namespace ab_split
{

namespace
{

std::atomic<bool> g_tagging{ false };

std::string Trim(std::string const& text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '"'))
        ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '"'))
        --end;
    return text.substr(begin, end - begin);
}

// The split's own keys: arm B cannot switch the split or the trace.
bool Protected(std::string const& key)
{
    return key.rfind("Arm", 0) == 0 || key.rfind("Trace", 0) == 0;
}

void Apply(std::unordered_map<std::string, std::string>& out, std::unordered_map<std::string, std::string> const& original, std::string const& key,
           std::string const& value, std::vector<std::string>& warnings, std::vector<std::string>* applied)
{
    if (Protected(key))
    {
        warnings.push_back("ArmB override " + key + ": the split's own keys (Arm*, Trace*) are the same for both arms; ignored.");
        return;
    }
    auto const before = original.find(key);
    out[key] = value;
    if (applied)
        applied->push_back(key + "=" + value + " (A: " + (before == original.end() ? std::string("default") : before->second) + ")");
}

} // namespace

std::vector<std::pair<std::string, std::string>> ParseOverrides(std::string const& text, std::vector<std::string>& warnings)
{
    std::vector<std::pair<std::string, std::string>> pairs;
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t const comma = text.find(',', start);
        std::string const item = Trim(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        start = comma == std::string::npos ? text.size() + 1 : comma + 1;
        if (item.empty())
            continue;
        std::size_t const eq = item.find('=');
        std::string const key = eq == std::string::npos ? std::string() : Trim(item.substr(0, eq));
        if (key.empty())
        {
            warnings.push_back("ArmBOverrides: '" + item + "' is not Key=Value; dropped.");
            continue;
        }
        pairs.emplace_back(key, Trim(item.substr(eq + 1)));
    }
    return pairs;
}

std::unordered_map<std::string, std::string> ArmBConfig(std::unordered_map<std::string, std::string> const& config, std::vector<std::string>& warnings,
                                                        std::vector<std::string>* applied)
{
    std::unordered_map<std::string, std::string> out = config;
    auto const list = config.find("ArmBOverrides");
    if (list != config.end())
        for (auto const& [key, value] : ParseOverrides(list->second, warnings))
            Apply(out, config, key, value, warnings, applied);
    std::string const prefix = "Arm.B.";
    // In key order, so the start-up line and the warnings do not depend on the hash order.
    std::vector<std::pair<std::string, std::string>> keyed;
    for (auto const& [key, value] : config)
        if (key.size() > prefix.size() && key.rfind(prefix, 0) == 0)
            keyed.emplace_back(key.substr(prefix.size()), value);
    std::sort(keyed.begin(), keyed.end());
    for (auto const& [key, value] : keyed)
        Apply(out, config, key, value, warnings, applied);
    return out;
}

std::unordered_set<std::uint64_t> ParseGuidList(std::string const& text, std::vector<std::string>& warnings)
{
    std::unordered_set<std::uint64_t> guids;
    std::string token;
    auto const flush = [&]()
    {
        std::string const item = Trim(token);
        token.clear();
        if (item.empty())
            return;
        std::uint64_t guid = 0;
        auto const [end, error] = std::from_chars(item.data(), item.data() + item.size(), guid);
        if (error != std::errc() || end != item.data() + item.size() || guid == 0)
            warnings.push_back("TraceBots: '" + item + "' is not a bot guid; dropped.");
        else
            guids.insert(guid);
    };
    for (char c : text)
    {
        if (c == ',' || c == ' ' || c == '\t')
            flush();
        else
            token += c;
    }
    flush();
    return guids;
}

std::uint64_t BotOfLine(std::string const& line)
{
    std::size_t at = line.find(" bot=");
    if (at == std::string::npos)
        return 0;
    at += 5;
    std::uint64_t guid = 0;
    auto const [end, error] = std::from_chars(line.data() + at, line.data() + line.size(), guid);
    return error == std::errc() && end != line.data() + at ? guid : 0;
}

void SetTagging(bool on) { g_tagging.store(on, std::memory_order_relaxed); }
bool Tagging() { return g_tagging.load(std::memory_order_relaxed); }

std::string Tag(std::string line)
{
    if (!Tagging())
        return line;
    std::uint64_t const bot = BotOfLine(line);
    if (bot == 0)
        return line;
    line += " arm=";
    line += ArmLetter(ArmOf(true, bot));
    return line;
}

std::string ArmField(std::uint64_t guid)
{
    if (!Tagging() || guid == 0)
        return std::string();
    return std::string(" arm=") + ArmLetter(ArmOf(true, guid));
}

} // namespace ab_split
