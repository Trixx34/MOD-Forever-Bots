// SPDX-License-Identifier: GPL-2.0-or-later
// Realism features that touch no combat decision (docs/bot-realism-backlog.md, items 2 to 5): idle fidgeting while a flight is awaited, the reading delay before a
// quest accept or a trainer visit, the passing-player buff, and town AFK time. All off by default. Pure logic with the clock and the random draw passed in;
// unit-tested in tests/realism_test.cpp. Nothing here picks a target, a spell of the rotation, or reacts to a fight: the policy asks these rules only when the
// bot is out of combat, has no target and no pull pending.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace realism {

// ---- Item 2: idle fidgeting (config IdleFidget*) ----
struct FidgetSettings
{
    bool Enabled = false;                   // IdleFidget
    std::uint32_t ChancePct = 30;           // IdleFidgetChancePct: the chance that a due check does something
    std::uint32_t MinGapSec = 3;            // IdleFidgetMinSec / IdleFidgetMaxSec: the gap between two checks of one bot (drawn per bot and time)
    std::uint32_t MaxGapSec = 10;
    std::uint32_t StandWeight = 2;          // IdleFidgetStandWeight: a stop order
    std::uint32_t StepWeight = 2;           // IdleFidgetStepWeight: a short step to one side
    std::uint32_t TurnWeight = 1;           // IdleFidgetTurnWeight: a short step the other way (the bot turns to face it)
    std::uint32_t AfterOrderMs = 4000;      // the first check comes this long after the flight order, so the take-off is never disturbed
};

enum class Fidget : std::uint8_t { None, Stand, Step, Turn };

struct FidgetState
{
    std::uint64_t NextMs = 0;               // the next check is due at this time (0: draw the first gap)
    std::uint32_t Counter = 0;
};

// One tick of a waiting bot: None most of the time. Each bot draws its own gaps and choices from the seed, so bots do not fidget together.
Fidget NextFidget(FidgetSettings const& settings, FidgetState& state, std::uint64_t seed, std::uint64_t bot, std::uint64_t nowMs);

// ---- Item 3: reading delay (config ReadDelay*) ----
struct ReadSettings
{
    bool Enabled = false;                   // ReadDelay
    std::uint32_t MinSec = 3;               // ReadDelayMinSec
    std::uint32_t MaxSec = 12;              // ReadDelayMaxSec
};

// The bot opened a quest or trainer window at OpenedMs and may act when the delay has passed.
struct ReadGate
{
    std::uint64_t Key = 0;                  // what is read: a quest id, or a trainer marker
    std::uint64_t OpenedMs = 0;             // 0: not open
    std::uint64_t DelayMs = 0;
};

// The delay for one opening, within [MinSec, MaxSec] seconds.
std::uint64_t ReadDelayMs(ReadSettings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint64_t counter);

// True when the bot may accept or train now. With the feature off it is always true. The first call for a key opens the gate; Close() after the action.
bool ReadReady(ReadSettings const& settings, ReadGate& gate, std::uint64_t key, std::uint64_t seed, std::uint64_t bot, std::uint64_t nowMs);
inline void Close(ReadGate& gate) { gate = ReadGate{}; }

// ---- Item 4: passing-player buff (config PassingBuff*) ----
struct BuffSettings
{
    bool Enabled = false;                   // PassingBuff
    std::uint32_t ChancePct = 10;           // PassingBuffChancePct: the chance per check
    std::uint32_t ManaAbovePct = 80;        // PassingBuffManaAbovePct: the mana the bot must have
    std::uint32_t CooldownSec = 600;        // PassingBuffCooldownSec: per bot
    std::uint32_t ZonePerMin = 2;           // PassingBuffZonePerMin: buffs a minute over all bots of one map
    std::uint32_t ReserveCasts = 4;         // PassingBuffReserveCasts: casts of the bot's own fight rotation whose mana is always kept back
    float RangeYards = 15.0f;
};

struct BuffFacts
{
    bool InCombat = false;
    bool HasTarget = false;
    bool Alive = true;
    bool Moving = false;
    bool Casting = false;
    bool ManaUser = false;                  // power type mana with a maximum
    std::uint32_t Power = 0;
    std::uint32_t MaxPower = 0;
    std::uint32_t BuffCost = 0;             // 0: the cost is not known, and then the bot does not buff
    std::uint32_t RotationReserve = 0;      // the mana of the dearest ReserveCasts casts of the bot's fight rotation (ReserveMana())
    float TargetDistance = 1e9f;            // to the friendly player
};

// The mana the bot keeps for its own fights: the sum of the `casts` dearest costs.
std::uint32_t ReserveMana(std::vector<std::uint32_t> costs, std::uint32_t casts);

// Whether a buff may go out, apart from the chance: out of combat, mana above the threshold, and after the buff still the rotation's reserve left.
bool BuffAllowed(BuffSettings const& settings, BuffFacts const& facts, std::uint64_t nowMs, std::uint64_t botNextMs);

// A rate over a window, per key (a map): at most `perMinute` grants in any minute.
class ZoneRate
{
public:
    bool TryGrant(std::uint32_t zone, std::uint64_t nowMs, std::uint32_t perMinute);

private:
    std::map<std::uint32_t, std::vector<std::uint64_t>> _grants;
};

// ---- Item 5: town AFK (config TownAfk*) ----
struct AfkSettings
{
    bool Enabled = false;                   // TownAfk
    std::uint32_t ChancePct = 10;           // TownAfkChancePct: the chance that a bot arriving at a hub stays
    std::uint32_t MinMin = 5;               // TownAfkMinMin / TownAfkMaxMin: the stay
    std::uint32_t MaxMin = 20;
    std::uint32_t ZoneMax = 5;              // TownAfkZoneMax: idle bots at once per map
};

// The stay in ms, within [MinMin, MaxMin] minutes.
std::uint64_t AfkDurationMs(AfkSettings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint64_t counter);
// Whether the bot arriving at a hub takes AFK time (the chance only; the zone cap is AfkZones::TryStart).
bool AfkRoll(AfkSettings const& settings, std::uint64_t seed, std::uint64_t bot, std::uint64_t counter);

// Who is AFK on each map, to keep the number per map under the cap.
class AfkZones
{
public:
    // Starts a stay for `bot` on `zone` until untilMs unless `zoneMax` bots are staying there. Ended stays are dropped first.
    bool TryStart(std::uint32_t zone, std::uint64_t bot, std::uint64_t nowMs, std::uint64_t untilMs, std::uint32_t zoneMax);
    void End(std::uint64_t bot);
    std::size_t Count(std::uint32_t zone, std::uint64_t nowMs);

private:
    std::map<std::uint32_t, std::map<std::uint64_t, std::uint64_t>> _stays; // zone -> bot -> until
};

} // namespace realism
