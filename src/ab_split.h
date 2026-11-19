// SPDX-License-Identifier: GPL-2.0-or-later
// ArmSplit: two configs in one sim run (doc/sim-ab.md). The bots are split by guid parity (even = arm A, odd = arm B); arm B reads the config with the
// overrides of ArmBOverrides ("PathVariety=1,LevelSafeWalks=0") and of the Arm.B.<Key> keys; every event line with a bot= field gets " arm=A|B".
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ab_split
{

enum class Arm : std::uint8_t
{
    A = 0,
    B = 1,
};

// The arm of a bot: with the split off every bot is arm A; with it on the odd guids are arm B. One AND, no table: cheap enough for every option read.
inline Arm ArmOf(bool split, std::uint64_t guid) { return split && (guid & 1u) ? Arm::B : Arm::A; }
inline char ArmLetter(Arm arm) { return arm == Arm::B ? 'B' : 'A'; }

// "Key=Value,Key=Value": the pairs in order. Spaces around keys and values are trimmed; an empty item is skipped; an item without '=' or with an empty
// key is dropped with a warning.
std::vector<std::pair<std::string, std::string>> ParseOverrides(std::string const& text, std::vector<std::string>& warnings);

// The config arm B reads: `config` with the ArmBOverrides pairs applied, then the Arm.B.<Key> keys (they win over the list). The split's own keys
// (Arm*, Trace*) cannot be overridden (warning). `applied` gets "Key=new (A: old|default)" for each key changed, for the start-up line. The Arm.B.* keys
// themselves stay in the copy (ParseBotSettings ignores them).
std::unordered_map<std::string, std::string> ArmBConfig(std::unordered_map<std::string, std::string> const& config, std::vector<std::string>& warnings,
                                                        std::vector<std::string>* applied = nullptr);

// "235,66" or "235 66": the bot guids of TraceBots. A token that is not a whole number is dropped with a warning.
std::unordered_set<std::uint64_t> ParseGuidList(std::string const& text, std::vector<std::string>& warnings);

// The guid of the bot an event line is about: the first " bot=<n>" (or a line that starts with "<event>: bot=<n>"). 0 when none.
std::uint64_t BotOfLine(std::string const& line);

// Process-wide: whether event lines are tagged (set once at start, before the pump thread runs; read from every thread).
void SetTagging(bool on);
bool Tagging();

// The event line with " arm=A|B" appended when tagging is on and the line names a bot; unchanged otherwise.
std::string Tag(std::string line);
// " arm=A|B" for a bot when tagging is on, "" otherwise (for the lines written field by field).
std::string ArmField(std::uint64_t guid);

} // namespace ab_split
