// SPDX-License-Identifier: GPL-2.0-or-later
// Bot guilds: which guild each bot of the pool is in, by guild purpose. Pure logic, tests/bot_guilds_test.cpp. No core needed.
//
// Four purposes (guild kinds), one faction per guild:
//   Leveling  bots below LevelCap. A bot leaves at LevelCap or when its want changes. Not capped in size. An empty leveling guild is dissolved.
//   Utility   dungeons, professions, auction house. Long-term: a bot stays until its want changes. Not capped in size.
//   Raiding   a bot joins only at RaidMinLevel or more and when RaidQualified. Capped at RaidSize bots. It stays until its want changes.
//   Pvp       duels and battlegrounds: a bot joins only at PvpMinLevel or more. Not capped in size. It stays until its want changes.
//
// The pool split (KindOf): a bot with no Want set gets its kind in this order:
//   1. Pvp when PvpShare > 0, its level is at least PvpMinLevel and its guid falls in the PvpShare percent (a guid hash, stable for the bot).
//   2. KindByLevel off (default): from its guid and the shares (GuildsLevelingShare, RaidingShare, UtilityShare; 60/20/20), as before.
//      KindByLevel on: Raiding when RaidQualified, else Leveling below LevelCap, else Utility (a capped bot that is not raid ready yet: pre-raid).
// A bot whose Want is set (by the policy) keeps it. GuildsEnabled is off by default: then PlanMoves plans nothing. PvpShare 0 and KindByLevel off are today's split.
//
// Names (doc/bot-guilds.md): PlanMoves names a new guild "BotLvl A001" and so on. The policy replaces that name with one from the names file
// (data/bot_guilds.txt, guild|style|weight|name) when the file is loaded: PickName draws a name weighted by the row weight times the weight of its style
// for the guild's kind (NameStyles), never one that is taken (case ignored). No file, an empty file or every name taken: the "BotLvl A001" name stays.
//
// Memory: a guild's kind, faction and name are module store rows guild/<guild guid> = "<kind> <A|H> <name>" (EncodeRecord), so a restarted module
// (GuildsRemember) knows its guilds again; the snapshot only carries the guild id.
//
// PlanMoves is the one function that decides membership. It returns one Move per bot that changes, with the guild it leaves (From) and the guild it joins
// (To, or a new guild by index). A bot appears once, so it is never in two guilds at once. Running PlanMoves on the result of the moves gives no moves.
// No guild mixes factions: a guild whose bots are of both factions is reported and left alone, and nobody joins it.
//
// The pool is the bots that are online: the planner only sees what the caller passes, and offline bots are not covered yet.
//
// GuildMates is the roster query: the bots of a purpose (and faction, online, level, raid-ready) that a bot can group with.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bot_guilds {

enum class Purpose : std::uint8_t { Leveling = 0, Utility = 1, Raiding = 2, Pvp = 3 };
constexpr std::size_t kPurposeCount = 4;

// The styles of the names file. The weight of each style per guild kind (NameStyles): 0 leaves the style out for that kind.
enum class NameStyle : std::uint8_t { Lore = 0, Funny = 1, Normal = 2, RealLife = 3 };
constexpr std::size_t kNameStyleCount = 4;
using StyleWeights = std::array<std::uint32_t, kNameStyleCount>;   // indexed by NameStyle

// The default style weights of a kind: leveling and utility take every style alike; raiding leans on lore and normal; pvp takes normal, lore and real life.
StyleWeights DefaultStyleWeights(Purpose kind);

struct Bot;

struct Settings
{
    bool Enabled = false;               // GuildsEnabled
    std::uint32_t LevelCap = 60;        // GuildsLevelCap: a leveling bot at this level leaves
    std::uint32_t RaidMinLevel = 60;    // GuildsRaidMinLevel: a raiding bot must be at least this
    std::uint32_t LevelingShare = 60;   // GuildsLevelingShare (percent of the pool)
    std::uint32_t RaidingShare = 20;    // GuildsRaidingShare
    std::uint32_t UtilityShare = 20;    // GuildsUtilityShare
    std::uint32_t RaidSize = 40;        // GuildsRaidSize: bots per raiding guild (2 to 40)
    bool KindByLevel = false;           // GuildsKindByLevel: the kind follows the bot's level and raid readiness (see KindOf); off = the guid shares
    std::uint32_t PvpShare = 0;         // GuildsPvpShare: percent of the bots at PvpMinLevel or more that want a pvp guild (0 = no pvp guilds)
    std::uint32_t PvpMinLevel = 10;     // GuildsPvpMinLevel (unset: BattlegroundMinLevel, else 10): a pvp bot must be at least this
    bool Remember = false;              // GuildsRemember: read the guild/<guid> store rows at start, so the guilds of the last run keep their kind
    std::string NameFile;               // GuildsNameFile: the names file (guild|style|weight|name). Empty: the old "BotLvl A001" names
    std::array<StyleWeights, kPurposeCount> NameStyles{ DefaultStyleWeights(Purpose::Leveling), DefaultStyleWeights(Purpose::Utility),
                                                        DefaultStyleWeights(Purpose::Raiding), DefaultStyleWeights(Purpose::Pvp) };
                                        // GuildsNameStylesLeveling, ...Utility, ...Raiding, ...Pvp: "lore:3 funny:0 normal:3 reallife:1"
    bool RefusalBackoff = true;         // GuildsRefusalBackoff: a master whose create was refused (or never confirmed) is skipped for a growing window (120 s doubled, 16 min at most), so the creates behind it get the slots
    // The raiding check beyond the level (attunement, gear). Empty: the level alone qualifies.
    std::function<bool(Bot const&)> RaidQualified;
};

// Reads the Guilds* keys from the config map. A bad value is clamped and reported in warnings; shares that do not add up to 100 are replaced by 60/20/20.
Settings LoadSettings(std::unordered_map<std::string, std::string> const& config, std::vector<std::string>& warnings);

// A guild the caller knows (a bot guild). Guid is its guild id in the core, Alliance its one faction.
struct Guild
{
    std::uint64_t Guid = 0;
    Purpose Kind = Purpose::Leveling;
    bool Alliance = false;
};

// One bot of the pool. Guild is its current guild (0 none). Want is the purpose the policy sets (nullopt: the pool share decides).
struct Bot
{
    std::uint64_t Guid = 0;
    std::uint64_t Guild = 0;
    bool Alliance = false;
    bool Online = false;
    std::uint32_t Level = 1;
    std::optional<Purpose> Want;
};

// A bot changes guild: From is the guild it leaves (0 none), To the guild it joins (0 none), or NewGuild is the index in Plan::Created it joins (-1 none).
struct Move
{
    std::uint64_t Bot = 0;
    std::uint64_t From = 0;
    std::uint64_t To = 0;
    int NewGuild = -1;
};

struct NewGuild
{
    std::string Name;                    // "BotLvl A001", "BotUtil H002", "BotRaid A001", "BotPvp H001": tag, faction and ordinal among the guilds of that kind and faction
    Purpose Kind = Purpose::Leveling;
    bool Alliance = false;
    std::uint64_t Master = 0;            // the lowest guid of the members
    std::vector<std::uint64_t> Members;  // in guid order, the master first
};

struct Plan
{
    std::vector<Move> Moves;             // in guid order
    std::vector<NewGuild> Created;
    std::vector<std::uint64_t> Dissolved;    // empty leveling guilds: disband them
    std::vector<std::uint64_t> Mixed;        // guilds with both factions: left alone
};

// The raiding guild size actually used (clamped to 2 to 40).
std::uint32_t EffectiveRaidSize(Settings const& settings);

// The purpose a bot with no Want gets from its guid and the shares.
Purpose ShareOf(Settings const& settings, std::uint64_t guid);

// Whether the bot's guid falls in the PvpShare percent (a hash of the guid, independent of ShareOf).
bool InPvpShare(Settings const& settings, std::uint64_t guid);

// The kind a bot with no Want gets (see the top of this file).
Purpose KindOf(Settings const& settings, Bot const& bot);

// The purpose the bot wants: its Want if set, else KindOf.
Purpose WantOf(Settings const& settings, Bot const& bot);

// Whether a bot may join a raiding guild: level at least RaidMinLevel, and RaidQualified if set.
bool RaidQualified(Settings const& settings, Bot const& bot);

// The moves for the pool. `guilds` are the bot guilds known to the caller; a bot in a guild not listed is outside this plan and is not moved.
// Enabled off: an empty plan.
Plan PlanMoves(Settings const& settings, std::vector<Guild> const& guilds, std::vector<Bot> const& pool);

// The roster query. Candidates are the bots in a guild of Kind and the faction of self, not self. SameGuildOnly: only the guild of self.
struct MateQuery
{
    Purpose Kind = Purpose::Utility;
    std::uint32_t MinLevel = 60;
    bool OnlineOnly = true;
    bool SameGuildOnly = false;
    bool RaidReadyOnly = false;          // only the bots RaidQualified accepts
};

// Sorted by guid.
std::vector<std::uint64_t> GuildMates(Settings const& settings, std::vector<Guild> const& guilds, std::vector<Bot> const& pool, std::uint64_t self,
                                      MateQuery const& query);

// ---- Names ----

struct NameEntry
{
    NameStyle Style = NameStyle::Normal;
    std::uint32_t Weight = 1;            // 1 to 100
    std::string Name;
};

// The text of a names file. Comments (#) and blank lines are skipped; a bad row (field count, style, weight, a name that is not 2 to 24 letters, spaces,
// apostrophes or hyphens) or a duplicate (case ignored) is reported in warnings and skipped.
std::vector<NameEntry> ParseNames(std::string const& text, std::vector<std::string>& warnings);

// "lore:3 funny:0 normal:3 reallife:1" (spaces or commas between, styles in any order; a style not named keeps its weight in `weights`).
// False (and `weights` unchanged) on a bad style or number.
bool ParseStyleWeights(std::string const& text, StyleWeights& weights);

// The name key used for uniqueness: lower case.
std::string NameKey(std::string const& name);

// A name for a new guild of `kind`, weighted by row weight times NameStyles[kind], drawn with `seed`, never one whose NameKey is in `taken`.
// Nothing (nullopt) when the list is empty or nothing is left.
std::optional<std::string> PickName(Settings const& settings, std::vector<NameEntry> const& names, Purpose kind,
                                    std::function<bool(std::string const&)> const& taken, std::uint64_t seed);

// ---- Store rows (module store): guild/<guid> = "<kind> <A|H> <name>" ----

inline constexpr char const* kStorePrefix = "guild/";
char const* KindName(Purpose kind);                       // "leveling", "utility", "raiding", "pvp"
std::optional<Purpose> KindFromName(std::string const& text);
std::string StoreKeyFor(std::uint64_t guild);
bool GuildFromStoreKey(std::string const& key, std::uint64_t& guild);
std::string EncodeRecord(Guild const& guild, std::string const& name);
// False on a value that is not "<kind> <A|H> <name>"; the name may be empty (a guild created with the old name before names were stored is never written so).
bool DecodeRecord(std::string const& value, Purpose& kind, bool& alliance, std::string& name);

}  // namespace bot_guilds
