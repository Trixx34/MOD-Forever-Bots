// SPDX-License-Identifier: GPL-2.0-or-later
// Whisper replies and busy refusals (ChatWhisperReplies, off by default). A player who whispers a bot that is not a chat command gets a short
// answer from it: a greeting, what it is doing (no position, no inventory), or that it needs an invite. A bot that is busy (dead, in a fight,
// in a dungeon, travelling) refuses an invite or a trade with a line saying so. Pure logic, unit-tested in tests/chat_reply_test.cpp; BotPolicy
// decides when to answer, main.cpp sends the lines as whispers (BotSpeak, kind 1).
//
// Limits: a reply is at most kMaxReplyBytes. A player gets at most WhisperMaxPerMin replies in a minute across all bots, and at most one reply per
// (player, bot) every WhisperCooldownMs; past that the bot stays silent. Replies never go to our own bots (the policy checks), and a whisper that
// is a chat command or comes from a trade partner never reaches this code (main.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>

constexpr std::size_t kMaxReplyBytes = 255;

enum class WhisperIntent : std::uint8_t
{
    Other,      // not something a bot answers
    Hello,
    Status,     // "what are you doing"
    Where,      // "where are you"
    NeedGroup,  // "need a group", "can I join you"
    Thanks,
};

enum class BusyKind : std::uint8_t
{
    None,
    Dead,       // a corpse or a ghost
    Combat,
    Dungeon,
    Travelling,
};

// What the bot is doing, for the status and where lines. Chosen by the policy from the snapshot and its state; never a position.
enum class BotActivity : std::uint8_t
{
    Grinding,
    Following,  // follows a player (chat order)
    Holding,    // holds its position (chat order)
    Travelling,
    Dungeon,
    Fighting,
};

// The intent of a whisper that is not a chat command: words matched whole, case and punctuation ignored. Other when none matches.
WhisperIntent ClassifyWhisper(std::string const& text);

// The busy state of a bot: the first that applies, in the order dead, combat, dungeon, travelling. None when the bot is free.
BusyKind BusyReason(bool inCombat, bool dead, bool inDungeon, bool travelling);

// The answer to a whisper. Empty for Other. The wording varies per bot (its guid picks the variant, the same on every run).
std::string ReplyLine(WhisperIntent intent, std::uint64_t botGuid, BotActivity activity);

// The refusal for an invite or a trade while busy. None gives the general "busy" line.
std::string BusyLine(std::uint64_t botGuid, BusyKind busy);

// The text cut to kMaxReplyBytes.
std::string ClampReply(std::string text);

// Limits the replies: per (player, bot) a cooldown, per player a count in a sliding minute. The maps are bounded: expired entries are dropped
// when they grow past kMaxTracked. Not thread-safe (the pump thread uses it).
class WhisperLimiter
{
public:
    WhisperLimiter(std::uint64_t cooldownMs, std::uint32_t maxPerMin) : _cooldownMs(cooldownMs), _maxPerMin(maxPerMin) {}

    // True when a reply to `player` from `bot` may be sent now, and records it.
    bool Allow(std::uint64_t player, std::uint64_t bot, std::uint64_t nowMs);

    std::size_t Tracked() const { return _lastPair.size() + _replies.size(); }

    static constexpr std::size_t kMaxTracked = 4096;

private:
    void Prune(std::uint64_t nowMs);

    std::uint64_t _cooldownMs;
    std::uint32_t _maxPerMin;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> _lastPair; // (player, bot) -> last reply time
    std::map<std::uint64_t, std::deque<std::uint64_t>> _replies;               // player -> reply times in the last minute
};
