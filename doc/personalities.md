# Bot personalities

Status: built and unit-tested (`src/personality.*`, `tests/personality_test.cpp`, `presence_test.cpp`, `settings_test.cpp`). **Nothing here has run on the sim.**
Off by default (`Personality = 0`); with it off no bot's behaviour changes.

The first version (rest threshold, idle dwell, pre-pull pause, pack cap, pull mistakes, town AFK) was already in the module. This version adds the
presence session length, social tendency in greetings and guild looks, reaction jitter on interrupts and heal calls, and forgotten upkeep buffs, and
measures the cost.

## Why deterministic

A trait is `bot_random::Draw(RandomSeed, guid, salt, 0) % 101`: a pure function of the run's seed, the bot's guid and a fixed salt per trait. Nothing is stored,
so there is no new table, no state to lose, and a bot keeps its personality across restarts, relogins and log offs of the presence scheduler as long as
`RandomSeed` and the guid are the same. Tests can pin every value. Events that need fresh randomness (a jitter, a forgotten buff) use the same function with
a counter, so a run with the same seed and the same events repeats.

## Traits (0..100, 50 is the middle)

| Trait | Salt | High means |
|---|---|---|
| Pace | 0xA0 | short reaction delays (pull pause, interrupt and heal jitter), shorter idle dwell |
| Caution | 0xA1 | rests earlier, avoids the largest packs, rarely makes a mistake or forgets a buff |
| Sociability | 0xA2 | more town AFK, higher greeting chance, looks for a guild invite sooner |
| Session | 0xA3 | longer presence sessions and longer town AFK stays |
| Patience | 0xA4 | longer idle dwell |

## What each trait changes

Every effect has its own strength key; 0 switches that effect off and the base value is returned unchanged.

| Decision | Rule | Range | Key |
|---|---|---|---|
| Rest below health | base +- Caution * 8 points | clamp 70..98 | `PersonalityRestStrength` |
| Idle dwell before a walk | base * (1 +- 0.6 * (Patience, inverse Pace)) | 0.4..1.6 of base | `PersonalityIdleStrength` |
| Pause before a solo pull | draw in 0..Max * (100 - Pace) / 100 | 0..10000 ms | `PersonalityReactionMaxMs` |
| Largest pack pulled | one less than the shared cap for wary bots (Caution high) | at most 3 skips in a row | `PersonalityCautionStrength` |
| Pull a mob other than the best | chance Max * (100 - Caution) / 100 percent | 0..30 percent | `PersonalityMistakeMaxPct` |
| Town AFK chance and stay | chance by Sociability, stay by Session, up to +-100 percent | chance at most 100 | `PersonalitySocialStrength` |
| **Greeting chance** (MeetGreet, CityGreet) | the same formula as the AFK chance | at most 100 | `PersonalitySocialStrength` |
| **Guild invite look** (GuildJoin, once a minute by default) | interval * (1 - 0.5 * Sociability centred) | 0.5..1.5 of 60 s | `PersonalitySocialStrength` |
| **Presence session length** | drawn session * (1 +- 0.6 * Session centred), applied to the session only, not the absence | 0.4..1.6, at least 1 minute | `PersonalitySessionStrength` |
| **Interrupt reaction** | a delay before the first interrupt row on a new cast, 0..JitterMax * (100 - Pace) / 100 | 0..2000 ms | `PersonalityJitterMaxMs` |
| **Heal call reaction** | the same delay before the first ordinary heal on a member | 0..2000 ms | `PersonalityJitterMaxMs` |
| **Forgotten buff** | one roll per upkeep buff when it comes due: chance ForgetBuffPct * (100 - Caution) / 100 percent; a forgotten buff waits 30..120 s | at most 30 percent | `PersonalityForgetBuffPct` |

Bold rows are new in this version.

### Where the presence session length is applied

`presence::Online` carries `SessionPct` (100 = unchanged). The module's pump fills it from `BotPolicy::SessionPctOf(guid)`, which is 100 when Personality is
off. The scheduler scales every session it draws for that bot (the partly-over first session and each fresh one). The hour-of-day ceiling, the minimum and
the cap still decide how many bots are around, so personalities change who leaves when, not the crowd size.

### Chat

The chat chance (`AmbientChat`) lives in the core-facing main code and has its own chance key; it is not scaled yet (see open questions). Greetings and the
guild look are scaled. Group invites: the party planner's choice of who forms parties is unchanged.

## Imperfect play: where it never applies

* **Interrupt jitter** is not taken when the cast would end before the kick can land after the delay (`delay + kInterruptMinRemainingMs >= cast_remaining_ms`
  gives no delay), so jitter never turns a possible interrupt into a miss. When the core reports no remaining time (0) the delay is capped by the 2 s key limit.
* **Heal jitter** applies only to the ordinary pick: not an emergency (below 35 percent), not the tank, not a shield or Renew, not a member under 50 percent,
  and not the heal-mix proactive pick. A member that falls under 50 percent during the wait is healed at once.
* **Forgotten buffs** are upkeep buffs only (the rows marked `buff`), never a pet summon, never in a fight, and only one roll per due buff until it is cast;
  the buff is delayed 30 to 120 s, never dropped.
* **Dungeons and raids**: the jitter and forgotten buffs are not applied when a route is active, the map has a dungeon route or the group is larger than a
  party, unless `PersonalityInstances = 1`. The pull rules (pause, pack cap, mistake) only ever act on a solo bot with no route and out of combat.
  Rest threshold, idle dwell, greeting and guild look are not in any fight.
* Survival rules, level and elite guards, rotations and raid rules are never touched.

## Keys (`Module.forever-bots.`)

| Key | Default | |
|---|---|---|
| `Personality` | 0 | switch |
| `PersonalityRestStrength`, `PersonalityIdleStrength`, `PersonalityCautionStrength`, `PersonalitySocialStrength`, `PersonalitySessionStrength` | 50 | 0..100 |
| `PersonalityReactionMaxMs` | 3000 | 0..10000 |
| `PersonalityJitterMaxMs` | 400 | 0..2000 |
| `PersonalityMistakeMaxPct`, `PersonalityForgetBuffPct` | 5, 3 | 0..30 |
| `PersonalityInstances` | 0 | allow the jitter and forgotten buffs in dungeons and raids |

All are in `config/modules.d/forever-bots.conf.example` with comments and in the README table.

## Cost per bot per tick

Everything is a few integer mixes: `TraitsFor` is 5 hash draws (about 20 SplitMix64 rounds), each rule is a multiply and a clamp. The policy asks for the traits at the
site it needs them (rest at the first sight of a bot, idle dwell when idle, the pull, greeting, an interrupt or heal event, a buff coming due), so most ticks pay
for none or one. With Personality off the cost is one boolean test per site.

`personality_test` measures it (`CostPerBotTickIsSmall`): 800 bots, 200 ticks, every rule asked once with the traits drawn once is an upper bound on a tick, and the
common tick (traits plus one rule). The numbers are printed when the test runs and the test fails above 10 percent and 5 percent of the 17 us decide time.
Measured on the build host (release build, `nice` 19, other jobs running): every rule on one bot, traits once, 161 ns per bot tick (0.95 percent of 17 us); the common idle tick 43 ns (0.25 percent). With the traits drawn at each of three sites the all-rules figure stays under 2 percent.

## Tests

`personality_test` (off changes nothing, each new key's own off switch, traits stable and spread, every range, jitter and forget bounds, the instance gate, the cost),
`presence_test` (`SessionPctScalesTheSession`: bots at 50 percent leave in half the time, the default is the old behaviour), `settings_test` (the new keys and their clamps).

## Open questions

* Ambient chat frequency by sociability: the chat chance is not scaled yet; it can use `GreetChancePct` in a second step.
* Group invites by sociability: bots that form parties are cut by `PartyPercent` from the guid; a sociability weight could replace the plain draw.
* Whether the cautious bots should also leave a dangerous pack mid-fight earlier (a flee threshold): left alone because it is a survival rule.
* Absence length by trait (a bot with a long session could also have a short absence): left out so the crowd curve stays predictable.
