// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the bot guild grouping and the guild-mate roster query (src/bot_guilds.h). No core or SDK needed.
#include "bot_guilds.h"

#include <cstdio>
#include <map>
#include <set>

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
}  // namespace

using namespace bot_guilds;

namespace {

Bot MakeBot(std::uint64_t guid, bool alliance, std::uint64_t guild = 0, bool online = false, std::uint32_t level = 60)
{
    Bot bot;
    bot.Guid = guid;
    bot.Alliance = alliance;
    bot.Guild = guild;
    bot.Online = online;
    bot.Level = level;
    return bot;
}

// The pool after the plan is carried out: joins applied, each new guild given the next guid from `nextGuild`.
std::vector<Bot> Apply(std::vector<Bot> pool, Plan const& plan, std::uint64_t& nextGuild)
{
    std::map<std::uint64_t, std::uint64_t> guildOf;
    for (Join const& join : plan.Joins)
        guildOf[join.Bot] = join.Guild;
    for (NewGuild const& created : plan.Created)
    {
        std::uint64_t guild = nextGuild++;
        for (std::uint64_t member : created.Members)
            guildOf[member] = guild;
    }
    for (Bot& bot : pool)
        if (guildOf.count(bot.Guid))
            bot.Guild = guildOf[bot.Guid];
    return pool;
}

void EmptyPoolMakesNothing()
{
    Plan plan = PlanGuilds(Settings{}, {});
    CHECK(plan.Joins.empty() && plan.Created.empty() && plan.MixedGuilds.empty());
}

void GroupsOfTheSizeWithMasters()
{
    std::vector<Bot> pool;
    // 25 Alliance bots, given in a shuffled guid order
    for (std::uint64_t g : { 14, 3, 27, 9, 1, 22, 5, 18, 11, 30, 2, 25, 7, 16, 4, 20, 12, 8, 29, 6, 10, 24, 13, 26, 15 })
        pool.push_back(MakeBot(100 + g, true));
    Plan plan = PlanGuilds(Settings{ 10 }, pool);
    CHECK(plan.Joins.empty());
    CHECK(plan.Created.size() == 3);
    if (plan.Created.size() == 3)
    {
        CHECK(plan.Created[0].Name == "BotGuild A001");
        CHECK(plan.Created[1].Name == "BotGuild A002");
        CHECK(plan.Created[2].Name == "BotGuild A003");
        CHECK(plan.Created[0].Members.size() == 10);
        CHECK(plan.Created[1].Members.size() == 10);
        CHECK(plan.Created[2].Members.size() == 5);
        CHECK(plan.Created[0].Master == 101);                          // lowest guid of the first guild
        CHECK(plan.Created[0].Members.front() == plan.Created[0].Master);
        CHECK(plan.Created[2].Master == plan.Created[2].Members.front());
        CHECK(plan.Created[0].Alliance);
    }
    // every bot exactly once
    std::map<std::uint64_t, int> count;
    for (NewGuild const& created : plan.Created)
        for (std::uint64_t member : created.Members)
            ++count[member];
    CHECK(count.size() == 25);
    for (auto const& [guid, n] : count)
        CHECK(n == 1);
}

void FactionsNeverShareAGuild()
{
    std::vector<Bot> pool;
    for (std::uint64_t g = 1; g <= 12; ++g)
        pool.push_back(MakeBot(g, true));
    for (std::uint64_t g = 13; g <= 19; ++g)
        pool.push_back(MakeBot(g, false));
    Plan plan = PlanGuilds(Settings{ 10 }, pool);
    std::map<std::uint64_t, bool> alliance;
    for (Bot const& bot : pool)
        alliance[bot.Guid] = bot.Alliance;
    CHECK(plan.Created.size() == 3);   // A: 10 + 2, H: 7
    bool allSame = true;
    for (NewGuild const& created : plan.Created)
        for (std::uint64_t member : created.Members)
            if (alliance[member] != created.Alliance)
                allSame = false;
    CHECK(allSame);
    CHECK(plan.Created[2].Name == "BotGuild H001");
    CHECK(plan.Created[2].Members.size() == 7);
}

void FirstRunThenRerunIsEmpty()
{
    std::vector<Bot> pool;
    for (std::uint64_t g = 1; g <= 23; ++g)
        pool.push_back(MakeBot(g, g % 3 != 0));
    std::uint64_t nextGuild = 900;
    Plan first = PlanGuilds(Settings{ 5 }, pool);
    CHECK(!first.Created.empty());
    std::vector<Bot> placed = Apply(pool, first, nextGuild);
    Plan second = PlanGuilds(Settings{ 5 }, placed);
    CHECK(second.Joins.empty());
    CHECK(second.Created.empty());
    CHECK(second.MixedGuilds.empty());
}

void FillsExistingRoomWithoutMovingMembers()
{
    std::vector<Bot> pool;
    // guild 500: 4 Alliance members (room for 6), guild 600: full
    for (std::uint64_t g = 1; g <= 4; ++g)
        pool.push_back(MakeBot(g, true, 500));
    for (std::uint64_t g = 11; g <= 20; ++g)
        pool.push_back(MakeBot(g, true, 600));
    for (std::uint64_t g = 31; g <= 38; ++g)
        pool.push_back(MakeBot(g, true));   // 8 unplaced
    Plan plan = PlanGuilds(Settings{ 10 }, pool);
    CHECK(plan.Joins.size() == 6);
    for (Join const& join : plan.Joins)
        CHECK(join.Guild == 500 && join.Bot >= 31);   // placed bots are never in a join
    CHECK(plan.Created.size() == 1);
    if (plan.Created.size() == 1)
    {
        CHECK(plan.Created[0].Name == "BotGuild A003");   // two Alliance guilds already exist
        CHECK(plan.Created[0].Members.size() == 2);
    }
}

void MixedGuildsAreReportedAndUntouched()
{
    std::vector<Bot> pool;
    pool.push_back(MakeBot(1, true, 700));
    pool.push_back(MakeBot(2, false, 700));   // a guild with both factions
    pool.push_back(MakeBot(3, true));
    pool.push_back(MakeBot(4, false));
    Plan plan = PlanGuilds(Settings{ 10 }, pool);
    CHECK(plan.MixedGuilds.size() == 1 && plan.MixedGuilds[0] == 700);
    CHECK(plan.Joins.empty());               // nobody is put into the mixed guild
    CHECK(plan.Created.size() == 2);
}

void SizeIsClamped()
{
    CHECK(EffectiveGuildSize(Settings{ 0 }) == 2);
    CHECK(EffectiveGuildSize(Settings{ 1 }) == 2);
    CHECK(EffectiveGuildSize(Settings{ 10 }) == 10);
    CHECK(EffectiveGuildSize(Settings{ 100 }) == 40);
    std::vector<Bot> pool;
    for (std::uint64_t g = 1; g <= 5; ++g)
        pool.push_back(MakeBot(g, false));
    Plan plan = PlanGuilds(Settings{ 1 }, pool);   // clamped to 2: 2 + 2 + 1
    CHECK(plan.Created.size() == 3);
}

void RosterListsOnlineMatesAtSixty()
{
    std::vector<Bot> pool;
    pool.push_back(MakeBot(1, true, 800));                       // self
    pool.push_back(MakeBot(9, true, 800, true, 60));             // mate, online, 60: listed
    pool.push_back(MakeBot(4, true, 800, true, 62));             // mate above 60: listed
    pool.push_back(MakeBot(2, true, 800, false, 60));            // offline: not
    pool.push_back(MakeBot(3, true, 800, true, 59));             // below 60: not
    pool.push_back(MakeBot(5, true, 801, true, 60));             // other guild: not
    pool.push_back(MakeBot(6, false, 800, true, 60));            // other faction in a guild (not expected): not
    pool.push_back(MakeBot(7, true, 0, true, 60));               // no guild
    std::vector<std::uint64_t> mates = OnlineGuildMates(pool, 1);
    CHECK(mates.size() == 2);
    CHECK(mates.size() == 2 && mates[0] == 4 && mates[1] == 9);   // sorted by guid
    std::vector<std::uint64_t> all = OnlineGuildMates(pool, 1, 1);  // minLevel 1: the 59 joins, the offline one still not
    CHECK(all.size() == 3);
    CHECK(OnlineGuildMates(pool, 7).empty());                    // a bot with no guild has no mates
    CHECK(OnlineGuildMates(pool, 999).empty());                  // unknown bot
}

}  // namespace

int main()
{
    EmptyPoolMakesNothing();
    GroupsOfTheSizeWithMasters();
    FactionsNeverShareAGuild();
    FirstRunThenRerunIsEmpty();
    FillsExistingRoomWithoutMovingMembers();
    MixedGuildsAreReportedAndUntouched();
    SizeIsClamped();
    RosterListsOnlineMatesAtSixty();
    if (failures == 0)
        std::printf("bot_guilds_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
