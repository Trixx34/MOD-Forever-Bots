// SPDX-License-Identifier: GPL-2.0-or-later
#include "ambient_chat.h"

#include <algorithm>
#include <cctype>

namespace ambient {
namespace {

constexpr std::size_t kMaxBytes = 255;
constexpr std::uint64_t kMinuteMs = 60000;

std::string Trim(std::string s)
{
    auto const notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

bool ParseUnsigned(std::string const& text, std::uint32_t& out)
{
    if (text.empty() || text.size() > 6 || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); }))
        return false;
    out = static_cast<std::uint32_t>(std::stoul(text));
    return true;
}

// Stable and well spread: a bot and a time pick different lines.
std::uint64_t Mix(std::uint64_t bot, std::uint64_t nowMs)
{
    std::uint64_t x = bot * 0x9E3779B97F4A7C15ull + (nowMs / 1000) * 0xBF58476D1CE4E5B9ull;
    x ^= x >> 30;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 27;
    return x;
}

} // namespace

std::vector<LineDef> ParseLines(std::istream& in, std::size_t& errors)
{
    std::vector<LineDef> lines;
    std::string raw;
    while (std::getline(in, raw))
    {
        if (auto const hash = raw.find('#'); hash != std::string::npos && raw.find_first_not_of(" \t") == hash)
            continue; // a whole-line comment; a '#' inside a line is text
        std::string const line = Trim(raw);
        if (line.empty())
            continue;
        // Four '|' separators; the text is everything after the fourth, so it may hold '|' of its own.
        std::vector<std::string> fields;
        std::size_t from = 0;
        for (int i = 0; i < 4; ++i)
        {
            std::size_t const bar = line.find('|', from);
            if (bar == std::string::npos)
                break;
            fields.push_back(Trim(line.substr(from, bar - from)));
            from = bar + 1;
        }
        if (fields.size() != 4)
        {
            ++errors;
            continue;
        }
        fields.push_back(Trim(line.substr(from)));
        LineDef def;
        std::string const chans = fields[0];
        if (chans == "g")
            def.Channels = kGeneralBit;
        else if (chans == "t")
            def.Channels = kTradeBit;
        else if (chans == "*")
            def.Channels = kGeneralBit | kTradeBit;
        else
        {
            ++errors;
            continue;
        }
        if (!ParseUnsigned(fields[1], def.MinLevel) || !ParseUnsigned(fields[2], def.MaxLevel) || def.MaxLevel < def.MinLevel)
        {
            ++errors;
            continue;
        }
        def.Event = fields[3] == "-" ? std::string() : fields[3];
        std::string const text = fields[4];
        if (text.empty() || text.size() > kMaxBytes)
        {
            ++errors;
            continue;
        }
        def.Text = text;
        lines.push_back(std::move(def));
    }
    return lines;
}

std::string ZoneName(std::uint32_t mapId)
{
    switch (mapId)
    {
        case 0: return "Eastern Kingdoms";
        case 1: return "Kalimdor";
        case 530: return "Outland";
        case 571: return "Northrend";
        default: return {};
    }
}

std::string ClassName(std::uint32_t classId)
{
    // The class ids of bot_pick.h.
    switch (classId)
    {
        case 1: return "Warrior";
        case 2: return "Paladin";
        case 3: return "Hunter";
        case 4: return "Rogue";
        case 5: return "Priest";
        case 7: return "Shaman";
        case 8: return "Mage";
        case 9: return "Warlock";
        case 11: return "Druid";
        default: return {};
    }
}

std::string Fill(std::string const& text, Context const& ctx)
{
    std::string out = text;
    auto const replaceAll = [&out](std::string const& token, std::string const& value)
    {
        if (value.empty())
            return false;
        for (std::size_t at = out.find(token); at != std::string::npos; at = out.find(token, at + value.size()))
            out.replace(at, token.size(), value);
        return true;
    };
    if (out.find("{zone}") != std::string::npos && !replaceAll("{zone}", ctx.Zone))
        return {};
    if (out.find("{class}") != std::string::npos && !replaceAll("{class}", ctx.Class))
        return {};
    if (out.find("{level}") != std::string::npos && !replaceAll("{level}", std::to_string(ctx.Level)))
        return {};
    if (out.find('{') != std::string::npos || out.find('}') != std::string::npos || out.size() > kMaxBytes)
        return {};
    return out;
}

Speaker::Speaker(std::vector<LineDef> lines, Limits limits) : _lines(std::move(lines)), _limits(limits)
{
}

bool Speaker::RateAllows(std::uint64_t bot, std::uint64_t nowMs)
{
    auto const last = _lastByBot.find(bot);
    if (last != _lastByBot.end() && nowMs - last->second < _limits.BotCooldownMs)
        return false;
    while (!_sent.empty() && nowMs - _sent.front() >= kMinuteMs)
        _sent.pop_front();
    return _sent.size() < _limits.PerMinute;
}

bool Speaker::Usable(LineDef const& line, Channel chan, Context const& ctx) const
{
    std::uint32_t const bit = chan == Channel::General ? kGeneralBit : kTradeBit;
    if ((line.Channels & bit) == 0)
        return false;
    if (line.MinLevel != 0 || line.MaxLevel != 0)
    {
        if (ctx.Level < line.MinLevel || ctx.Level > line.MaxLevel)
            return false;
    }
    if (line.Event != ctx.Event)
        return false;
    return !Fill(line.Text, ctx).empty();
}

std::optional<std::string> Speaker::Pick(std::uint64_t bot, Channel chan, Context const& ctx, std::uint64_t nowMs)
{
    if (!RateAllows(bot, nowMs))
        return std::nullopt;
    if (!ctx.Event.empty() && _eventDone.count({ bot, ctx.Event }) != 0)
        return std::nullopt;

    std::vector<std::size_t> candidates;
    for (std::size_t i = 0; i < _lines.size(); ++i)
        if (Usable(_lines[i], chan, ctx))
            candidates.push_back(i);
    if (candidates.empty())
        return std::nullopt;

    // The first candidate from a bot-and-time offset that is not in the dedupe ring and is not this bot's last line.
    auto const lastText = _lastTextByBot.find(bot);
    std::size_t const start = static_cast<std::size_t>(Mix(bot, nowMs) % candidates.size());
    for (std::size_t n = 0; n < candidates.size(); ++n)
    {
        std::string const text = Fill(_lines[candidates[(start + n) % candidates.size()]].Text, ctx);
        bool const repeats = std::find(_recent.begin(), _recent.end(), text) != _recent.end() ||
                             (lastText != _lastTextByBot.end() && lastText->second == text);
        if (repeats)
            continue;
        _sent.push_back(nowMs);
        _lastByBot[bot] = nowMs;
        _lastTextByBot[bot] = text;
        _recent.push_back(text);
        while (_recent.size() > _limits.DedupeRing)
            _recent.pop_front();
        if (!ctx.Event.empty())
            _eventDone.insert({ bot, ctx.Event });
        return text;
    }
    return std::nullopt;
}

std::vector<std::uint64_t> DeathWatch::Observe(std::vector<LifeSample> const& bots)
{
    std::vector<std::uint64_t> observers;
    for (LifeSample const& bot : bots)
    {
        auto const last = _lastLife.find(bot.Guid);
        bool const died = last != _lastLife.end() && last->second == 0 && bot.LifeState != 0;
        _lastLife[bot.Guid] = bot.LifeState;
        if (!died)
            continue;
        std::vector<std::uint64_t> candidates;
        for (LifeSample const& other : bots)
            if (other.Guid != bot.Guid && other.Map == bot.Map && other.LifeState == 0 && !other.InCombat)
                candidates.push_back(other.Guid);
        if (!candidates.empty())
            observers.push_back(candidates[static_cast<std::size_t>(Mix(bot.Guid, candidates.size()) % candidates.size())]);
    }
    return observers;
}

bool BossWatch::Grew(std::uint64_t key, std::size_t killed)
{
    auto const at = _last.find(key);
    if (at == _last.end())
    {
        _last[key] = killed;
        return false;
    }
    bool const grew = killed > at->second;
    at->second = killed;
    return grew;
}

} // namespace ambient
