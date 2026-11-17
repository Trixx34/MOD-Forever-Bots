# Death fixes (2026-11-17)

Prod, 800 bots, 2 h of log (11-17): 10,727 `died` events, 5,360 an hour. All keys below are default off; each is read in `bot_settings.cpp`, applied in the policy, and covered by `tests/death_fixes_test.cpp`.

## What the log showed

- `death_action_refused ... action=resurrect_accept status=out of range`: 102,020 in 2 h on 580 bots, **no resurrect offer was ever pending** (0 `resurrect_accept:` lines). The core fills `last_action_spell` for `kCastSpell` only (`ModuleMgr.cpp`: `result.Spell = ... kCastSpell ? action.spell_id : 0`), so the answer to the spirit-healer request (`kResurrect`, spell_id 2) never matched `kSpiritHealerSpell` in `bot_policy_result.cpp` and fell into the generic branch, labelled `resurrect_accept`. The real cause is the ghost asking a spirit healer from its corpse, far from the graveyard (the core looks within about 40 yards), every 3 s: out of range, again and again. 99,565 of the refusal gaps are 5 to 15 s.
- Pulls: 32% start below 90% health, 11% below 75% (the fixed rest mark is 75 with no hysteresis); 5.8% of mana-user pulls start below 20% mana. 42% of deaths follow a pull made below 75% health (15% below 50%), against 11% of all pulls below 75%.
- 61% of deaths are to the creature the bot pulled last; half come 15 to 60 s after that pull, 28% within 15 s.
- Several attackers: 61% of deaths show two or more distinct sources in the last five hits (4,488 two, 1,714 three, 432 four or five).
- Repeat killers by entry (deaths, deaths per 100 pulls of the entry): 5953 Razor Hill Grunt 454 deaths of one level 3 bot, 252763 Al'Aketh Spiritcaller 442 (never pulled: it attacks), 252762 249, 3273 Kolkar Stormer 246, 1161 Stonesplinter Trogg 245, 1162 Stonesplinter Scout 221, 1166 Stonesplinter Seer 203 (106 per 100 pulls), 3242 Zhevra Runner 203, 3255 Sunscale Screecher 198, 256092 186. Entries with 100 or more deaths per 100 pulls (1166, 2018, 3245, 3269, 2529, 3416, 1418, 5953) kill about every pull.

## Keys

| key | default | effect |
|---|---|---|
| `SoloPullHpPct` | 0 | 1 to 95: a solo bot out of combat below this health percent starts no fight and rests until this plus 10 (hysteresis; at most 150 s, `kRecoverHoldMs`); event `pull_wait reason=hp`. 0 keeps the fixed 75 mark without hysteresis. |
| `PullMaxGroup` | 0 | 1 to 3: a solo bot does not pull a creature with more than this many hostiles (itself included) within 10 yards. 1 = singles only. 0 = built-in 3. |
| `PullMaxLevelAbove` | 2 | 0 to 2: a solo bot pulls no creature more than this many levels above it. 2 is the built-in rule. |
| `ResurrectBackoff` | 0 | The spirit-healer answer is recognised (a `kResurrect` answer with no offer pending): a refusal puts the ghost back on the corpse run / reclaim for two minutes (the existing `kSpiritHealerRefusedMs`) and logs `death_action_refused action=spirit_healer`; a ghost that wants the healer but stands far from the graveyard goes through the ghost fallback (teleport to the graveyard, then ask) with `ghost_fallback reason=spirit_healer_far`. A refused real offer (`resurrect_accept`) waits 4, 8, 16, 32, 60 s (`resurrect_backoff`) and the corpse run goes on meanwhile. |

Existing keys that address the same deaths and are still off on prod: `SoloPullManaPct` (mana gate), `KillerAvoidMs` (do not re-pull the entry that killed the bot).

## Values to enable on the sim

`SoloPullHpPct = 85`, `PullMaxGroup = 2`, `PullMaxLevelAbove = 1`, `ResurrectBackoff = 1`, with `SoloPullManaPct = 50` and `KillerAvoidMs = 1800000` (existing keys).

## Not done: grind bands

`data/grind_spots.txt` is a spot list by creature level band; the death lines carry no creature level and the world database of this fork keeps levels in ContentTuning, so no band could be shown to be too high. `PullMaxLevelAbove` is the lever instead. To tighten bands by data, add the pulled target's level to the `pull` event and the killer's level to `died`, then count deaths per spot.
