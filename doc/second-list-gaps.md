# Second-list features: decisions in, actions missing

Duels, battlegrounds, inn downtime, guild join and mail gifts are pure decision modules (src/duels, src/battlegrounds, src/inn_downtime,
src/guild_join, src/mail_gifts), each off by default (keys in config/modules.d/forever-bots.conf.example). Their tests are tests/*_test.cpp.

Not wired into bot_policy.cpp yet, because the SDK has no action for these:

- Duel challenge and accept: no duel action in the SDK. Gap: `duel_gap`.
- Battleground queue and leave: no battleground queue action. Gap: `battleground_gap`.
- Guild accept and leave: no guild action (kGroup covers parties only). Gap: `guild_gap`.
- Gift by mail: the auction mail path only carries auction results. Gap: `gift_gap`.
- Inn downtime: uses the existing kRest action and kMoveTo to a point; the innkeeper position is not in the data yet, so GoToInn needs an inn table.

Adding any of these needs a new SDK action and a matching core handler, which is a core change and a layout or protocol decision.
