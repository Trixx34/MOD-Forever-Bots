// SPDX-License-Identifier: GPL-2.0-or-later
#include "ab_split.h"

#include <algorithm>
#include <charconv>

namespace ab_split
{

namespace
{

std::atomic<bool> g_tagging{ false };
std::atomic<SplitMode> g_mode{ SplitMode::Guid };

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

bool ParseSplitMode(std::string const& text, SplitMode& mode)
{
    std::string const value = Trim(text);
    if (value == "guid")
        mode = SplitMode::Guid;
    else if (value == "strata")
        mode = SplitMode::Strata;
    else
        return false;
    return true;
}

char const* SplitModeName(SplitMode mode) { return mode == SplitMode::Strata ? "strata" : "guid"; }

std::uint64_t Strata::KeyOf(std::uint32_t race, std::uint32_t cls)
{
    std::uint32_t const group = (race == 95 || race == 96) ? kSkyborneGroup : race;
    return (std::uint64_t(group) << 32) | cls;
}

void Strata::Assign(std::vector<Bot> bots)
{
    std::unique_lock<std::shared_mutex> lock(_lock);
    // The new bots by stratum, each in guid order.
    std::sort(bots.begin(), bots.end(), [](Bot const& x, Bot const& y) {
        std::uint64_t const kx = KeyOf(x.Race, x.Class);
        std::uint64_t const ky = KeyOf(y.Race, y.Class);
        return kx != ky ? kx < ky : x.Guid < y.Guid;
    });
    auto const give = [this](std::uint64_t guid, std::uint64_t key, Arm arm) {
        _arm[guid] = arm;
        ++_count[key][std::size_t(arm)];
        ++_total[std::size_t(arm)];
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> leftOver;   // (guid, stratum): one per stratum at most, placed last by the overall count
    std::size_t i = 0;
    while (i < bots.size())
    {
        std::uint64_t const key = KeyOf(bots[i].Race, bots[i].Class);
        std::vector<std::uint64_t> fresh;
        for (; i < bots.size() && KeyOf(bots[i].Race, bots[i].Class) == key; ++i)
            if (!_arm.count(bots[i].Guid) && (fresh.empty() || fresh.back() != bots[i].Guid))
                fresh.push_back(bots[i].Guid);
        std::size_t next = 0;
        std::array<std::size_t, 2> const have = _count[key];
        // A stratum that leans one way (by one at most) evens out first.
        if (next < fresh.size() && have[0] != have[1])
            give(fresh[next++], key, have[0] < have[1] ? Arm::A : Arm::B);
        for (; next + 1 < fresh.size(); next += 2)
        {
            give(fresh[next], key, Arm::A);
            give(fresh[next + 1], key, Arm::B);
        }
        if (next < fresh.size())
            leftOver.emplace_back(fresh[next], key);
    }
    for (auto const& [guid, key] : leftOver)
        give(guid, key, _total[1] < _total[0] ? Arm::B : Arm::A);
}

Arm Strata::Assign(std::uint64_t guid, std::uint32_t race, std::uint32_t cls)
{
    Assign(std::vector<Bot>{ Bot{ guid, race, cls } });
    Arm arm = Arm::A;
    Find(guid, arm);
    return arm;
}

bool Strata::Find(std::uint64_t guid, Arm& arm) const
{
    std::shared_lock<std::shared_mutex> lock(_lock);
    auto const it = _arm.find(guid);
    if (it == _arm.end())
        return false;
    arm = it->second;
    return true;
}

std::array<std::size_t, 2> Strata::Totals() const
{
    std::shared_lock<std::shared_mutex> lock(_lock);
    return _total;
}

std::map<std::uint64_t, std::array<std::size_t, 2>> Strata::PerStratum() const
{
    std::shared_lock<std::shared_mutex> lock(_lock);
    return _count;
}

void Strata::Clear()
{
    std::unique_lock<std::shared_mutex> lock(_lock);
    _arm.clear();
    _count.clear();
    _total = {};
}

void SetMode(SplitMode mode) { g_mode.store(mode, std::memory_order_relaxed); }
SplitMode Mode() { return g_mode.load(std::memory_order_relaxed); }

Strata& ProcessStrata()
{
    static Strata strata;
    return strata;
}

Arm StrataArmOf(std::uint64_t guid)
{
    Arm arm = ParityArm(guid);
    ProcessStrata().Find(guid, arm);
    return arm;
}

} // namespace ab_split
