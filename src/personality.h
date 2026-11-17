// SPDX-License-Identifier: GPL-2.0-or-later
// Bot personalities (config Personality, off by default): five stable traits per bot, derived from the seed and the guid, that tune behaviours that already exist by
// a small, bounded amount so bots do not all act alike: the health at which an idle bot rests, how long it idles before it walks, a short reaction delay before a
// pull, how large a pack it is willing to pull, a small chance to pull a different mob than the best one, and the town AFK chance and length.
// Pure logic, no state: tests/personality_test.cpp. The policy asks these rules only for a SOLO bot out of combat, outside a dungeon route and outside a raid;
// nothing here touches a rotation, a survival rule (flee, heal, rest-before-pull gate, level and elite guards), a party or a route. Every effect is
// scaled by its own strength key, and 0 switches that effect off.
#pragma once

#include <cstddef>
#include <cstdint>

namespace personality {

struct Settings
{
    bool Enabled = false;                    // Personality
    std::uint32_t RestStrength = 50;         // PersonalityRestStrength (0..100): the rest threshold moves up to +-8 points at 100
    std::uint32_t IdleStrength = 50;         // PersonalityIdleStrength (0..100): the idle dwell before a walk is scaled by up to +-60 percent at 100
    std::uint32_t ReactionMaxMs = 3000;      // PersonalityReactionMaxMs (0..10000): the longest reaction delay before a pull (the slowest bot); 0 = none
    std::uint32_t CautionStrength = 50;      // PersonalityCautionStrength (0..100): how many bots are wary of the largest packs; 0 = none
    std::uint32_t MistakeMaxPct = 5;         // PersonalityMistakeMaxPct (0..30): the chance per pull of the least cautious bot to take another mob than the best one
    std::uint32_t SocialStrength = 50;       // PersonalitySocialStrength (0..100): the town AFK chance and stay vary by up to +-100 percent at 100;
                                             // also the greeting chances (MeetGreet, CityGreet) and how soon a guild invite is looked for
    std::uint32_t SessionStrength = 50;      // PersonalitySessionStrength (0..100): with Presence on, the session length is scaled by up to +-60 percent at 100
    std::uint32_t JitterMaxMs = 400;         // PersonalityJitterMaxMs (0..2000): the longest reaction delay on an interrupt window or a heal call (the slowest bot); 0 = none
    std::uint32_t ForgetBuffPct = 3;         // PersonalityForgetBuffPct (0..30): the chance per upkeep buff, for the least cautious bot, to put it off for 30 to 120 s; 0 = none
    bool Instances = false;                  // PersonalityInstances: the jitter and the forgotten buffs also apply in dungeons and raids (off: only in the open world)
};

// Each trait is 0..100, drawn once per bot (50 is the middle; a bot's traits never change).
struct Traits
{
    std::uint32_t Pace = 50;                 // boldness / speed: high = short reaction delay and short idle dwell
    std::uint32_t Caution = 50;              // high = rests earlier, avoids big packs, rarely makes a mistake
    std::uint32_t Sociability = 50;          // high = more town AFK
    std::uint32_t Session = 50;              // high = long town AFK stays
    std::uint32_t Patience = 50;             // high = long idle dwell
};

constexpr std::uint32_t kRestFloorPct = 70;  // the rest threshold stays within [70, 98], as RandomPct's draw does
constexpr std::uint32_t kRestCeilPct = 98;
constexpr std::uint32_t kMaxReactionMs = 10000;
constexpr std::uint32_t kMaxPullSkips = 3;   // a wary bot skips at most this many packs in a row, then pulls: it never starves

constexpr std::uint32_t kMaxJitterMs = 2000;
constexpr std::uint64_t kForgetMinMs = 30000;   // a forgotten buff is put off for this long at least ...
constexpr std::uint64_t kForgetMaxMs = 120000;  // ... and this long at most

Traits TraitsFor(std::uint64_t seed, std::uint64_t bot);

// The rest threshold: `base` moved by the caution trait (cautious bots rest earlier), clamped to [kRestFloorPct, kRestCeilPct]. Off: base.
std::uint32_t RestPct(Settings const& settings, Traits const& traits, std::uint32_t base);

// The idle dwell before the next walk: `baseMs` scaled by patience and (inverse) pace, within [0.4, 1.6] of baseMs. Off: baseMs.
std::uint64_t IdleDwellMs(Settings const& settings, Traits const& traits, std::uint64_t baseMs);

// The reaction delay before a pull (a draw per pull `counter`), within [0, ReactionMaxMs * (100 - Pace) / 100]. Off or ReactionMaxMs 0: 0.
std::uint64_t ReactionDelayMs(Settings const& settings, Traits const& traits, std::uint64_t seed, std::uint64_t bot, std::uint64_t counter);

// The largest pack the bot pulls: `base` (the shared cap), one less for a wary bot (Caution at or above 100 - CautionStrength / 2), never below 1.
std::size_t PackCap(Settings const& settings, Traits const& traits, std::size_t base);

// Whether this bot makes a small mistake on this pull and takes another mob than the best one (the caller still applies every pull guard to it).
// The chance is MistakeMaxPct * (100 - Caution) / 100 percent.
bool MakesMistake(Settings const& settings, Traits const& traits, std::uint64_t seed, std::uint64_t bot, std::uint64_t counter);

// The town AFK chance: `basePct` scaled by sociability (0 to twice at full strength), at most 100. Off: basePct.
std::uint32_t AfkChancePct(Settings const& settings, Traits const& traits, std::uint32_t basePct);

// The town AFK stay: `baseMs` scaled by the session trait (0 to twice at full strength). Off: baseMs.
std::uint64_t AfkStayMs(Settings const& settings, Traits const& traits, std::uint64_t baseMs);

// The greeting chance (MeetGreet, CityGreet): `basePct` scaled by sociability like the AFK chance (SocialStrength). Off: basePct.
std::uint32_t GreetChancePct(Settings const& settings, Traits const& traits, std::uint32_t basePct);

// How long a bot without a guild waits before it looks for a guild invite again: `baseMs` scaled by sociability, within [0.5, 1.5] of baseMs (sociable bots look sooner). Off: baseMs.
std::uint64_t GuildPollMs(Settings const& settings, Traits const& traits, std::uint64_t baseMs);

// The presence session length: `baseMs` scaled by the session trait, within [0.4, 1.6] of baseMs (SessionStrength). Off: baseMs.
std::uint64_t SessionMs(Settings const& settings, Traits const& traits, std::uint64_t baseMs);

// The same as a percent of the scheduler's draw (100 = unchanged), for presence::Online::SessionPct.
std::uint32_t SessionPct(Settings const& settings, Traits const& traits);

// Whether the imperfections (jitter, forgotten buffs) may apply here: always in the open world, in a dungeon or raid only with Instances.
bool ImperfectionAllowed(Settings const& settings, bool instanced);

// A reaction delay on an event (an interrupt window, a heal call; `counter` is one draw per event), within [0, JitterMaxMs * (100 - Pace) / 100]. Off, JitterMaxMs 0 or not allowed here: 0.
std::uint64_t JitterMs(Settings const& settings, Traits const& traits, bool instanced, std::uint64_t seed, std::uint64_t bot, std::uint64_t counter);

// Whether the bot forgets this upkeep buff for a while (`counter` is one draw per buff decision). The chance is ForgetBuffPct * (100 - Caution) / 100 percent, at most 30.
bool ForgetsBuff(Settings const& settings, Traits const& traits, bool instanced, std::uint64_t seed, std::uint64_t bot, std::uint64_t counter);

// How long a forgotten buff is put off: kForgetMinMs to kForgetMaxMs.
std::uint64_t ForgetMs(std::uint64_t seed, std::uint64_t bot, std::uint64_t counter);

} // namespace personality
