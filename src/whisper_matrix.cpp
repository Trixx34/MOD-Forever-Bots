// SPDX-License-Identifier: GPL-2.0-or-later
#include "whisper_matrix.h"

#include "advisor_text.h"
#include "bot_random.h"

#include <cctype>

namespace whisper_matrix {
namespace {

struct Phrases
{
    Keyword Key;
    std::vector<std::string> List;
};

// Checked in this order: "lfg" and "inv" are looked for before the looser "group".
std::vector<Phrases> const& Table()
{
    static std::vector<Phrases> const table = {
        { Keyword::Where, { "where are you", "where r u", "where you at", "wher are you", "your location" } },
        { Keyword::Level, { "what level", "what lvl", "which level", "your level", "how high level" } },
        { Keyword::Lfg, { "lfg", "looking for group", "lf group", "lfm" } },
        { Keyword::Invite, { "inv", "invite", "invite me", "pls inv", "inv pls" } },
        { Keyword::Group, { "group", "party", "join you", "join your group", "team up" } },
        { Keyword::Help, { "help", "can you help", "need help", "help me" } },
    };
    return table;
}

std::string Normalize(std::string const& text)
{
    std::string out = " ";
    for (unsigned char c : text)
    {
        if (std::isalnum(c))
            out += char(std::tolower(c));
        else if (out.back() != ' ')
            out += ' ';
    }
    if (out.back() != ' ')
        out += ' ';
    return out;
}

}  // namespace

Keyword Match(std::string const& text)
{
    std::string const norm = Normalize(text);
    for (Phrases const& entry : Table())
        for (std::string const& phrase : entry.List)
            if (norm.find(" " + phrase + " ") != std::string::npos)
                return entry.Key;
    return Keyword::None;
}

char const* KeywordName(Keyword keyword)
{
    switch (keyword)
    {
        case Keyword::Where: return "where";
        case Keyword::Invite: return "inv";
        case Keyword::Group: return "group";
        case Keyword::Lfg: return "lfg";
        case Keyword::Level: return "level";
        case Keyword::Help: return "help";
        default: return "none";
    }
}

char const* StateName(State state)
{
    switch (state)
    {
        case State::Grinding: return "grinding";
        case State::Questing: return "questing";
        case State::Traveling: return "traveling";
        case State::Dungeon: return "dungeon";
        case State::FullGroup: return "fullgroup";
    }
    return "any";
}

std::vector<Template> Parse(std::vector<std::string> const& lines)
{
    std::vector<Template> out;
    for (std::string const& line : lines)
    {
        std::size_t const a = line.find('|');
        std::size_t const b = a == std::string::npos ? a : line.find('|', a + 1);
        if (b == std::string::npos)
            continue;
        Template t;
        std::string const key = line.substr(0, a), state = line.substr(a + 1, b - a - 1);
        for (Keyword k : { Keyword::Where, Keyword::Invite, Keyword::Group, Keyword::Lfg, Keyword::Level, Keyword::Help })
            if (key == KeywordName(k))
                t.Key = k;
        if (t.Key == Keyword::None)
            continue;
        if (state != "any")
        {
            t.State_ = -1;
            for (State s : { State::Grinding, State::Questing, State::Traveling, State::Dungeon, State::FullGroup })
                if (state == StateName(s))
                    t.State_ = int(s);
            if (t.State_ < 0)
                continue;
        }
        t.Text = line.substr(b + 1);
        if (!t.Text.empty())
            out.push_back(std::move(t));
    }
    return out;
}

std::string Matrix::Reply(Settings const& settings, std::vector<Template> const& templates, std::string const& whisper, State state, std::uint32_t level,
                          std::uint64_t player, std::uint64_t seed, std::uint64_t bot, std::uint64_t nowMs)
{
    if (!settings.Enabled)
        return {};
    Keyword const key = Match(whisper);
    if (key == Keyword::None)
        return {};
    // Forget old sends so the map stays small.
    if (_sent.size() > 4096)
        for (auto it = _sent.begin(); it != _sent.end();)
            it = nowMs - it->second >= settings.RepeatMs ? _sent.erase(it) : std::next(it);
    std::vector<Template const*> specific, general;
    for (Template const& t : templates)
    {
        if (t.Key != key)
            continue;
        auto const sent = _sent.find({ player, t.Text });
        if (sent != _sent.end() && nowMs - sent->second < settings.RepeatMs)
            continue;
        if (t.State_ == int(state))
            specific.push_back(&t);
        else if (t.State_ < 0)
            general.push_back(&t);
    }
    std::vector<Template const*> const& pool = !specific.empty() ? specific : general;
    if (pool.empty())
        return {};
    Template const& chosen = *pool[bot_random::Below(bot_random::Draw(seed, bot, 0xC1, nowMs / 1000 + player), std::uint32_t(pool.size()))];
    _sent[{ player, chosen.Text }] = nowMs;
    return advisor::Replace(chosen.Text, "level", std::to_string(level));
}

}  // namespace whisper_matrix
