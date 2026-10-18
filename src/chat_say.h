// What a bot says out loud when it takes a chat order (chat_commands.h). Pure logic, unit-tested in tests/chat_say_test.cpp. BotPolicy collects
// the bots that accepted an order, main.cpp sends the lines as /say through the core (BotSpeak, kind 0).
//
// In a big group the lines would drown the chat, so ChatSayMax caps how many bots speak for one order; which bots is decided by a hash of the bot
// and the verb, so it is the same on every run but not always the same bots. A bot that was whispered is addressed on its own and always answers.
#pragma once

#include "chat_commands.h"

#include <cstdint>
#include <string>
#include <vector>

struct ChatAck
{
    std::uint64_t Bot = 0;     // the bot's character guid counter
    std::string Text;
};

// The line this bot says for this verb, or "" when the verb gets none (status already answers by whisper). The wording varies per bot.
std::string AckLine(ChatVerb verb, std::uint64_t botGuid);

// From the bots that accepted `verb` (guids), the ones that speak: all of them when `addressed` (one bot was whispered), else at most `maxSpeakers`
// (0 = none), chosen by a stable hash.
std::vector<std::uint64_t> PickSpeakers(std::vector<std::uint64_t> const& accepted, ChatVerb verb, bool addressed, std::uint32_t maxSpeakers);
