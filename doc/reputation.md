# Reputation goals

Status: module side done, off by default (`ReputationGrind = 0`). The core does not send the facts it needs yet, so with the key on nothing happens until it does.

## Idea

Bots at a level pick one faction whose reputation gates equipment, earn the reputation with the quests, dungeons and kills they already play (the existing quest lists and
level dungeons give reputation as a side effect), and walk to the faction's vendor when the core says that vendor sells them a reputation-gated upgrade they may buy now. The
buy is the core's: it picks the best upgrade by its gear score (the same scoring as the equip pass, by the numbers, never by item id) and checks the rank at the vendor.
The module never names an item.

## What the sim data holds (2026-11-14)

`tools/gen_reputation_goals.py` reads the hotfix `item_sparse` gate (`MinFactionID`, `MinReputation`), `npc_vendor` and the quest reward factions, and writes
`data/reputation_goals.txt`. Result: 6 factions, and only one has a vendor in `npc_vendor` (Nightclaw Druids, one level 15 piece at Exalted). The classic reputation vendors
(Argent Dawn, Thorium Brotherhood, Timbermaw, Cenarion Circle, ...) sell recipes, patterns and quest items in this database, not gear, and the Darkspear Raiders / Theramore
Expeditionary Force gear (level 28 to 60 cloaks, weapons, gloves and shoulders at Honored to Exalted) has no vendor. The hotfix table holds only 117 gated items: the base
`ItemSparse.db2` is a sparse table the repository's reader cannot read, so gated classic items that exist only there are not in the list. A faction with no vendor is never a goal, so
the feature is idle by data today and starts to work as vendors are added (run the tool again; nothing is selected by item entry).

## What was built (module)

- `src/reputation.{h,cpp}`: the ranks (the core's `ReputationRank`, thresholds -42000, -6000, -3000, 0, 3000, 9000, 21000, 42000), the goal table parser, the goal choice and the visit rules.
- Goal choice: a faction of the bot's side, with gear within 5 levels of the bot's level, with a vendor, not yet Exalted; the one with the most reputation quests, ties spread by a hash of the bot's guid; kept while it stays a candidate.
- `BotPolicy::TurnReputation` (`src/bot_policy_reputation.cpp`): an idle, solo bot out of combat whose core facts name a reputation vendor walks there (`rep_vendor` move, 3 minute limit) and sends `kVendor` with the buy bit. Judged 7 seconds later: less money or fewer free slots is a purchase; otherwise the next visit waits 30 minutes, doubling up to 6 hours.
- Journal lines: `rep_goal`, `rep_buy`, `rep_buy_done`, `rep_buy_failed`.
- Keys: `ReputationGrind` (0), `ReputationMinLevel` (10), `ReputationTripSec` (600), `ReputationMinCopper` (10000), `ReputationGoalFile`.

## What the core needs (not done, protocol 29 stays compatible)

1. Snapshot facts, appended per the layout 25 growth rule (a new section or a tail on `BotSnapshot`; the Tier 2 layout version goes up, the base stays):
   - `rep_vendor_valid`, `rep_vendor_x/y/z`, `rep_vendor_guid`: the nearest vendor on the bot's map that sells a reputation-gated piece the bot can buy now (`Player::GetReputationRank(faction) >= ItemTemplate::GetRequiredReputationRank()` with `GetRequiredReputationFaction()`), usable by the bot's class and better than what it wears by the core's gear score. Same shape as `ammo_vendor_*`, `goods_vendor_*`.
   - Optionally a standings section: `{faction id, standing int32, rank uint8}` for every faction with a non-zero standing (`ReputationMgr::GetReputation(factionId)`, `GetRank`). The module only needs it to drop Exalted goals; the ranks are read from `ReputationMgr` thresholds, which the PvP rank track of the Karrz1 merge changes for faction 2800 (renown), so the PvP faction must be left out of the block or reported with its own curve.
2. `kVendor` bit 32768 (`spell_id`; bits 12 to 14 carry the gear profile as for bit 2048): buy, at the vendor in `target_guid`, the best reputation-gated upgrade by the gear score, one piece per action, keeping the money reserve; refused with a reason when none is buyable. Documented in `tier2_layout.h` next to the other service bits, config key on the core side (for example `Bot.Vendor.BuyReputationGear`, off by default).
3. Wire `PolicyOptions::ReputationFacts` in `main_options.cpp` to the new snapshot fields once the SDK has them (today it is empty on purpose).

## Not done / unverified

- Nothing was run against a core: the policy test uses a fake facts provider. The judge rule (money or slots went down) is the only signal the module has without a vendor result.
- Faction side is read from the parent chain in `Faction.db2` (Alliance 469, Horde 67); the SoD-style factions have neither parent, so they are side 0 (both).
- Steering the earning (preferring dungeons or quests of the goal faction) is not built: the goal is only logged and used to choose vendors. The data gives the number of reputation quests per faction, which a later quest-planner weight can use.
