// SPDX-License-Identifier: GPL-2.0-or-later
// ArmSplit: two configs in one sim run (doc/sim-ab.md). The bots are split by guid parity (even = arm A, odd = arm B; ArmSplitMode = guid) or per stratum
// (ArmSplitMode = strata: Skyborne or race, crossed with class, alternating A/B within each stratum, see Strata); arm B reads the config with the overrides of
// ArmBOverrides ("PathVariety=1,LevelSafeWalks=0") and of the Arm.B.<Key> keys; every event line with a bot= field gets " arm=A|B".
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <shared_mutex>
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

// How the bots are split (ArmSplitMode): by guid parity, or per stratum (Strata).
enum class SplitMode : std::uint8_t
{
    Guid = 0,
    Strata = 1,
};

// "guid" or "strata" (case as written); false for anything else.
bool ParseSplitMode(std::string const& text, SplitMode& mode);
char const* SplitModeName(SplitMode mode);

// The guid parity arm: odd guids are arm B.
inline Arm ParityArm(std::uint64_t guid) { return (guid & 1u) ? Arm::B : Arm::A; }

// ArmSplitMode = strata: each bot gets its arm at first sight from a per-stratum count, so every stratum ends within one bot of 50/50 and the arms within one
// bot of each other overall (for the bots of one batch; across batches each stratum still holds). The stratum is the race group (Skyborne, races 95 and 96,
// is one group; every other race its own) crossed with the class; race or class 0 (unknown) is a stratum of its own. A bot keeps its arm for the life of
// the object (offline and back: the same arm). Thread-safe.
class Strata
{
public:
    struct Bot
    {
        std::uint64_t Guid = 0;
        std::uint32_t Race = 0;
        std::uint32_t Class = 0;
    };
    // The stratum key of a race and class: (race group << 32) | class, the race group of Skyborne being kSkyborneGroup.
    static constexpr std::uint32_t kSkyborneGroup = 0xFFFFu;
    static std::uint64_t KeyOf(std::uint32_t race, std::uint32_t cls);

    // The bots of one batch (a tick's view): those without an arm get one, in stratum order and within a stratum in guid order, so the result does not
    // depend on the order of `bots`. A stratum that leans one way gives its next bot to the other arm; within a stratum the pairs go A then B; the one left
    // over of a stratum goes to the arm that has fewer bots overall (A on a tie). Already assigned bots and repeated guids are skipped.
    void Assign(std::vector<Bot> bots);
    // One bot (a batch of one); returns its arm (the stored one when it has one).
    Arm Assign(std::uint64_t guid, std::uint32_t race, std::uint32_t cls);
    // The stored arm; false when the bot has none yet.
    bool Find(std::uint64_t guid, Arm& arm) const;
    // Bots per arm, and per stratum (A, B): for the start-up line and the tests.
    std::array<std::size_t, 2> Totals() const;
    std::map<std::uint64_t, std::array<std::size_t, 2>> PerStratum() const;
    void Clear();

private:
    mutable std::shared_mutex _lock;
    std::unordered_map<std::uint64_t, Arm> _arm;                       // bot guid -> arm
    std::map<std::uint64_t, std::array<std::size_t, 2>> _count;        // stratum -> bots per arm
    std::array<std::size_t, 2> _total{};
};

// Process-wide: the split mode (set once at start, before the pump thread runs) and the strata of the process (strata mode).
void SetMode(SplitMode mode);
SplitMode Mode();
Strata& ProcessStrata();

// The arm of a bot: with the split off every bot is arm A; with it on, guid mode: the odd guids are arm B (one AND, no table); strata mode: the stored arm of
// ProcessStrata, and the parity arm for a bot not yet seen by the policy (an event line from another thread before its first turn).
Arm StrataArmOf(std::uint64_t guid);
inline Arm ArmOf(bool split, std::uint64_t guid)
{
    if (!split)
        return Arm::A;
    return Mode() == SplitMode::Strata ? StrataArmOf(guid) : ParityArm(guid);
}
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
