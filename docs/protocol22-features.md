# Protocol 22 features: keys, defaults, risks, rollout

All four are off by default and read from the module config (`Module.forever-bots.<key>`, `config/modules.d/forever-bots.conf`; parsed in `src/bot_settings.cpp`). They need a core that sends protocol 22 snapshots (guild_guid, item_level, in_capital; the invite-declined event). None of them is enabled by this note.

| Feature | Keys (default) | What it does | Risk |
| --- | --- | --- | --- |
| PathJitter | `PathJitter` (0), `PathJitterYards` (1.0, range 0..5) | Offsets an out-of-combat walking target by up to N yards in a random direction. Not in dungeons, not within 2 yards of an unwalkable point, never across a required route waypoint. | Highest. It touches every walk. A bad offset can put a bot against a wall or off a thin path and show up as stuck episodes or longer quest and grind walks. Needs the stuck and refused-action counters compared with a control block. |
| QuestInvite | `QuestInvite` (0) | An idle bot and a human on the same quest objective, human within 30 yards: the bot invites the human to its group. At most once per human per 30 min, never within 2 h of a decline. Limits are code constants, not keys. | High for player experience. Real players get group invites from bots, and a bot in a group changes its behaviour (follows, shares the party's route). Invite spam is the main complaint risk. Needs the core's invite-declined event to be working. |
| GuildMilestone | `GuildMilestone` (0), `GuildMilestoneChancePct` (30, 0..100), `GuildMilestoneFile` (`data/guild_milestones.txt`) | When a player member of a guild with bot members levels up, one random bot of that guild says a line in guild chat (chance above, one answer per event). | Low. Chat only. Risks are repetitive lines (small line file) and a bot speaking in a guild where the player does not expect it. |
| CityInspect | `CityInspect` (0), `CityInspectChancePct` (5, 0..100), `CityInspectZoneMax` (2, 0..100 emotes a minute per map) | An idle bot in a capital that sees a player within 20 yards wearing a higher-level item than its own salutes, applauds or cheers at the player. One bot per player per 5 min. | Lowest. Emotes only. Risk is crowded capitals (bounded by ZoneMax) and wrong item-level data making emotes at the wrong players. |

## Proposed rollout (one block each, 4 h of levelling, compare with an all-off control block; see docs/117-data-plan.md phase 3)

1. CityInspect. Emote only, no movement or group effect. Checks that the new snapshot fields (item_level, in_capital) arrive right.
2. GuildMilestone. Chat only. Checks guild_guid and the guild member list; start with the default 30 percent.
3. QuestInvite. Do it only with a human tester online, and watch the invite and decline counts and group sizes. If the decline event does not arrive, keep it off.
4. PathJitter. Last, because it changes every walk. First at 1.0 yard; compare deaths, stuck episodes, level-ups per hour and refused actions with the control. Do not raise the yards before that block is clean.

Roll back by setting the key to 0 and restarting the module; no data is changed by any of the four.
