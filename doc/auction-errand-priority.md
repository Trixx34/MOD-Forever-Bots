# AuctionErrandPriority (2026-11-23)

Sim arm E (2026-11-22): 1188 `errand_start`, all `kind=train`, none `kind=auction`. The scheduler starts training before the Auction House, and the feed
(`src/main_feeds.cpp`) only sets `AuctionDue` for a bot that is not `TrainDue`, so a bot with a profession rank due never went to the house.

## The key

`AuctionErrandPriority` (default 0, 0..2), read with the other `Errand*` keys, in `config/modules.d/forever-bots.conf.example`.

| Value | Behaviour |
|---|---|
| 0 | As before: training first, an auction errand only for a bot that is not due training. Nothing else changes. |
| 1 | A bot that is due training and also has listable goods with its bags at or above `AuctionBagFullPct` goes to the Auction House first. |
| 2 | The same bot alternates: training first, then the house, then training, per bot. |

Only that bot is touched (`errand::Bot::AuctionReady`); a bot with empty or part-full bags, or nothing listable, trains first as before. The daily
minutes, the gap, the per-city cap and the starts per tick still apply. The auction trip goes to the faction's auction capitals (`AuctionCities`).
Bag use is `16 - free slots` over the assumed 16 (same as `AuctionBagFullPct`).

## The diagnostic

Always on (it logs, it does not change behaviour), for a bot with an auctioneer on its map that has looted items or bags at or above `AuctionBagFullPct`:

    forever-bots: event auction_due bot=<guid> items=<n> listable=<0|1> train_due=<0|1> bag_pct=<n>

`items` is the loot book size, `listable` the result of `auction::HasListable` (after the gear hold). One line per bot per period, the period being
`LogThrottleSec` but at least 60 s; a line after a held-back run carries `suppressed=N`.

## Sim verification

    grep -c 'event auction_due' forever-bots.log
    grep 'event auction_due' forever-bots.log | grep -c 'listable=1'
    grep 'event auction_due' forever-bots.log | grep 'listable=1' | grep -c 'train_due=1'     # the starved bots
    grep -c 'event errand_start.*kind=auction' forever-bots.log                                 # was 0 in arm E
    grep -c 'event errand_start.*kind=train' forever-bots.log
    grep 'event errand_done.*kind=auction' forever-bots.log | head
    grep -c 'auction: bot [0-9]* listed item' forever-bots.log                                  # lists after the walk
    grep -c 'auction: bot [0-9]* list_refused' forever-bots.log

With the key at 1 or 2, `kind=auction` starts must appear for bots with `listable=1 train_due=1` and a high `bag_pct`. If `listable=0` dominates, the
cause is the loot book (full bags refuse loot; gear held by `GearReady` unless `AuctionGearSell`), not the scheduler.

Tests: `tests/auction_errand_priority_test.cpp`.
