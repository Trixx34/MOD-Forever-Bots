# Bot guilds: kinds, names and memory

Status: module side done (2026-11-14). Off unless `GuildsEnabled = 1`; with only that key set, the bots behave as before (guid shares, "BotLvl A001" names, no memory).

The planner is `src/bot_guilds.h/.cpp` (pure, `tests/bot_guilds_test.cpp`, `tests/bot_guild_kinds_test.cpp`); the policy turn that sends the orders is
`src/bot_policy_guilds.cpp` (`tests/social_test.cpp`). The core runs the orders (`ModuleBotSocial.cpp` `ExecuteGuild`: create, invite, accept, leave).

## Kinds

| Kind | Who joins | Size | Leaves |
|---|---|---|---|
| leveling | bots below `GuildsLevelCap` | not capped | at the cap, or when its kind changes; an empty leveling guild is dropped |
| raiding | raid ready bots: `GuildsRaidMinLevel` and the readiness check (`doc/pre-raid-progression.md`, "Raid ready"; not wired yet, so the level alone counts) | `GuildsRaidSize` | when its kind changes |
| pvp | bots at `GuildsPvpMinLevel` or more (battlegrounds and duels, `doc/second-list-gaps.md`) | not capped | when its kind changes |
| utility | dungeons, professions, auction house; with `GuildsKindByLevel` the capped bots that are not raid ready yet (pre-raid) | not capped | when its kind changes |

Every guild has one faction. A bot's kind (`KindOf`), in this order:

1. **pvp** when `GuildsPvpShare` > 0, the bot is at `GuildsPvpMinLevel` or more and its guid falls in that percent (a hash of the guid: the same bot always
   answers the same, independent of the other shares).
2. `GuildsKindByLevel = 0` (default): the guid shares `GuildsLevelingShare` / `GuildsRaidingShare` / `GuildsUtilityShare` (60/20/20), as before.
   `GuildsKindByLevel = 1`: **raiding** when raid ready, else **leveling** below `GuildsLevelCap`, else **utility** (waits there until it is raid ready, then
   moves to a raiding guild).

A bot whose kind changes leaves its guild and joins one of its new kind on a later plan. A bot joins the lowest-guid guild of its kind and faction that has
room; when there is none, the lowest-guid bot of the new members becomes the master and creates one. The join rules (faction, mixed guilds left alone,
guilds the module does not know left alone, the raid cap, the create backoff) are unchanged.

## Names

`GuildsNameFile` (unset: none) names the names file, normally `data/bot_guilds.txt`: `guild|style|weight|name`, styles `lore`, `funny`, `normal`, `reallife`,
weight 1 to 100, name 2 to 24 letters, spaces, apostrophes and hyphens, unique ignoring case (`python3 tools/check_bot_guilds.py data/bot_guilds.txt`).
A bad row is skipped with a line in the log.

A new guild's name is drawn when its create is sent: each row weighs its weight times the weight of its style for the guild's kind:

| Kind | Key | Default (lore, funny, normal, reallife) |
|---|---|---|
| leveling | `GuildsNameStylesLeveling` | `lore:1 funny:1 normal:1 reallife:1` |
| utility | `GuildsNameStylesUtility` | `lore:1 funny:1 normal:1 reallife:1` |
| raiding | `GuildsNameStylesRaiding` | `lore:3 funny:1 normal:3 reallife:1` |
| pvp | `GuildsNameStylesPvp` | `lore:2 funny:0 normal:3 reallife:2` |

A style left out of the key keeps its default; 0 leaves the style out for that kind. A name in use is never drawn: names of the guilds the module made or
restored, of the creates in flight, and names the core refused ("a guild with that name exists"), all ignoring case. The module cannot see the names of
guilds it did not make (the snapshot has the guild id only), so a player guild with a listed name is found by the core's refusal: the name is marked taken
and the master draws another name on the next plan, without the refusal backoff. No file, no valid row, or every name of the kind's styles taken: the old
name ("BotLvl A001", "BotPvp H002", ...).

The name reaches the core in the existing create order: SDK `BotGuildCreate(bot, name)` (protocol 23, `GuildAction.name`), core `ExecuteGuild` op 3 uses
`request.Text` as the name. No core or SDK change.

## Memory

The core only tells the module a bot's guild id. The module keeps each guild's kind, faction and name in its store: the row `guild/<guild id>` =
`<kind> <A|H> <name>` (for example `pvp H Night Watch`), written when a create is confirmed and removed when a leveling guild is dropped. With
`GuildsRemember = 1` the rows are read at start, so the guilds of the last run keep their kind and their names stay taken. Off (default): as before, a
restarted module does not know its old guilds and leaves them and their members alone. The rows are written either way.

## Keys

| Key | Default | |
|---|---|---|
| `GuildsKindByLevel` | 0 | 1: the kind follows level and raid readiness; 0: the guid shares |
| `GuildsPvpShare` | 0 | percent of the bots at the pvp level that want a pvp guild (0 to 100) |
| `GuildsPvpMinLevel` | `BattlegroundMinLevel`, else 10 | the lowest level of a pvp guild member |
| `GuildsNameFile` | (empty) | the names file; empty: the old names |
| `GuildsNameStylesLeveling`, `...Utility`, `...Raiding`, `...Pvp` | see Names | style weights per kind |
| `GuildsRemember` | 0 | read the `guild/<id>` store rows at start |

Event lines: `guild_create: master=.. kind=.. name=..` (create sent), `guild_created: guild=.. master=.. kind=.. name=..` (confirmed),
`guild_restored: guild=.. kind=.. name=..`.

## Open

- The raid readiness predicate (`doc/pre-raid-progression.md`) is not wired into `Settings::RaidQualified` yet: raiding takes every bot at `GuildsRaidMinLevel`.
- The pvp guilds are not split by battleground bracket.
- A guild the core holds but whose store row is gone (or that was made before this change) stays unknown to the module after a restart.
