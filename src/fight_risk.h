// SPDX-License-Identifier: GPL-2.0-or-later
// Fight-risk retreat (2026-11-19, FightRiskRetreat, default off; doc/survival.md). The sim arm C (2026-11-19) had about one pull in twelve end in the bot's death,
// warriors 38.5 and rogues 29 deaths per bot: attrition with no recovery inside a fight. A solo bot now estimates, while it fights, how long it lasts against how long
// its target lasts, and when it is losing it drinks a potion or bandages first (the supplies it carries), then walks away toward the last place it stood safe, and
// only then (FightRiskTeleport, needs the core's Bot.Actions.TeleportInCombat) takes a rescue teleport to a level-safe spot of the same map.
//
// The estimate: incoming damage per second is the larger of the bot's own health loss over the window (the snapshots, net of heals) and the big hits the core reported
// in the window (protocol 29 damage events, the died line's last_hits); outgoing damage per second is the health its target lost over the window. Time to die = health
// / incoming, time to kill = target health / outgoing (no damage on the target: never). Losing: the bot is at or below kJudgeBelowHealthPct and time to die is less
// than time to kill times MarginPct / 100 (above 100 a closer fight counts as lost). Nothing is judged before kMinFightMs of a fight.
// Pure logic, unit-tested in tests/survival_test.cpp.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace fight_risk
{

constexpr std::uint64_t kWindowMs = 6000;            // the estimate's window
constexpr std::uint64_t kMinFightMs = 3000;          // a fight younger than this is not judged
constexpr std::uint32_t kJudgeBelowHealthPct = 70;   // a bot above this health is never losing
constexpr std::uint64_t kHitQuietMs = 3000;          // a bandage only when nothing hit the bot this long (a hit breaks it)
constexpr std::uint64_t kDisengageMs = 8000;         // walked away this long and still losing: the rescue teleport (when allowed)
constexpr std::uint64_t kTeleportCooldownMs = 600000; // one rescue teleport per bot per 10 minutes
constexpr std::size_t kSamples = 16;

struct Options
{
    bool Enabled = false;          // FightRiskRetreat (default 0)
    std::uint32_t MarginPct = 100; // FightRiskMarginPct (default 100, 50..400): above 100 the bot leaves earlier
    bool Teleport = false;         // FightRiskTeleport (default 0): only with the core's Bot.Actions.TeleportInCombat = 1
};

struct Estimate
{
    bool Known = false;            // the fight is old enough and has two samples in the window
    float InDps = 0.0f;
    float OutDps = 0.0f;
    float TimeToDieS = 0.0f;       // 1e9 for never
    float TimeToKillS = 0.0f;      // 1e9 for never
    bool Losing = false;
};

// The bot's own health and its target's, one sample per decide turn of a fight.
class Watch
{
public:
    void Reset();
    // A sample of a fight: the bot's health and its target (0 none) with that target's health.
    void Note(std::uint64_t nowMs, std::uint32_t health, std::uint64_t target, std::uint32_t targetHealth);
    // `hitDamage`: the damage of the big hits reported in the last kWindowMs. `healthPct` the bot's health percent now.
    Estimate Judge(Options const& options, std::uint64_t nowMs, std::uint32_t healthPct, std::uint32_t hitDamage) const;
    std::uint64_t FightSinceMs() const { return _fightSinceMs; }

private:
    struct Sample
    {
        std::uint64_t AtMs = 0;
        std::uint32_t Health = 0;
        std::uint64_t Target = 0;
        std::uint32_t TargetHealth = 0;
    };
    std::array<Sample, kSamples> _samples{};
    std::size_t _count = 0;   // newest at _samples[_count - 1]
    std::uint64_t _fightSinceMs = 0;
};

// What to do about a losing fight, in order: a potion, a bandage, walk away, the rescue teleport.
enum class Step : std::uint8_t { kNone, kPotion, kBandage, kDisengage, kTeleport };
char const* StepName(Step step);

struct Facts
{
    bool Losing = false;
    bool HasPotion = false;        // a health potion carried (layout 16)
    bool PotionReady = false;      // the shared potion cooldown is over
    bool HasBandage = false;
    bool Bandaged = false;         // already bandaged in this fight
    bool HitRecently = false;      // a hit in the last kHitQuietMs
    bool Disengaging = false;      // already walking away
    std::uint64_t DisengageForMs = 0;
    bool TeleportAllowed = false;  // the key, solo, open world, not on a transport
    bool TeleportReady = false;    // the cooldown is over and no teleport was sent in this fight
};

Step Next(Facts const& facts);

} // namespace fight_risk
