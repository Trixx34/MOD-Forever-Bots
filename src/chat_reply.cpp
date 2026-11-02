// SPDX-License-Identifier: GPL-2.0-or-later
#include "chat_reply.h"

#include <algorithm>
#include <iterator>
#include <cctype>
#include <initializer_list>
#include <cstdint>
#include <string>

namespace {

constexpr std::uint64_t kMinuteMs = 60000;

// Lower-case words separated by single spaces; every other character is a separator.
std::string Normalize(std::string const& text)
{
    std::string out;
    bool space = true;
    for (char c : text)
    {
        unsigned char const u = static_cast<unsigned char>(c);
        if (std::isalnum(u))
        {
            out += static_cast<char>(std::tolower(u));
            space = false;
        }
        else if (!space)
        {
            out += ' ';
            space = true;
        }
    }
    if (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// The phrase occurs in `words` as whole words.
bool HasPhrase(std::string const& words, char const* phrase)
{
    std::string const p = phrase;
    std::size_t at = 0;
    while ((at = words.find(p, at)) != std::string::npos)
    {
        bool const startOk = at == 0 || words[at - 1] == ' ';
        std::size_t const end = at + p.size();
        bool const endOk = end == words.size() || words[end] == ' ';
        if (startOk && endOk)
            return true;
        ++at;
    }
    return false;
}

bool HasAny(std::string const& words, std::initializer_list<char const*> phrases)
{
    return std::any_of(phrases.begin(), phrases.end(), [&words](char const* p) { return HasPhrase(words, p); });
}

char const* ActivityPhrase(BotActivity activity)
{
    switch (activity)
    {
        case BotActivity::Grinding: return "grinding nearby";
        case BotActivity::Following: return "following my group";
        case BotActivity::Holding: return "holding a spot";
        case BotActivity::Travelling: return "on my way somewhere";
        case BotActivity::Dungeon: return "running a dungeon";
        case BotActivity::Fighting: return "in a fight";
    }
    return "grinding nearby";
}

} // namespace

WhisperIntent ClassifyWhisper(std::string const& text)
{
    std::string const words = Normalize(text);
    if (words.empty())
        return WhisperIntent::Other;
    // Most specific first: "where are you" is Where, not Status.
    if (HasAny(words, { "where are you", "where are u", "where r u", "where you at", "where at", "where u at" }))
        return WhisperIntent::Where;
    if (HasAny(words, { "need a group", "need group", "want a group", "want to group", "group up", "join your group", "can i join", "can i join you", "invite me" }))
        return WhisperIntent::NeedGroup;
    if (HasAny(words, { "status", "what are you doing", "what you doing", "whatcha doing", "what s up", "how are you", "what are you up to" }))
        return WhisperIntent::Status;
    if (HasAny(words, { "thanks", "thank you", "thx", "ty" }))
        return WhisperIntent::Thanks;
    if (HasAny(words, { "hi", "hello", "hey", "yo", "sup", "good morning", "good evening" }))
        return WhisperIntent::Hello;
    return WhisperIntent::Other;
}

BusyKind BusyReason(bool inCombat, bool dead, bool inDungeon, bool travelling)
{
    if (dead)
        return BusyKind::Dead;
    if (inCombat)
        return BusyKind::Combat;
    if (inDungeon)
        return BusyKind::Dungeon;
    if (travelling)
        return BusyKind::Travelling;
    return BusyKind::None;
}

std::string ReplyLine(WhisperIntent intent, std::uint64_t botGuid, BotActivity activity)
{
    std::size_t const variant = static_cast<std::size_t>(botGuid % 3);
    std::string const what = ActivityPhrase(activity);
    switch (intent)
    {
        case WhisperIntent::Other:
            return "";
        case WhisperIntent::Hello:
        {
            static char const* const lines[] = { "Hi there.", "Hey, how's it going?", "Hello!" };
            return lines[variant];
        }
        case WhisperIntent::Status:
        {
            std::string const lines[] = { "Right now I'm " + what + ".", "I'm " + what + " at the moment.", "At the moment I'm " + what + "." };
            return lines[variant];
        }
        case WhisperIntent::Where:
        {
            std::string const lines[] = { "I'm " + what + ", not far from here.", "Somewhere close by, " + what + ".", "Not far. I'm " + what + "." };
            return lines[variant];
        }
        case WhisperIntent::NeedGroup:
        {
            static char const* const lines[] = { "Invite me to your group and I'll join you.", "Send me an invite and I'll come along.",
                                                 "If you want a group, invite me and I'll join." };
            return lines[variant];
        }
        case WhisperIntent::Thanks:
        {
            static char const* const lines[] = { "You're welcome.", "No problem.", "Anytime." };
            return lines[variant];
        }
    }
    return "";
}

std::string BusyLine(std::uint64_t botGuid, BusyKind busy)
{
    std::size_t const variant = static_cast<std::size_t>(botGuid % 3);
    switch (busy)
    {
        case BusyKind::Dead:
        {
            static char const* const lines[] = { "I'm dead right now. Invite me once I'm back.", "I'm a ghost at the moment, try again soon.", "I'm dead for now, ask me again in a bit." };
            return lines[variant];
        }
        case BusyKind::Combat:
        {
            static char const* const lines[] = { "I'm in a fight right now. Invite me in a moment.", "Busy in a fight, try again in a bit.", "Give me a moment, I'm fighting." };
            return lines[variant];
        }
        case BusyKind::Dungeon:
        {
            static char const* const lines[] = { "I'm running a dungeon right now.", "I'm in a dungeon, try again when I'm out.", "Busy in a dungeon at the moment." };
            return lines[variant];
        }
        case BusyKind::Travelling:
        {
            static char const* const lines[] = { "I'm on my way somewhere, ask me again soon.", "I'm travelling right now.", "Busy travelling, try again in a bit." };
            return lines[variant];
        }
        case BusyKind::None:
        {
            static char const* const lines[] = { "I'm busy right now, try again later.", "Not free at the moment, try again later.", "Busy for now, ask me again later." };
            return lines[variant];
        }
    }
    return "";
}

std::string ClampReply(std::string text)
{
    if (text.size() > kMaxReplyBytes)
        text.resize(kMaxReplyBytes);
    return text;
}

bool WhisperLimiter::Allow(std::uint64_t player, std::uint64_t bot, std::uint64_t nowMs)
{
    if (_lastPair.size() + _replies.size() > kMaxTracked)
        Prune(nowMs);
    auto const pair = _lastPair.find({ player, bot });
    if (pair != _lastPair.end() && nowMs - pair->second < _cooldownMs)
        return false;
    std::deque<std::uint64_t>& recent = _replies[player];
    while (!recent.empty() && nowMs - recent.front() >= kMinuteMs)
        recent.pop_front();
    if (recent.size() >= _maxPerMin)
        return false;
    recent.push_back(nowMs);
    _lastPair[{ player, bot }] = nowMs;
    return true;
}

void WhisperLimiter::Prune(std::uint64_t nowMs)
{
    for (auto it = _lastPair.begin(); it != _lastPair.end();)
        it = nowMs - it->second >= _cooldownMs ? _lastPair.erase(it) : std::next(it);
    for (auto it = _replies.begin(); it != _replies.end();)
    {
        while (!it->second.empty() && nowMs - it->second.front() >= kMinuteMs)
            it->second.pop_front();
        it = it->second.empty() ? _replies.erase(it) : std::next(it);
    }
    // Still past the bound (every entry is inside its cooldown): the oldest pair goes.
    while (_lastPair.size() > kMaxTracked)
        _lastPair.erase(std::min_element(_lastPair.begin(), _lastPair.end(), [](auto const& a, auto const& b) { return a.second < b.second; }));
}
