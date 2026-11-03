// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for guild milestone chat (src/guild_milestone.h) and its policy step (BotPolicy::OnGuildMemberLevel): off by default, the chance, one bot per
// event, a bot of our own never triggers it, and only bots of that guild speak. No core needed.
#include "advisor_text.h"
#include "bot_policy.h"
#include "guild_milestone.h"

#include <cstdio>
#include <set>

using namespace guild_milestone;
using forever_module::BotSnapshot;
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

Settings On(std::uint32_t pct = 30)
{
    Settings settings;
    settings.Enabled = true;
    settings.ChancePct = pct;
    return settings;
}

void OffByDefault()
{
    Speaker speaker;
    CHECK(!Settings{}.Enabled);
    CHECK(Settings{}.ChancePct == 30);
    CHECK(speaker.Pick(Settings{}, 1, 2, 10, { 5, 6 }, 1) == 0);
}

void ChanceMatchesTheConfig()
{
    Speaker speaker;
    int spoke = 0;
    for (std::uint64_t member = 1; member <= 2000; ++member)
        spoke += speaker.Pick(On(30), 1, member, 20, { 5, 6, 7 }, 99) != 0;
    CHECK(spoke > 480 && spoke < 720);
    Speaker always, never;
    for (std::uint64_t member = 1; member <= 50; ++member)
    {
        CHECK(always.Pick(On(100), 1, member, 20, { 5 }, 1) == 5);
        CHECK(never.Pick(On(0), 1, member, 20, { 5 }, 1) == 0);
    }
}

void OneBotPerEventAndNoBotsNoLine()
{
    Speaker speaker;
    std::uint64_t const first = speaker.Pick(On(100), 4, 77, 30, { 5, 6, 7, 8 }, 3);
    CHECK(first != 0);
    CHECK(speaker.Pick(On(100), 4, 77, 30, { 5, 6, 7, 8 }, 3) == 0);   // the same event again: nobody else answers it
    CHECK(speaker.Pick(On(100), 4, 77, 31, { 5, 6, 7, 8 }, 3) != 0);   // the next level is a new event
    CHECK(speaker.Pick(On(100), 4, 78, 30, {}, 3) == 0);               // a guild with no bot
}

void ThePickIsRandomOverTheMembers()
{
    std::set<std::uint64_t> seen;
    Speaker speaker;
    for (std::uint64_t member = 1; member <= 300; ++member)
        seen.insert(speaker.Pick(On(100), 4, member, 20, { 5, 6, 7 }, 3));
    CHECK(seen.size() == 3);
}

BotSnapshot Bot(std::uint64_t guid)
{
    BotSnapshot bot;
    bot.guid = guid;
    bot.health = bot.max_health = 1000;
    bot.level = 20;
    return bot;
}

void PolicySendsOneGuildLineFromAGuildBot()
{
    PolicyOptions options;
    options.GuildMilestone = On(100);
    auto pools = std::make_shared<advisor::Pools>();
    pools->GuildMilestones = { "Grats on {level}!" };
    options.Pools = pools;
    BotPolicy policy(options);
    // the guild each bot is in comes from its snapshot (BotSnapshot::guild_guid, layout 23)
    BotSnapshot a = Bot(5), b = Bot(6), c = Bot(7);
    a.guild_guid = 900;
    b.guild_guid = 900;
    c.guild_guid = 901;   // another guild
    WorldTickSnapshotView view;
    view.max_nearby_units = 4;
    view.bots = { a, b, c };
    view.nearby.resize(12);
    policy.OnGuildMemberLevel(view, 900, 1234, 25, 1000);
    auto orders = policy.TakeAdvisorOrders();
    CHECK(orders.size() == 1);
    if (orders.size() == 1)
    {
        CHECK(orders[0].What == advisor::Kind::GuildSay);
        CHECK(orders[0].Bot == 5 || orders[0].Bot == 6);
        CHECK(orders[0].Text == "Grats on 25!");
    }
    policy.OnGuildMemberLevel(view, 900, 1234, 25, 2000);   // the same event delivered twice
    CHECK(policy.TakeAdvisorOrders().empty());
    policy.OnGuildMemberLevel(view, 902, 1234, 26, 3000);   // a guild with no known bot
    CHECK(policy.TakeAdvisorOrders().empty());
}

void PolicyIsSilentWhenOff()
{
    BotPolicy off{ PolicyOptions{} };
    BotSnapshot a = Bot(5);
    a.guild_guid = 900;
    WorldTickSnapshotView view;
    view.max_nearby_units = 4;
    view.bots = { a };
    view.nearby.resize(4);
    off.OnGuildMemberLevel(view, 900, 1234, 25, 1000);
    CHECK(off.TakeAdvisorOrders().empty());
}

}  // namespace

int main()
{
    OffByDefault();
    ChanceMatchesTheConfig();
    OneBotPerEventAndNoBotsNoLine();
    ThePickIsRandomOverTheMembers();
    PolicySendsOneGuildLineFromAGuildBot();
    PolicyIsSilentWhenOff();
    if (failures == 0)
        std::printf("guild_milestone_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
