# Group etiquette (GroupEtiquette, GroupEtiquetteLoot)

Off by default. Code: `src/group_etiquette.{h,cpp}`, tests `tests/group_etiquette_test.cpp`, lines `data/group_etiquette.txt`.

## Wait for the group (built)

In `RouteReadiness` (the route leader's walk), for a party of five or fewer, the ready gate `pullReady` also goes false while:

- the healer (a mana user, by `role_profile::StyleOf`) is under `GroupEtiquetteReadyPct` mana, or
- the tank is under `GroupEtiquetteReadyPct` health, or
- a living member is farther than `GroupEtiquetteStragglerYards` from the leader, for at most `GroupEtiquetteStragglerSec`.

Any wait ends after `GroupEtiquetteWaitSec`, and a released wait stays released until the party was ready once. Combat never waits; dead members
stay with the route's own dead-member wait. This sits beside the existing waits (`PartyMana`, `PullReadyManaPct`, `PullMinHpPct`, the `kRouteWaitYards`
together check, the wipe hold): they are not changed, this one is stricter for the healer and the tank only. Logged as `pull_hold ... reason=etiquette_<healer|tank|straggler>`.
Parties above five (ZG, Onyxia) are not touched.

## Chat (built)

Uses the existing party chat order (`advisor::Kind::PartySay`), only from a quiet bot (out of combat, alive, nothing hostile near).
- greeting: 2 to 9 s after a bot first sees its dungeon party, said by at most two members;
- "ready?": the leader, when an etiquette wait ends, once per 3 minutes per party;
- thanks: a bot that was dead and is up again with a living member within 40 yards, within 90 s, once per 2 minutes.
Limits: a bot at most once a minute, a party once per 12 s and 6 lines per 5 minutes. Pool lines are `kind|text`.

## Group loot rolls (rule built, NOT wired)

`group_etiquette::DecideRoll`: pass on grey items and on bind-on-pickup items the bot cannot use; greed on a usable item that is no upgrade (under one item
level of score) or does not suit the spec, and on a bind-on-equip item nobody uses; need on a real upgrade, unless a visible groupmate that can use it
gains more (a tie goes to the lower bot id). The module has no way to see a roll or answer one: protocol 29 has no roll event and no roll action
(the only loot facts are `PlayerItemLootedEvent`, after the item is stored, and `kLoot`).

What the core would need (optional proto fields, no new hook if the roll start can be an event of an existing hook; otherwise a hook, which needs a module approve):
1. an event `GroupLootRollEvent`: roll id, the bot, item entry, quality, inventory type, item level, bind type, and per bot the upgrade score (core's `BotGearRules` scale) and whether the class and spec can use it, plus the same for each groupmate it can see;
2. an action `LootRollAction`: bot, roll id, choice need, greed or pass (and disenchant).
Then the module calls `DecideRoll` for the event and answers. Until then `GroupEtiquetteLoot` has no effect.
