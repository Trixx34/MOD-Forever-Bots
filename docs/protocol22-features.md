# Protocol 22 features: keys, defaults, risks, rollout

All four are off by default and read from the module config (`Module.forever-bots.<key>`, `config/modules.d/forever-bots.conf`; parsed in `src/bot_settings.cpp`). They need a core that sends protocol 22 snapshots (guild_guid, item_level, in_capital; the invite-declined event). None of them is enabled by this note.

| Feature | Keys (default) | What it does | Risk |
| --- | --- | --- | --- |
| PathJitter | `PathJitter` (0), `PathJitterYards` (1.0, range 0..5) | Offsets an out-of-combat walking target by up to N yards in a random direction. Not in dungeons, not within 2 yards of an unwalkable point, never across a required route waypoint. | Highest. It touches every walk. A bad offset can put a bot against a wall or off a thin path and show up as stuck episodes or longer quest and grind walks. Needs the stuck and refused-action counters compared with a control block. |
| QuestInvite | `QuestInvite` (0) | An idle bot and a human on the same quest objective, human within 30 yards: the bot invites the human to its group. At most once per human per 30 min, never within 2 h of a decline. Limits are code constants, not keys. | High for player experience. Real players get group invites from bots, and a bot in a group changes its behaviour (follows, shares the party's route). Invite spam is the main complaint risk. Needs the core's invite-declined event to be working. |
| GuildMilestone | `GuildMilestone` (0), `GuildMilestoneChancePct` (30, 0..100), `GuildMilestoneFile` (`data/guild_milestones.txt`) | When a player member of a guild with bot members levels up, one random bot of that guild says a line in guild chat (chance above, one answer per event). | Low. Chat only. Risks are repetitive lines (small line file) and a bot speaking in a guild where the player does not expect it. |
| CityInspect | `CityInspect` (0), `CityInspectChancePct` (5, 0..100), `CityInspectZoneMax` (2, 0..100 emotes a minute per map) | An idle bot in a capital that sees a player within 20 yards wearing a higher-level item than its own salutes, applauds or cheers at the player. One bot per player per 5 min. | Lowest. Emotes only. Risk is crowded capitals (bounded by ZoneMax) and wrong item-level data making emotes at the wrong players. |

## What the code checks showed (2026-11-12)

- Core side present: `ModuleBotSnapshot.cpp` fills `in_capital` (zone has the Trade-channel flag), `guild_guid` (`GetGuildId()`) and `item_level` (the bot's and each nearby player's `GetAverageItemLevel()`), plus `shared_objective` for quest-mates; `GuildScript.OnMemberLevel` fires from `ScriptMgr` on a guild member's level-up; `GroupScript.OnInviteDeclined` fires from `GroupHandler.cpp` on a decline. All three events reach the module only when the module declares the hook (module.json lists all seven; main.cpp subscribes at start).
- Module side present and wired: settings parsed in `bot_settings.cpp`, decline list handed to the pump thread (`QuestDeclines`), unit tests for each feature (`guild_milestone_test`, `city_inspect_test`, `quest_invite_test`). Nothing was missing in code; no code change was needed for the rollout.
- A new module.json hook list needs `module approve forever-bots` on the realm that loads it. Prod has `Modules.AutoApproveLocal = 0`; check the sim's setting before assuming the module loads.

## Safe on the sim, and the config lines

All four stay off in the committed example. For a block, put the lines in the sim's `config/modules.d/forever-bots.conf` (never prod), restart the module, and run the 4 h block against an all-off control.

| Order | Feature | Safe on the sim now | Lines |
| --- | --- | --- | --- |
| 1 | CityInspect | Yes, any time; emotes only | `Module.forever-bots.CityInspect = 1` and `Module.forever-bots.CityInspectChancePct = 5` and `Module.forever-bots.CityInspectZoneMax = 2` |
| 2 | GuildMilestone | Yes, if a player character sits in a guild that has bots; chat only | `Module.forever-bots.GuildMilestone = 1` and `Module.forever-bots.GuildMilestoneChancePct = 30` |
| 3 | QuestInvite | Only with a human tester online who is on a bot's quest objective, and told to decline once; it sends real group invites | `Module.forever-bots.QuestInvite = 1` |
| 4 | PathJitter | Only after 1 to 3 ran clean; touches every walk | `Module.forever-bots.PathJitter = 1` and `Module.forever-bots.PathJitterYards = 1.0` |

Per feature, what to check in the log and what stops the block:

1. CityInspect. Count the emote lines; counts per hour must not exceed `CityInspectZoneMax` per map. Stop on any bot that emotes outside a capital (the flag is the Trade-channel area flag, so an outpost zone with the flag also counts; that is acceptable).
2. GuildMilestone. A level-up of the tester inside a guild with a bot gives exactly one guild line. Stop on two bots answering the same event or a line in the wrong channel.
3. QuestInvite. The tester stands near a bot with the same quest objective (within 30 yards), sees one invite, declines it, and gets no second one for 2 h (the decline must show in the module log as a decline). If no decline arrives, switch it off at once; the 30 minute cap per human still holds but the 2 h pause does not.
4. PathJitter. Yards 1.0 only. Compare deaths, stuck episodes, level-ups per hour and refused actions with the control; no raise before the block is clean.

## Proposed rollout (one block each, 4 h of levelling, compare with an all-off control block; see docs/117-data-plan.md phase 3)

1. CityInspect. Emote only, no movement or group effect. Checks that the new snapshot fields (item_level, in_capital) arrive right.
2. GuildMilestone. Chat only. Checks guild_guid and the guild member list; start with the default 30 percent.
3. QuestInvite. Do it only with a human tester online, and watch the invite and decline counts and group sizes. If the decline event does not arrive, keep it off.
4. PathJitter. Last, because it changes every walk. First at 1.0 yard; compare deaths, stuck episodes, level-ups per hour and refused actions with the control. Do not raise the yards before that block is clean.

Roll back by setting the key to 0 and restarting the module; no data is changed by any of the four.
