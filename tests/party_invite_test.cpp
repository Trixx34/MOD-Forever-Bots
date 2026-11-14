// SPDX-License-Identifier: GPL-2.0-or-later
// Party invite backoff (bot_policy_turn_town.cpp, Parties (M8); the quest-group invites of bot_policy_features.cpp use the same BotState helpers): a leader
// invited a member that never joined every kInviteRetryMs (15 s) for as long as it stayed out of the party. Neither the core's refusal of an invite nor a
// bot's decline names the member in what the module sees, so the count of invites without a join is the signal: the first kInviteFreeTries invites are 15 s
// apart, then the wait doubles per invite up to kInviteRetryMaxMs. A join, or an hour without an invite, starts the count again.
#include "bot_policy.h"
#include "bot_policy_internal.h"

#include <cstdio>
#include <string>
#include <vector>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotSnapshot;
using forever_module::MakeUnitRef;
using forever_module::WorldTickSnapshotView;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

std::uint64_t PlayerRefOf(std::uint64_t counter)
{
    return MakeUnitRef(2, 0, counter);
}

PolicyOptions TwoBotParty()
{
    PolicyOptions options;
    options.PartySize = 2;
    options.PartyCount = 1;
    options.PartyPercent = 100;
    options.RandomPct = 0;
    options.ClassOf = [](std::uint64_t guid) { return guid == 1 ? 1u : 5u; };   // a warrior leads, a priest follows
    return options;
}

// Bot 1 leads (its own party of one), bot 2 is its member: in the party when `joined`, else in no party and with no invite waiting (it refused, declined, or
// the invite expired). `online` false leaves bot 2 out of the view.
WorldTickSnapshotView View(std::uint64_t tick, bool joined, bool online = true)
{
    WorldTickSnapshotView view;
    view.tick_number = tick;
    view.max_nearby_units = 1;
    for (std::uint64_t guid = 1; guid <= (online ? 2u : 1u); ++guid)
    {
        BotSnapshot bot;
        bot.guid = guid;
        bot.health = 1000;
        bot.max_health = 1000;
        bot.level = 20;
        if (guid == 1 || joined)
        {
            bot.group_role = guid == 1 ? 2 : 1;
            bot.group_leader_guid = PlayerRefOf(1);
            bot.group_size = joined ? 2 : 1;
        }
        view.bots.push_back(bot);
    }
    view.nearby.resize(view.bots.size());
    return view;
}

bool InvitesBot2(std::vector<BotAction> const& actions)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kGroup && action.bot_guid == 1 && action.spell_id == 1 && action.target_guid == PlayerRefOf(2))
            return true;
    return false;
}

// The leader's invites to bot 2 from `fromMs` to `toMs` (one tick a second), with bot 2 out of the party the whole time.
std::vector<std::uint64_t> InvitesBetween(BotPolicy& policy, std::uint64_t& tick, std::uint64_t fromMs, std::uint64_t toMs)
{
    std::vector<std::uint64_t> at;
    for (std::uint64_t now = fromMs; now <= toMs; now += 1000)
        if (InvitesBot2(policy.Decide(View(++tick, false), now)))
            at.push_back(now);
    return at;
}

void TheWaitGrowsAfterThreeInvitesWithoutAJoin()
{
    CHECK(InviteRetryWaitMs(1) == kInviteRetryMs && InviteRetryWaitMs(kInviteFreeTries) == kInviteRetryMs);
    CHECK(InviteRetryWaitMs(kInviteFreeTries + 1) == 2 * kInviteRetryMs);
    CHECK(InviteRetryWaitMs(kInviteFreeTries + 2) == 4 * kInviteRetryMs);
    CHECK(InviteRetryWaitMs(kInviteFreeTries + 6) == kInviteRetryMaxMs);   // 15 s << 6 is 16 minutes: capped at ten
    CHECK(InviteRetryWaitMs(1000) == kInviteRetryMaxMs);                    // no overflow for a long refusal
}

void ARefusingMemberIsInvitedLessAndLessOften()
{
    BotPolicy policy(TwoBotParty());
    std::uint64_t tick = 0;
    std::vector<std::uint64_t> const at = InvitesBetween(policy, tick, 1000, 1000 + 40 * 60 * 1000);   // 40 minutes
    CHECK(at.size() >= 8);
    if (at.size() < 8)
        return;
    // 15 s, 15 s, 15 s, then 30 s, 60 s, 120 s, 240 s, 480 s, 600 s (the cap) between invites
    std::vector<std::uint64_t> const gaps = { 15000, 15000, 15000, 30000, 60000, 120000, 240000 };
    for (std::size_t i = 0; i < gaps.size(); ++i)
        CHECK(at[i + 1] - at[i] == gaps[i]);
    for (std::size_t i = 8; i + 1 < at.size(); ++i)
        CHECK(at[i + 1] - at[i] <= kInviteRetryMaxMs && at[i + 1] - at[i] >= 480000);
    // The old pace was an invite every 15 s: 160 in 40 minutes. Now about a dozen.
    CHECK(at.size() <= 14);
}

void AJoinStartsTheCountAgain()
{
    BotPolicy policy(TwoBotParty());
    std::uint64_t tick = 0;
    std::vector<std::uint64_t> const before = InvitesBetween(policy, tick, 1000, 1000 + 10 * 60 * 1000);
    CHECK(before.size() >= 5);   // in the backoff by now
    // Bot 2 joins, stays a minute, then leaves: it is invited again at once, and then 15 s apart, as a new member.
    std::uint64_t now = 1000 + 10 * 60 * 1000 + 1000;
    for (std::uint64_t end = now + 60000; now < end; now += 1000)
        CHECK(!InvitesBot2(policy.Decide(View(++tick, true), now)));
    std::vector<std::uint64_t> const after = InvitesBetween(policy, tick, now, now + 60000);
    CHECK(after.size() >= 4);
    if (after.size() >= 4)
    {
        CHECK(after[0] == now);
        CHECK(after[1] - after[0] == kInviteRetryMs && after[2] - after[1] == kInviteRetryMs);
    }
}

void AQuietHourStartsTheCountAgain()
{
    BotPolicy policy(TwoBotParty());
    std::uint64_t tick = 0;
    std::vector<std::uint64_t> const before = InvitesBetween(policy, tick, 1000, 1000 + 20 * 60 * 1000);
    CHECK(before.size() >= 7);
    // Bot 2 goes offline for an hour and a half: no invites meanwhile.
    std::uint64_t now = 1000 + 20 * 60 * 1000 + 1000;
    for (std::uint64_t end = now + 90 * 60 * 1000; now < end; now += 10000)
        CHECK(!InvitesBot2(policy.Decide(View(++tick, false, false), now)));
    std::vector<std::uint64_t> const after = InvitesBetween(policy, tick, now, now + 60000);
    CHECK(after.size() >= 3);
    if (after.size() >= 3)
        CHECK(after[1] - after[0] == kInviteRetryMs && after[2] - after[1] == kInviteRetryMs);
}

} // namespace

int main()
{
    TheWaitGrowsAfterThreeInvitesWithoutAJoin();
    ARefusingMemberIsInvitedLessAndLessOften();
    AJoinStartsTheCountAgain();
    AQuietHourStartsTheCountAgain();

    if (failures == 0)
        std::printf("party_invite_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
