# Second-list features: duels, battlegrounds, guild join, mail gifts, inn downtime

Duels, battlegrounds, guild join and mail gifts are wired end to end. Each has a pure decision module (src/duels, src/battlegrounds, src/guild_join,
src/mail_gifts), off by default (keys in config/modules.d/forever-bots.conf.example). The per-bot turn `TurnSocial` (src/bot_policy_social.cpp) asks the
decision and queues a `social::Order`; the Run() thread sends it with the SDK's `ModuleClient::BotDuel`, `BotBattleground`, `BotGuild` or `BotGiftMail`
(src/social_actions.cpp, main_commands.cpp); the core's answer goes back to the policy (`NoteSocialOutcome`). Their tests are tests/*_test.cpp and
tests/social_test.cpp.

SDK: actions.proto fields 23 to 26 (additive, protocol "20" and layout 22 unchanged; see the SDK's CHANGELOG, "Unreleased: duels, battlegrounds, guild, gift
mail"). Core: needs a core that handles them (the core's Bot.Social.Enabled = 1). A core without them refuses the request ("unknown action"); the policy then
logs the fallback event `duel_gap`, `battleground_gap`, `guild_gap` or `gift_gap`, and the feature does nothing else.

What each feature does, and what feeds it:

- Duels: an idle, free bot of DuelMinLevel or more challenges the nearest player (not one of our bots) within 25 yards, at most once per DuelCooldownMin.
  A challenge to the bot is accepted (kAccept) when `BotPolicy::NoteDuelChallenge` has been told of it. Remaining: the core does not report a duel
  challenge to modules (no snapshot field or event), so nothing calls `NoteDuelChallenge` yet; only the challenge side is live.
- Battlegrounds: an idle bot of BattlegroundMinLevel or more in no party queues for a battleground its level allows (Warsong Gulch from 10, Arathi Basin from
  20, Alterac Valley from 51, Eye of the Storm from 61); a queue not popped within BattlegroundMaxWaitMin is left (kLeaveQueue); a refused queue, and a
  battleground left, wait out a 60 minute cooldown. "Inside" is the bot's map being a battleground map.
- Guild join: the snapshot shows no guild invitation, so a free bot without a guild asks the core to accept a pending one (kAccept) once a minute; the core
  refuses silently when there is none. Leave (kLeave) is sent when `NoteGuildLeaveRequest` was called; no chat verb calls it yet.
- Mail gifts: after a trade with a player finishes (OnBotTrade completed) the bot may thank that player with a gift, at most MailGiftDailyCap a day. The
  sender lists the bot's bags (kListBags), `gifts::PickGift` chooses a stack of 2 to 20 units, and BotGiftMail sends it; the core refuses a bound or quest item
  and anything above its value cap. A refused or empty gift gives the day's slot back.
- Inn downtime: still logic only. It uses the existing kRest action and kMoveTo to a point; the innkeeper position is not in the data yet, so GoToInn needs an
  inn table.
