# Bots mailing each other (BotMail)

Status 2026-11-14, branch `feature/bot-trade-mail`. Off by default (`BotMail = 0`). Not run on the sim.

## What it does

Every `BotMailPlanEverySec` (120 s) the policy (`DriveMail`, `src/bot_policy_mail.cpp`) looks at the online bots and asks `botmail::Plan` (`src/bot_mail.cpp`) for mails.
Sender and recipient must be bots of the **same guild** (`guild_guid` in the snapshot) and the same faction (the core refuses a mail across factions). A sender is free
(alive, out of combat, not flying, in no party) and is not a player's own character (`IsAlt`).

- **Gold** (`BotMailGold`): a bot with more than `BotMailGoldKeepCopper` (5 gold) of surplus of at least `BotMailGoldMinCopper` mails the poorest guildmate below
  `BotMailNeedyBelowCopper` (2 gold) enough to reach twice that, at most `BotMailGoldMaxCopper` (1 gold) per mail.
- **Materials** (`BotMailMaterials`): to a guildmate whose profession uses the goods (Alchemy: herbs, Blacksmithing and Engineering: ore and bars, Leatherworking:
  leather, Tailoring: cloth) and the sender's does not. The sender's bags are listed (`BotTrade kListBags`) and `PickMaterial` takes the biggest whole stack of a known
  material between `BotMailMaterialMinCount` and `BotMailMaterialMaxCount`. The table is in code (`kMaterials`, by item id of the material; no gear is ever filtered by id).
- **Caps and cooldowns**: `BotMailDailyCap` per sender per day, `BotMailCooldownSec` between two mails of one sender, `BotMailRecipientDailyCap` per recipient per day,
  `BotMailMaxPerPlan` per plan. A refusal (or no stack to send) gives the day's slots back; the cooldown stays, so a failing pair is not retried at once.

The mail goes out through the core's existing `BotGiftMail` action (SDK `ActionRequest.gift_mail`, field 26): no mailbox visit, the core moves the item or money, charges
30 copper postage, delivers after `Mail.DeliveryDelay` (3600 s) and refuses a gift whose value (copper plus vendor price of the item) is above `Bot.Social.GiftMaxCopper`
(10000 by default). Config needed in the core: `Bot.Social.Enabled = 1`; `Bot.Trade.Enabled = 1` for the materials (bag list). The recipient's bags are not
touched until it collects.

**Core requirement (found in `src/server/game/Bots/BotMail.cpp`).** `WorldSession::CollectMailAsBot` takes only `MAIL_AUCTION` mail ("Player mail ... and any other mail a bot
receives are left alone"). A gift from `BotGiftMail` is sent as `MailSender(bot)`, that is `MAIL_NORMAL`, so a receiving bot would never take it and the money and
items would sit in its mailbox. The core must also collect `MAIL_NORMAL` mail whose sender is a bot character (money and items, not cash on delivery, delivered, bag space
checked as now), behind its own switch (suggested `Bot.Mail.CollectFromBots`, default 0). Until that is in, `BotMail` only moves goods into mailboxes: switch it on only with
that core change. Gifts to a human player's character work today (the player opens the mailbox).

## Why no live trade between bots

The SDK's `BotTrade` only accepts a trade a *player* opened (`kBegin`) and works the window of a bot that is in one. There is no action for a bot to open a trade with
another bot. Mail is the channel.

## Gear: ready, not wired

`botmail::PickGearRecipient` picks the guildmate who would gain most from a piece the sender will not wear, with the module's gear score
(`gear_score.h`: item level x 10 plus weighted stats), the class's best armour subclass at its level (`BestArmorSubclass`), the required level and the margin
`kSellMarginScore` (an empty slot is always an upgrade); soulbound and quest items never. It is unit-tested. The policy does not call it, because the module has no
inputs for it:

1. **What the item is.** `BotTrade kListBags` gives entry, count and name only. Needed (optional fields on `TradeBagItem`, additive): `inventory_type`, `item_level`,
   `quality`, `required_level`, `armor_subclass`, `bound` (and `stats` or a score per class).
2. **What a guildmate wears.** Needed: a worn-items listing for any controlled bot, for example `BotTradeAction kListWorn = 7` answering `worn_items` (slot, entry, item
   level, armour subclass), or a core-side score query "how much would `item_guid` of bot A improve bot B" (the same `BotGearUpgradeScore` the auction browse already
   uses for the requesting bot). The second is cheaper for the module and exact.
3. **Does the sender want it itself.** The loot facts of `feature/loot-facts` (`upgrade_score`, `usable`, `bound`, `required_level` on `PlayerItemLootedEvent`) say it for
   the looter; with them the sender knows an item is not an upgrade for itself.

With those, `DriveMail` would add `Kind::Gear` offers (the order needs the item guid and recipient, the subject is `GearSubject`) behind a key (`BotMailGear`, default 0).
A bot's own alts (characters of one account) need an account id in the snapshot; today only the guild tells friends from strangers.

## Unverified

- The core change above is not written; the effect of the whole feature depends on it.
- The sim has not run it. Watch `bot_mail:` event lines (sender, recipient, kind), `social_refused: ... reason=`, the `mail` table and the bots' copper.
- The materials table covers the classic gathering goods only; unknown entries are never sent.

Tests: `tests/bot_mail_test.cpp` (plan, caps, cooldown, refusal refund, materials, gear), `tests/social_test.cpp` (`BotMailIsPlannedFromTheGuildAndSentAsGiftMail`),
`tests/settings_test.cpp` (`BotMailSettings`).
