# PvP track: best PvP gear and battlegrounds

Product point (owner, 2026-11-13): "Part of the bots will pursue best pvp gear as well. They will also run battlegrounds etc." Next to the PvE path (leveling, pre-raid
BiS and attunements, then raids; `doc/pre-raid-progression.md`) a share of the level 60 bots follow a PvP track: wear the best PvP gear, queue for and play
battlegrounds, climb the honor ranks. This page is the design, the inventory of what the core and the module can do for it today, the gaps and a phased plan. Sim
first; prod only with the owner's go-ahead. Item selection is by rules (flags, rank, source, class fit, stats), never by item entry.

Status: phase 1 (this page and the wishlists `data/pvp/`) is done; nothing in the module reads the lists or plays a battleground yet.

## 1. Which bots take the PvP track

A level 60 bot belongs to one track, chosen once and kept (stored with the bot in the module store, so a restart keeps it). Proposed settings (none exist yet; the first
slice adds none):

| Key | Default | Meaning |
| --- | --- | --- |
| `PvpTrackShare` | 0 | percent of level 60 bots on the PvP track (0 = nobody; the sim starts at 25) |
| `PvpTrackRoles` | `dps,healer` | roles that may take the PvP track; a tank is never drafted (a battleground has no tank slot, and the raid roster needs the tanks) |
| `PvpTrackMinLevel` | 60 | the track starts at this level; below it the bot levels as before |

The pick is a stable hash of the bot's GUID against `PvpTrackShare`, so a bot does not flip between runs; a bot already assigned a raid roster slot (ScenarioRoster, a
dungeon or raid group) stays PvE for that run. The share is a per-class cap too: no class goes above `PvpTrackShare` + 10 points, so the PvP side is not all casters.
Bots keep their track after a battleground; a PvP bot that is invited to a PvE group by a player may go (the normal group logic), the track only decides what it does
when idle.

## 2. Stages

1. **Level 60 entry gear.** Same as the PvE track's first stage: the dungeon/quest/crafted pre-raid tier (`data/prebis/`), so the bot does not enter a battleground
   in greens. Reputation and honor vendor pieces (below) that need no rank are part of this stage already.
2. **Battleground grind.** The bot queues for Warsong Gulch, Arathi Basin and Alterac Valley in turn (section 5) and earns marks and rank points from every battle.
3. **Rank climb.** Rank points raise the honor rank R1..R14 (section 3). Each rank band unlocks a gear band (section 4); when a bot reaches a band's rank, the module
   grants the band's wishlist (best piece per slot of that band) and the core's equip service wears what is better.
4. **Reputation gear.** Marks of Honor and the battleground reputations (Frostwolf/Stormpike, Warsong/Silverwing, Defilers/League of Arathor) buy the no-rank pieces
   at the supply officers.
5. **Top of the track:** R13/R14 sets and weapons (Field Marshal / Warlord, Grand Marshal / High Warlord). The track then keeps queueing; no further gear exists.

## 3. Honor ranks in this core (what exists)

The core is not a classic honor system. It has a "Classic 1.60" layer (`src/server/game/Battlegrounds/Battleground.cpp`, `Reputation/ReputationMgr.cpp`):

- **Rank = reputation track of faction 2800 "PvP Rank Points"** (renown currency 3473). Rank 1 needs 750 points, rank 2 900, ... rank 14 3000, 24750 in all (the
  curve `RenownThresholdCurveID` 103650).
- **Points come from battleground results only**, when a battle ends: `Classic.PvPRank.PointsBattlegroundWin` 250, `...Loss` 100 (worldserver.conf, `Rate.Reputation.Gain`
  applies). Kills pay no rank points. By the arithmetic rank 14 is about 100 wins, or 140 battles at an even win rate; at roughly 20 minutes a battle that is 45 to
  50 hours of play per bot.
- **Marks of Honor** are cast on every player at the end: `ClassicMarks` for Alterac Valley (30, item 20560), Warsong Gulch (489, 20558), Arathi Basin (529, 20559); a
  loser gets the mark only after 10 minutes of battle. Eye of the Storm (566) is on the id list of the module but has no classic mark and is TBC, not for the level 60 track.
- **Rank is not enforced as an equip or purchase gate.** `npc_vendor` of the honor quartermasters (Armor, Weapons, Accessories Quartermaster) and the battleground
  supply officers has no extended cost and no player condition on any row; the `item_sparse.RequiredPVPRank` hotfix exists only on 500 Forever items (ranks 9..18 =
  honor rank 5..14: the raw number is the honor rank + 4) and the core C++ never reads it. The classic named sets (Knight-Captain's, Blood Guard's, Marshal's, ...) carry no
  rank data at all, only the rank in the name. So whoever grants or buys the gear enforces the rank: the module (section 4).
- The reputation of the battleground factions is the usual reputation system (not read by the module yet).

## 4. Gear wishlists (`data/pvp/`)

`tools/gen_pvp.py` (builds on `tools/gen_prebis.py`, copied unchanged from `feature/prebis-gear`; the two branches add the same file, so the merge is clean as long as the
generator stays as is) writes per class three files in the format of `data/prebis/<classId>.txt` (one item entry per line, a comment line above each entry):

- `<classId>-alliance.txt` and `<classId>-horde.txt`: the ladder for one faction, authoritative;
- `<classId>.txt`: both factions in one file (every non-weapon entry of both, the weapons of the higher scoring side only), the file a `ScenarioGearFile` folder lookup finds.

Comment line: `# slot=chest ilvl=71 quality=4 source=rank rank=R12 side=alliance Marshal's Leather Tunic`.

**What is a PvP piece (rules on the data):**

- *Rank gated:* `item_sparse.RequiredPVPRank > 0` (honor rank = raw - 4), or a rank title at the start of the name (`Knight-Captain's`, `Blood Guard's`, `Marshal's`,
  `General's`, `High Warlord's`, ... the table `NAME_RANK` in the generator; the classic items have no hotfix row: 405 of the 785 candidates on the sim carry the rank only in the name). Source `rank`.
- *Battleground reputation vendor:* sold by a creature whose subtitle is `... Supply Officer` (Defilers, League of Arathor, Frostwolf, Stormpike, Warsong, Silverwing).
  Source `bg-rep`, rank R0.
- *Honor quartermaster:* sold by a creature whose subtitle contains `Quartermaster`. Source `honor-vendor`, R0 unless the name says a rank.
- Side: from the rank title (Alliance titles vs Horde titles) or from the officer's faction (Defilers, Frostwolf, Warsong = Horde; League of Arathor, Stormpike,
  Silverwing = Alliance); every other piece is `both`. `AllowableRace` is -1 on all 500 hotfix items, so the data has no race gate; the sets are separated by name only.

**Selection:** quality 3-4, required level at most 60, item level 58-90, no test or placeholder items, no profession-gated items, usable by the class (armour type,
weapon skill, class mask). Best by a PvP stat profile: the `gen_prebis` profile of the class's PvP role (warrior arms/fury, ret paladin, hunter, rogue, priest/shaman/druid
healer, mage and warlock caster; `--profile` changes it) with Stamina raised to at least 1.0 and a Resilience weight of 1.0 (Forever's PvP variants such as the
Knight-Captain's and Blood Guard's mail pieces carry more Stamina, and the score takes that into account). A warrior takes a two-hander or two one-handers and a paladin
a two-hander, not the shield sets of the PvE lists.

**Bands:** per slot the generator keeps the best piece usable at each rank cap `0, 8, 10, 12, 14` (`--bands`) and lists them best rank first, so the file is a ladder: the
first entry of a slot is the top of the track, the last the no-rank piece. The ladder keeps the two-hander/one-hander kind of the top band in the weapon slots.

**Sim result (2026-11-13, `forever_test_world`)**: 9 classes, 27 files; per faction 11-30 entries (a Horde paladin has only 11: few paladin pieces carry Horde titles). Top R13/R14
pieces for every class: Field Marshal / Warlord sets (item level 74), Grand Marshal / High Warlord weapons (80).

**Not directly grantable yet.** A ladder holds 25-30 entries per faction and the 16-slot backpack cannot take them at once (the PvE lists have 17-19 for that reason), and
`ScenarioGearFile` has no notion of a faction or a rank. The module's loader has to read the side file of the bot's faction, drop entries above the bot's rank, keep the
best band per slot and grant that (section 8, phase 1b). Pieces that exist for several rank bands are listed because a bot gets the next band as it climbs.

## 5. Battlegrounds: roles, queue and play

Level 60 brackets: Warsong Gulch (BattlemasterList 2, 10 per side), Arathi Basin (3, 15 per side), Alterac Valley (1, 40 per side). Eye of the Storm (7) is outside the level
60 classic content. The first slice plays WSG; AB next; AV last (size, distance, many NPC objectives).

**Queue.** An idle PvP-track bot queues for the battleground that has been waiting longest in the module's own tally (round robin, weighted to the one the real players
are in, below); one battleground at a time; leaves a queue that has not popped in `BattlegroundMaxWaitMin`; cooldown between queues. The bots' decision module exists
(`src/battlegrounds.*`, `Decide`): idle, not in combat, level >= `BattlegroundMinLevel`, cooldown over. It needs a track gate, a faction-neutral way to pop a queue (all bots
queue solo and the core fills the teams), and the "accept the invitation" step the core lacks (section 6).

**Roles in a battleground** (decided when the bot enters, from class and spec profile; one of):

- *Flag carrier* (WSG): a bot with the best survival (plate or high Stamina, or a druid/hunter with a cheap escape) picks up the enemy flag and runs home; the others
  escort or hunt the enemy carrier. Needs the carrier state (below).
- *Escort and defend* (WSG, AB): healers and one or two damage bots stay with the carrier, the rest guard the own flag room.
- *Node capture* (AB): groups of 3-5 bots assigned a node (stables, blacksmith, farm, gold mine, lumber mill), capture it by `kUseObject` of the banner, defend it, call
  the next node when it is held.
- *Hunter-killer* (all): damage bots that attack the nearest enemy player who is not in a node or carrier fight; reuse the combat module's target choice.
- *Healer*: follow the carrier or the nearest group, heal by the existing healing logic.
- *AV:* "go north", the bots walk the road to the next tower or the enemy general; capture towers and graveyards with `kUseObject`; reinforcement by kills. Phase 3.

**Death and resurrection.** A bot that dies releases the spirit (`kReleaseSpirit`, used by the module in the open world), is resurrected at a battleground graveyard by the core's own
rules (to be verified in a run), and rejoins its role. The core's AFK rule may remove a bot that stands still; the module has to keep it moving.

## 6. What the core and the module can do today (precise)

Checked in `core-traps` (`feature/...`, head f18bb0a4be, protocol 29), the bots repo (origin/test, 2026-11-13) and the SDK (`sdk-p28`):

**Core: exists**

- `BattlegroundAction` (ActionRequest field 24, `ModuleBotSocial::ExecuteBattleground`, needs `Bot.Social.Enabled = 1`, default 0): `kQueue` (BattlemasterList id; the
  real `HandleBattlemasterJoinOpcode`, so level bracket, faction, deserter, "already in a queue" checks apply; solo only, a bot in a group is refused), `kLeaveQueue`,
  `kLeave` (a bot in a battleground leaves it; refused in combat). One request, one result: success or a reason string.
- The battleground code is the full retail-derived one (BattlegroundMgr/Queue, scripts of Warsong Gulch, Arathi Basin, Alterac Valley, Eye of the Storm and the rest under
  `src/server/scripts/Battlegrounds/`); the classic layer gives marks of honor and rank points at the end of a battle (section 3).
- `kMoveTo`, `kAttackTarget`, `kCastSpell`, `kFollow`, `kUseObject` (a game object by entry, as a click: a door, lever, altar, chest), `kVendor`, `kGrant`, `kReleaseSpirit`,
  `kMount`, `kPetCommand`, `kGossip`, `BuyItemForBot`; they work on any map the bot is on, with a battleground not excluded explicitly.
- Snapshot: bot position, `map_id`, health, level, group role, in-combat, nearby units (guid, position, hostile/friendly), corpse and life state. The module treats a
  battleground map id (30, 489, 529, 566; `social::IsBattlegroundMap`) as "inside".

**Core: does not exist (gaps)**

1. **Accepting the invitation.** The core's own header says: "A queued bot is not yet sent to the battleground by anything: accepting the invitation is not part of these
   actions." After the queue pops, nobody answers `CMSG_BATTLEFIELD_PORT`, so the invite expires. Without this action the track plays no battle. Needed: a
   `BattlegroundOp kAccept` (and `kDecline`), running `HandleBattleFieldPortOpcode` on the bot's session, plus the snapshot or an event that says "an invite is pending for
   battleground X (expires in N s)". Alternative: a core option `Bot.Battleground.AutoAccept` that accepts for controlled bots; simpler, less control.
2. **Battleground state in the snapshot or events.** Nothing tells the module which battleground it is in, its team (the bot's faction is known, but a battleground also has
   the team), the score, the flag state (who carries which flag), the node ownership (AB), the match timer or the winner. The module only sees a map id and nearby units.
   Needed: a per-bot battleground block (type, team, status, scores) and a list of objectives (id, entry, position, state, carrier guid), or events for flag pickup/drop/capture,
   node capture, match end.
3. **Objective interaction.** Picking up a flag or capturing a banner is a game object use (`kUseObject`), checked in the object's script (it requires the bot to be near
   and eligible); untested with bots. The WSG flag drop (killing the carrier drops it on the ground, picked up by use of the dropped-flag object by guid, not by entry) has no
   guid-based use action.
4. **Honor and rank in the snapshot.** The module cannot read the rank (faction 2800 reputation / currency 3473), marks in the bags are readable only through `kListBags`
   on a trade (`BotTrade` kListBags) or through bags in the equip logic; there is no honor, rank or reputation field. Needed: `pvp_rank` (renown level) and the points in the
   snapshot; or the module counts results itself (win/loss events) and computes rank from the known curve, which drifts when real play or the config changes it.
5. **Match result event.** No event for a battle's end (winner, honor, marks); the module sees only that the map changed. A `kLeave` after the match is the only way out; the
   core's own auto-close timer (`TIME_AUTOCLOSE_BATTLEGROUND`) ports players out some time after the end.
6. **Group queues.** Solo only. Premade groups (5 bots queue together to fight as a team) are refused; the module cannot form a battleground group.
7. **Bots are not aware of battleground rules.** The module's combat and movement logic treats the battleground like open world: a bot attacks the nearest enemy but never goes
   to an objective, never returns to the spawn, never leaves the starting room's gate zone safely; the pull/leash and avoid-spell rules of dungeons (`doc/dungeon-strategy.md`)
   do not apply.
8. **Spending marks and reputation.** Vendors in this world sell the supply officer sets with no extended cost (money only), so there is no mark-spending action to write
   and no mark gate; a marks-based economy needs the core's extended costs to be filled in (`item_extended_cost` has 3448-currency rows for retail items only).
9. **Mounts and travel to the battlemaster.** Not needed: the queue is done remotely (`kQueue`, no NPC needed) and the core teleports the bot to the battleground; coming
   back, the bot lands at its last position (or the graveyard of the zone).

**Module side: exists** (bots repo, `src/battlegrounds.*`, `social_actions.*`, `bot_policy_social.cpp`; config `Battlegrounds` off by default, `BattlegroundMinLevel`,
`BattlegroundMaxWaitMin`; test `tests/battlegrounds_test.cpp`)

- Pure decision `battlegrounds::Decide` (queue, leave a stale queue, cooldown 60 min), `social::PickBattleground(level, seed)` (open battlegrounds by level; picks WSG
  from 10, AB from 20, AV from 51, EotS from 61), `IsBattlegroundMap`, the per-bot `TurnSocial` that queues the order, and the handling of the core's answer.
- A refused queue logs `battleground_gap` and the feature does nothing else (an older core, or `Bot.Social.Enabled` off).
- Not in the module: the track choice, the rank/marks tracking, any in-battleground behavior, the invitation acceptance, the PvP wishlist loader, the PvP gear grant.

## 7. Risks

- **Bots and real players.** A bots-only battleground works (the core fills it with whoever is queued), but the bots will also pop in a queue with real players. A real
  player's queue then fills faster, which is good; a real player who finds that the opposing team is all bots with a fixed role logic may find it dull or unfair. Keep a
  setting to hold the bots back (`PvpTrackBackOffWhenPlayersQueue`) and never queue bots for a battleground bracket where a real player waits alone (the bot count per side
  is capped by `PvpTrackMaxPerSide`, default half the team).
- **Queue flooding.** All PvP bots queue at once and fill every battleground instance, leaving the real players' queue with long waits behind the bots. Cap the share of
  queued bots (`PvpTrackQueueCap`, default 40 queued per battleground type) and stagger by a random delay per bot.
- **Performance.** A battleground is one map instance with 20 to 80 bots on it; Alterac Valley (80) is the heaviest; the module's per-bot snapshot read and decision turns
  already scale with the bots in the world, but combat spells and pathing inside one instance, with pathing on large maps (AV), may spike a world update. Start with Warsong Gulch (20 bots) and watch the tick time.
- **Deserter and AFK.** A bot that leaves a battle early gets the Deserter debuff and cannot queue for a while (the queue handler checks it); the module's cooldown has to be
  longer than the debuff, and a bot must not `kLeave` for any reason but the end of the match. A bot that stands idle is reported AFK and removed by the core.
- **Honor farming of the same bots.** Bots-only battles feed rank points with no real opponents; the 250 point win reward is on a config key. Whether to let bots reach R14 is
  an owner decision (it also decides the market value of the rank sets for real players).
- **Equip and bag space.** A wishlist of 25-30 entries cannot be granted at once (section 4). A bot with full bags is refused grants.
- **Faction.** The bot's faction decides the queue side, the sets and the supply officers; a Horde paladin has few PvP pieces.
- **Gear and PvE interaction.** The owner's rule for the PvE track is "PvE first, PvP rank pieces only fill gaps"; the PvP track is the reverse and must not hand its
  pieces to a PvE bot (the track decides which list a bot gets; a PvE bot stays on `data/prebis/`).
- **Mixed items.** The ranked names in `NAME_RANK` have two Sergeant titles (Alliance Sergeant R3, Horde Sergeant R3), so a bot of either faction sees them as `both`; irrelevant
  at level 60 (their item level is 52 and below the list floor).

## 8. Phased plan

**Phase 1 (done): data and design.** This page; `tools/gen_pvp.py` with `tests/gen_pvp_test.py` (ctest `gen_pvp`); `data/pvp/` lists for the nine classes and two factions.

**Phase 1b: use the wishlists (module only; sim).** A loader for `data/pvp/<classId>-<side>.txt` in the scenario/gear code (the same parser as `ScenarioGearFile`),
keeping entries up to the bot's rank band, best band per slot, granted a few at a time. Until the rank is readable, the rank is a setting (`PvpTrackRank`, default R10)
or a per-bot value in the store. Smallest useful: a sim run where PvP-track bots wear the R10 band and the battleground stage below is not needed yet.

**Phase 2: queue and accept (needs a core change, protocol 30).** Add `BattlegroundOp kAccept` and an invite-pending field in the snapshot (gap 1), plus the battleground
block in the snapshot (gap 2, type/team/status/score) and a match-end event (gap 5). In the module: the track choice and settings of section 1, the queue decision with the
cap (`Decide` extended with the track and the queue cap), accept the invite, and a trivial in-battleground behavior: walk to the enemy base (WSG) or the nearest node (AB),
attack what is near, `kLeave` when the match ends. This is the smallest slice that plays a real battle and earns real marks and rank points.

**Phase 3: objectives.** Flag carrier and escort roles (WSG), node capture (AB) with `kUseObject`, the guid-based use for a dropped flag (gap 3). Measure win rate and rank
progress per hour.

**Phase 4: rank and marks tracking, rank-gated grants.** Read the rank (gap 4) or count results; the wishlist grant follows the bot's rank band automatically; supply officer
purchases by marks if the core gets extended costs (gap 8).

**Phase 5: Alterac Valley and the rest.** AV objectives, group queues (gap 6) for premades, Eye of the Storm only if the content is opened at level 60.

## 9. Left to do

- The wishlists are not wired into any bot yet (phase 1b).
- Protocol 30 core work (gaps 1, 2, 4, 5).
- Owner decisions: share of bots on the track (`PvpTrackShare`), whether bots may reach R14, behavior when real players queue.
- Regenerate after the sim database changes: `tools/gen_pvp.py --mysql "ssh forever-test sudo -n mysql forever_test_world" --out data/pvp`; `--dump-json FILE` saves the data read.
