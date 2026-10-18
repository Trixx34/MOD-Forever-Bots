# Bot presence: bots that come and go like players

Status: the scheduler (`src/bot_presence.*`), its settings, the roster bookkeeping and the policy's idle check are built and unit-tested
(`tests/presence_test.cpp`, `roster_test.cpp`, `settings_test.cpp`, `policy_test.cpp`). The core side is one gated change. **Nothing here has run on the sim.**

## What it does

Each ordinary bot plays a session of a random length, logs off, stays away for a random time and comes back, so the number of bots in the world
changes over the day like the number of players does. Off by default.

- **Sessions and absences**: a session lasts 60 to 240 minutes and an absence 30 to 240 minutes by default (`PresenceOnlineMinMin`, `PresenceOnlineMaxMin`,
  `PresenceOfflineMinMin`, `PresenceOfflineMaxMin`). Bots already online when the module starts get a session that is already partly over, so they do not
  all leave at once.
- **Time of day**: `PresenceDayPercent` is 24 comma-separated numbers (midnight first), the most of the population that may be around in that hour,
  with linear steps between the hours. It is a **ceiling**: the random sessions make the real number fluctuate below it, and when it falls the sessions
  due soonest end early to get under it. The default has the evening (19:00 to 22:00) at 90 to 100 percent and 04:00 to 06:00 at 10 percent.
  Local time of the machine the module runs on.
- **Minimum and maximum**: `PresenceMinOnline` ordinary bots are never taken offline (the soonest-due absences are cut short to keep it);
  `PresenceMaxOnline` caps how many are around at once (0 = no cap; the cap wins over the minimum). The overall cap on bot accounts is still `MaxBots`.
- **Who takes part**: only ordinary bots. A bot in a party, an alt, or a bot that is busy is never logged off and does not count toward the
  minimum. Busy means: in a fight, dead or a ghost, has a party invite, holds a chat order (`stay`, `follow`), is walking to a vendor or on a supply
  trip, has a refused quest hand-in waiting, or has a trade in progress. A bot that is due but busy is asked again 30 seconds later.
- **Pace**: at most 5 log offs and log ons per 5-second step (`MaxChangesPerTick`, fixed), so a crowd does not come and go on the same second.

## How it works

The pump thread (which has the snapshots) lists the ordinary online bots and whether each is idle every 2 seconds. A timer on the client thread
(every 5 seconds) runs the scheduler over that list, adds the trade check, and acts:

- **Log off** = `ReleaseControl(bot)`. The roster marks the bot away: it still counts toward `MaxBots`, so the ramp does not spawn a replacement.
  The core logs the bot out when `Bot.Presence.LogoutOnRelease = 1` (core `worldserver.conf`, default 0). Alts always log out when released.
- **Log on** = `RequestControl(guid)`, the same call that takes saved bots back at a restart. The bot keeps its place in the world (the spread
  at login only moves bots that are still at their creation spot). A refusal is retried after 30 seconds; "character not found" forgets the bot and deletes
  its store row. A bot sent back that has not shown up in 5 minutes counts as away again.
- The bot rows in the module store are untouched, so a restart of the module takes every bot back, away or not, and the schedule starts over.

## Settings

| Key (`Module.forever-bots.`) | Default | |
|---|---|---|
| `Presence` | 0 | switch |
| `PresenceOnlineMinMin`, `PresenceOnlineMaxMin` | 60, 240 | session length, minutes |
| `PresenceOfflineMinMin`, `PresenceOfflineMaxMin` | 30, 240 | absence length, minutes |
| `PresenceMinOnline`, `PresenceMaxOnline` | 0, 0 | floor and cap on ordinary bots online |
| `PresenceDayPercent` | evening-heavy curve | 24 numbers, 0 to 100 |

Core: `Bot.Presence.LogoutOnRelease = 1`. Not used with a scenario run (the scenario needs its fixed roster).

## What to check on the sim

- `forever-bots: presence: N log off, M back; A away, B online` lines every few minutes; the core log shows `bot ... logged out (released by its module)` and
  `is in the world` for the returns.
- A returning bot is where it logged off, not at the start of its race.
- The online count follows the curve (set a short curve to see it: `PresenceDayPercent` at 100 for the current hour, 20 for the next).
- No `could not bring bot ... back` lines. A bot whose login keeps failing is retried every 30 seconds.
- Memory: fewer bots online is less memory; the policy keeps a small state for each bot it has seen.

## Decisions made without asking (change them if wrong)

- The day curve is a ceiling, not a target: at the peak the online share is what the random sessions give (about half with the default lengths),
  not 100 percent. A target would make absences pointless at the peak.
- The trigger for a log off is `ReleaseControl`, so the core change is gated by a config key instead of adding a new action to the SDK.
