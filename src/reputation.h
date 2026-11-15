// SPDX-License-Identifier: GPL-2.0-or-later
// Reputation goals (data/reputation_goals.txt, tools/gen_reputation_goals.py): a bot picks one faction whose reputation gates equipment, earns the reputation with
// the quests, dungeons and kills it already plays, and visits the faction's vendor when the core says the vendor sells something it can buy now. Off by default
// (ReputationGrind = 1 turns it on). Pure logic: no core, no clock, no file access (tests/reputation_test.cpp). BotPolicy (TurnReputation) feeds it the facts.
//
// What the module cannot know without the core: a bot's standing with a faction, and which vendor sells it a reputation-gated upgrade. The core's reputation
// snapshot block and its kVendor buy bit are described in doc/reputation.md; until the core sends them (PolicyOptions::ReputationFacts is empty) nothing happens.
// The purchase itself is the core's: it picks the best upgrade by its gear score and checks the rank at the vendor, so nothing here selects an item by id.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace reputation {

// The core's ReputationRank (SharedDefines.h).
enum Rank : std::uint8_t { kHated, kHostile, kUnfriendly, kNeutral, kFriendly, kHonored, kRevered, kExalted, kRankCount };

// The standing at which each rank starts (ReputationMgr::ReputationRankThresholds): Hated -42000, Hostile -6000, Unfriendly -3000, Neutral 0, Friendly 3000,
// Honored 9000, Revered 21000, Exalted 42000. The standing is capped at 42000 (Exalted holds 999 more points in the game's own bar).
std::int32_t RankStart(Rank rank);
Rank RankOf(std::int32_t standing);
char const* RankName(Rank rank);
// Points still missing to reach `rank` from `standing` (0 when reached).
std::int32_t PointsTo(Rank rank, std::int32_t standing);

// One faction the bot may work on, a line of data/reputation_goals.txt.
struct Goal
{
    std::uint32_t Faction = 0;
    std::string Name;
    std::uint8_t Side = 0;          // 0 both, 1 Alliance, 2 Horde
    std::uint32_t MinLevel = 0;     // required levels of the gated gear
    std::uint32_t MaxLevel = 0;
    Rank MinRank = kFriendly;       // the lowest rank any of the gear asks for
    std::uint32_t Items = 0;
    std::uint32_t Vendors = 0;      // creatures that sell any of it: 0 means nobody does yet
    std::uint32_t Quests = 0;       // quests that reward reputation with the faction
};

// Parses the file: "faction|name|side|minLevel|maxLevel|minRank|items|vendors|quests". Blank lines and '#' lines are skipped; a malformed line is left out and
// described in `errors` ("line N: ..."), so one bad row does not drop the table.
std::vector<Goal> ParseGoals(std::string_view text, std::vector<std::string>& errors);

// A bot's standing with one faction, from the core's reputation block.
struct Standing
{
    std::uint32_t Faction = 0;
    std::int32_t Value = 0;
};

struct Settings
{
    bool Enabled = false;                    // config key ReputationGrind
    std::uint32_t MinLevel = 10;             // a bot below this level makes no goal (ReputationMinLevel)
    std::uint64_t TripEveryMs = 600000;      // a bot visits a reputation vendor at most this often (ReputationTripSec)
    std::uint64_t FailBackoffMs = 1800000;   // after a visit that bought nothing, the first wait; it doubles with each failure in a row
    std::uint64_t MaxBackoffMs = 21600000;   // six hours
    std::uint64_t MinCopper = 10000;         // a bot with less money than this does not shop for reputation gear (ReputationMinCopper)
};

// What the core tells about a bot (empty/zero when it does not).
struct Facts
{
    std::vector<Standing> Standings;         // the factions the bot has a standing with
    bool VendorValid = false;                // a reachable vendor sells a reputation-gated piece the bot may buy now and that beats what it wears
    float VendorX = 0.0f, VendorY = 0.0f, VendorZ = 0.0f;
    std::uint64_t VendorGuid = 0;
    std::uint32_t Level = 0;
    std::uint8_t Side = 0;                   // 1 Alliance, 2 Horde
    std::uint64_t Copper = 0;
    std::uint32_t FreeSlots = 0;
};

// The goal for a bot: among the goals that fit its side, with gear it can wear within kLevelSlack levels of its own, that a vendor sells, and whose faction the
// bot has not maxed out, the one with the most quests (reputation it can earn by quest play), ties broken by a hash of the guid so the bots spread over the
// factions. `current` (a faction id, 0 for none) is kept while it is still a candidate, so a bot does not change its mind every tick. nullopt: no goal.
constexpr std::uint32_t kLevelSlack = 5;
std::optional<std::size_t> ChooseGoal(std::vector<Goal> const& goals, Facts const& facts, std::uint64_t guid, std::uint32_t current);

// Per-bot memory of the visits (the backoff).
struct Book
{
    std::uint64_t NextTripMs = 0;
    std::uint32_t Failures = 0;
};

// A visit is due: the feature is on, the bot is rich and roomy enough, the core names a vendor, and the wait is over. The walk and the buy are the policy's.
bool TripDue(Settings const& settings, Facts const& facts, Book const& book, std::uint64_t nowMs);

// The visit was sent: the next one waits TripEveryMs. NoteResult records what came of it: bought is true when the bot's money or free bag slots went down.
void NoteTrip(Settings const& settings, Book& book, std::uint64_t nowMs);
void NoteResult(Settings const& settings, Book& book, std::uint64_t nowMs, bool bought);

} // namespace reputation
