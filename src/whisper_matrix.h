// SPDX-License-Identifier: GPL-2.0-or-later
// Whisper reply matrix (config WhisperMatrix, off by default): an incoming whisper is matched against small keyword lists (where are you, inv, group, lfg,
// what level, help) and answered from a template pool chosen by the bot's state (grinding, questing, travelling, dungeon, full group). The existing
// WhisperLimiter still applies. A template is never sent to the same player twice within 10 minutes. Pure logic, tests/whisper_matrix_test.cpp.
// Templates, data/whisper_replies.txt: "keyword|state|text" (state "any" fits all), {level} is filled in.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace whisper_matrix {

struct Settings
{
    bool Enabled = false;                 // WhisperMatrix
    std::uint64_t RepeatMs = 10ull * 60 * 1000;
};

enum class Keyword : std::uint8_t { None, Where, Invite, Group, Lfg, Level, Help };
enum class State : std::uint8_t { Grinding, Questing, Traveling, Dungeon, FullGroup };

// The first keyword list that matches a whole word or phrase of the text (case and punctuation ignored).
Keyword Match(std::string const& text);
char const* KeywordName(Keyword keyword);
char const* StateName(State state);

struct Template
{
    Keyword Key = Keyword::None;
    int State_ = -1;          // -1: any
    std::string Text;
};
std::vector<Template> Parse(std::vector<std::string> const& lines);

class Matrix
{
public:
    // The reply for this whisper, or "" (no match, no template for the state, or every fitting template was sent to this player in the last RepeatMs).
    std::string Reply(Settings const& settings, std::vector<Template> const& templates, std::string const& whisper, State state, std::uint32_t level,
                      std::uint64_t player, std::uint64_t seed, std::uint64_t bot, std::uint64_t nowMs);

private:
    std::map<std::pair<std::uint64_t, std::string>, std::uint64_t> _sent;   // (player, template text) -> when
};

}  // namespace whisper_matrix
