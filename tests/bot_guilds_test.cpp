// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the bot guild lifecycle (PlanMoves) and the guild-mate roster query (GuildMates, src/bot_guilds.h). No core or SDK needed.
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

Bot MakeBot(std::uint64_t guid, bool alliance, std::uint64_t guild, std::uint32_t level, std::optional<Purpose> want, bool online = true)
{
    Bot bot;
    bot.Guid = guid;
    bot.Alliance = alliance;
    bot.Guild = guild;
    bot.Level = level;
    bot.Want = want;
    bot.Online = online;
    return bot;
}

Guild MakeGuild(std::uint64_t guid, Purpose kind, bool alliance)
{
    Guild guild;
    guild.Guid = guid;
    guild.Kind = kind;
    guild.Alliance = alliance;
    return guild;
}

Move const* FindMove(Plan const& plan, std::uint64_t bot)
{
    for (Move const& move : plan.Moves)
        if (move.Bot == bot)
            return &move;
    return nullptr;
}

// Carries the plan out: leavers and joiners move, each created guild gets the next guid (from `next`), dissolved guilds are removed.
void Apply(Plan const& plan, std::vector<Guild>& guilds, std::vector<Bot>& pool, std::uint64_t& next)
{
    std::vector<std::uint64_t> newGuid;
    for (NewGuild const& created : plan.Created)
    {
        newGuid.push_back(next);
        guilds.push_back(MakeGuild(next++, created.Kind, created.Alliance));
    }
    for (Move const& move : plan.Moves)
        for (Bot& bot : pool)
            if (bot.Guid == move.Bot)
                bot.Guild = move.To ? move.To : (move.NewGuild >= 0 ? newGuid[move.NewGuild] : 0);
    for (std::uint64_t dissolved : plan.Dissolved)
        for (std::size_t i = 0; i < guilds.size(); ++i)
            if (guilds[i].Guid == dissolved)
            {
                guilds.erase(guilds.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
}

// Each bot appears at most once in the moves, and a move leaves a guild or joins one.
bool MovesAreOneEach(Plan const& plan)
{
    std::set<std::uint64_t> seen;
    for (Move const& move : plan.Moves)
    {
        if (!seen.insert(move.Bot).second)
            return false;
        if (move.From == 0 && move.To == 0 && move.NewGuild < 0)
            return false;
    }
    return true;
}

void JoinLevelingFromThePool()
{
    std::vector<Bot> pool = { MakeBot(1, true, 0, 20, Purpose::Leveling), MakeBot(2, true, 0, 35, Purpose::Leveling) };
    Plan plan = PlanMoves(Settings{}, {}, pool);
    CHECK(plan.Moves.size() == 2);
    CHECK(plan.Created.size() == 1);
    if (plan.Created.size() == 1)
    {
        CHECK(plan.Created[0].Kind == Purpose::Leveling);
        CHECK(plan.Created[0].Name == "BotLvl A001");
        CHECK(plan.Created[0].Master == 1);
        CHECK(plan.Created[0].Members.size() == 2);
    }
    Move const* first = FindMove(plan, 1);
    CHECK(first && first->From == 0 && first->To == 0 && first->NewGuild == 0);
    CHECK(MovesAreOneEach(plan));
}

void LeaveLevelingAtTheCap()
{
    std::vector<Guild> guilds = { MakeGuild(500, Purpose::Leveling, true) };
    std::vector<Bot> pool = { MakeBot(1, true, 500, 60, Purpose::Leveling), MakeBot(2, true, 500, 59, Purpose::Leveling),
                              MakeBot(3, true, 500, 60, Purpose::Leveling) };
    Plan plan = PlanMoves(Settings{}, guilds, pool);
    Move const* leaver = FindMove(plan, 1);
    CHECK(leaver && leaver->From == 500 && leaver->To == 0 && leaver->NewGuild == -1);   // at 60: out
    CHECK(FindMove(plan, 2) == nullptr);                                                 // at 59: stays
    CHECK(FindMove(plan, 3) && FindMove(plan, 3)->From == 500);
    CHECK(plan.Dissolved.empty());                                                       // bot 2 still in it
}

void EmptyLevelingGuildIsDissolved()
{
    std::vector<Guild> guilds = { MakeGuild(500, Purpose::Leveling, true), MakeGuild(501, Purpose::Utility, true) };
    std::vector<Bot> pool = { MakeBot(1, true, 500, 60, Purpose::Leveling) };
    Plan plan = PlanMoves(Settings{}, guilds, pool);
    CHECK(plan.Dissolved.size() == 1 && plan.Dissolved[0] == 500);
    CHECK(plan.Created.empty());
}

void ReassignedLevelingBotMovesToUtility()
{
    std::vector<Guild> guilds = { MakeGuild(500, Purpose::Leveling, true) };
    std::vector<Bot> pool = { MakeBot(1, true, 500, 30, Purpose::Utility) };
    Plan plan = PlanMoves(Settings{}, guilds, pool);
    Move const* move = FindMove(plan, 1);
    CHECK(move && move->From == 500 && move->To == 0 && move->NewGuild == 0);
    CHECK(plan.Created.size() == 1 && plan.Created[0].Kind == Purpose::Utility && plan.Created[0].Name == "BotUtil A001");
    CHECK(plan.Dissolved.size() == 1 && plan.Dissolved[0] == 500);
}

void UtilityStaysPut()
{
    std::vector<Guild> guilds = { MakeGuild(600, Purpose::Utility, true), MakeGuild(700, Purpose::Raiding, true) };
    std::vector<Bot> pool = { MakeBot(1, true, 600, 60, Purpose::Utility), MakeBot(2, true, 600, 12, Purpose::Utility),
                              MakeBot(3, true, 600, 60, std::nullopt) };
    Plan plan = PlanMoves(Settings{}, guilds, pool);
    CHECK(plan.Moves.size() == 1);                       // bot 3 has no bot guild wanted: out
    CHECK(FindMove(plan, 1) == nullptr);                 // a level-60 utility bot does not go to raiding
    CHECK(FindMove(plan, 2) == nullptr);
    Move const* out = FindMove(plan, 3);
    CHECK(out && out->From == 600 && out->To == 0 && out->NewGuild == -1);
    CHECK(plan.Dissolved.empty());                       // utility guilds are not dissolved
}

void RaidingNeedsQualification()
{
    std::vector<Guild> guilds = { MakeGuild(700, Purpose::Raiding, true) };
    Settings settings;
    settings.RaidQualified = [](Bot const& bot) { return bot.Guid != 2; };   // bot 2 lacks the attunement
    std::vector<Bot> pool = { MakeBot(1, true, 0, 59, Purpose::Raiding),     // below the cap: no
                              MakeBot(2, true, 0, 60, Purpose::Raiding),     // at the cap, not attuned: no
                              MakeBot(3, true, 0, 60, Purpose::Raiding) };   // qualified: joins
    Plan plan = PlanMoves(settings, guilds, pool);
    CHECK(FindMove(plan, 1) == nullptr);
    CHECK(FindMove(plan, 2) == nullptr);
    Move const* joined = FindMove(plan, 3);
    CHECK(joined && joined->To == 700 && joined->From == 0);
    CHECK(plan.Created.empty());
}

void QualifiedLevelingBotMovesStraightToRaiding()
{
    std::vector<Guild> guilds = { MakeGuild(500, Purpose::Leveling, true), MakeGuild(700, Purpose::Raiding, true) };
    std::vector<Bot> pool = { MakeBot(1, true, 500, 60, Purpose::Raiding) };
    Plan plan = PlanMoves(Settings{}, guilds, pool);
    Move const* move = FindMove(plan, 1);
    CHECK(move && move->From == 500 && move->To == 700);
    CHECK(plan.Dissolved.size() == 1 && plan.Dissolved[0] == 500);
}

void UnqualifiedLevelingBotLeavesAndWaits()
{
    std::vector<Guild> guilds = { MakeGuild(500, Purpose::Leveling, true) };
    Settings settings;
    settings.RaidQualified = [](Bot const&) { return false; };
    std::vector<Bot> pool = { MakeBot(1, true, 500, 60, Purpose::Raiding) };
    Plan plan = PlanMoves(settings, guilds, pool);
    Move const* move = FindMove(plan, 1);
    CHECK(move && move->From == 500 && move->To == 0 && move->NewGuild == -1);   // out of leveling, not into raiding
}

void FactionsDoNotShareAndMixedGuildsAreLeftAlone()
{
    std::vector<Guild> guilds = { MakeGuild(600, Purpose::Utility, true), MakeGuild(800, Purpose::Utility, true) };
    std::vector<Bot> pool = { MakeBot(1, false, 0, 40, Purpose::Utility),    // Horde, unguilded
                              MakeBot(2, true, 800, 40, Purpose::Utility),   // 800 has a Horde member too: mixed
                              MakeBot(3, false, 800, 40, Purpose::Utility),
                              MakeBot(4, true, 600, 40, Purpose::Utility) };
    Plan plan = PlanMoves(Settings{}, guilds, pool);
    CHECK(plan.Mixed.size() == 1 && plan.Mixed[0] == 800);
    CHECK(FindMove(plan, 2) == nullptr && FindMove(plan, 3) == nullptr);   // nobody moves out of the mixed guild
    CHECK(FindMove(plan, 4) == nullptr);                                  // Alliance bot stays in its utility guild
    Move const* horde = FindMove(plan, 1);
    CHECK(horde && horde->To == 0 && horde->NewGuild == 0);              // a new Horde guild, never 600 or 800
    CHECK(plan.Created.size() == 1 && !plan.Created[0].Alliance && plan.Created[0].Name == "BotUtil H001");
}

void ExternalGuildIsNotTouched()
{
    std::vector<Guild> guilds = { MakeGuild(500, Purpose::Leveling, true) };
    std::vector<Bot> pool = { MakeBot(1, true, 999, 60, Purpose::Leveling),   // a player guild this plan does not know
                              MakeBot(2, true, 0, 40, std::nullopt) };
    Plan plan = PlanMoves(Settings{}, guilds, pool);
    CHECK(plan.Moves.empty());
    CHECK(plan.Dissolved.size() == 1);   // 500 has no bot of ours: empty leveling guild
}

void SizeAndNames()
{
    std::vector<Bot> pool;
    for (std::uint64_t g = 1; g <= 12; ++g)
        pool.push_back(MakeBot(g, true, 0, 20, Purpose::Leveling));
    Plan plan = PlanMoves(Settings{}, {}, pool);
    CHECK(plan.Created.size() == 2);
    if (plan.Created.size() == 2)
    {
        CHECK(plan.Created[0].Members.size() == 10);
        CHECK(plan.Created[1].Members.size() == 2);
        CHECK(plan.Created[1].Name == "BotLvl A002");
    }
    Settings clamped;
    clamped.GuildSize = 100;
    CHECK(EffectiveGuildSize(clamped) == 40);
    clamped.GuildSize = 0;
    CHECK(EffectiveGuildSize(clamped) == 2);
}

void IdempotentRerunAfterFullScenario()
{
    std::vector<Guild> guilds = { MakeGuild(500, Purpose::Leveling, true), MakeGuild(600, Purpose::Utility, true),
                                  MakeGuild(700, Purpose::Raiding, true) };
    std::vector<Bot> pool;
    for (std::uint64_t g = 1; g <= 14; ++g)
        pool.push_back(MakeBot(g, true, 0, 15 + static_cast<std::uint32_t>(g), Purpose::Leveling));   // leveling joiners
    pool.push_back(MakeBot(50, true, 500, 60, Purpose::Leveling));                               // leaves at the cap
    pool.push_back(MakeBot(51, false, 0, 30, Purpose::Utility));                                  // Horde utility joiner
    pool.push_back(MakeBot(52, true, 600, 60, Purpose::Raiding));                                 // utility to raiding, qualified
    pool.push_back(MakeBot(53, true, 600, 60, Purpose::Utility));                                 // stays
    std::uint64_t next = 900;

    Plan first = PlanMoves(Settings{}, guilds, pool);
    CHECK(MovesAreOneEach(first));
    CHECK(!first.Moves.empty());
    Apply(first, guilds, pool, next);

    Plan second = PlanMoves(Settings{}, guilds, pool);
    CHECK(second.Moves.empty());
    CHECK(second.Created.empty());
    CHECK(second.Dissolved.empty());
    CHECK(second.Mixed.empty());
}

void RosterFiltersByPurposeFactionAndLevel()
{
    std::vector<Guild> guilds = { MakeGuild(600, Purpose::Utility, true), MakeGuild(601, Purpose::Utility, true),
                                  MakeGuild(500, Purpose::Leveling, true), MakeGuild(700, Purpose::Raiding, true),
                                  MakeGuild(602, Purpose::Utility, false) };
    std::vector<Bot> pool = {
        MakeBot(1, true, 600, 60, std::nullopt),          // self
        MakeBot(9, true, 601, 60, std::nullopt),          // utility, other guild, same faction, online, 60: listed
        MakeBot(4, true, 600, 62, std::nullopt),          // utility, above 60: listed
        MakeBot(2, true, 601, 60, std::nullopt, false),   // offline: not
        MakeBot(3, true, 601, 59, std::nullopt),          // below 60: not
        MakeBot(5, true, 500, 60, std::nullopt),          // leveling: not in utility
        MakeBot(6, false, 602, 60, std::nullopt),         // Horde utility: not
        MakeBot(7, true, 700, 60, std::nullopt),          // raiding: not in utility
        MakeBot(8, true, 0, 60, std::nullopt),            // no guild
    };
    MateQuery query;
    query.Kind = Purpose::Utility;
    std::vector<std::uint64_t> mates = GuildMates(Settings{}, guilds, pool, 1, query);
    CHECK(mates.size() == 2 && mates[0] == 4 && mates[1] == 9);

    query.SameGuildOnly = true;
    mates = GuildMates(Settings{}, guilds, pool, 1, query);
    CHECK(mates.size() == 1 && mates[0] == 4);

    query.SameGuildOnly = false;
    query.Kind = Purpose::Leveling;
    query.MinLevel = 1;
    mates = GuildMates(Settings{}, guilds, pool, 1, query);
    CHECK(mates.size() == 1 && mates[0] == 5);

    CHECK(GuildMates(Settings{}, guilds, pool, 999, query).empty());   // unknown self
}

void RosterRaidReady()
{
    std::vector<Guild> guilds = { MakeGuild(700, Purpose::Raiding, true) };
    Settings settings;
    settings.RaidQualified = [](Bot const& bot) { return bot.Guid != 3; };
    std::vector<Bot> pool = { MakeBot(1, true, 700, 60, std::nullopt), MakeBot(2, true, 700, 60, std::nullopt),
                              MakeBot(3, true, 700, 60, std::nullopt), MakeBot(4, true, 700, 59, std::nullopt) };
    MateQuery query;
    query.Kind = Purpose::Raiding;
    CHECK(GuildMates(settings, guilds, pool, 1, query).size() == 2);   // 2 and 3
    query.RaidReadyOnly = true;
    std::vector<std::uint64_t> ready = GuildMates(settings, guilds, pool, 1, query);
    CHECK(ready.size() == 1 && ready[0] == 2);                         // 3 lacks the attunement, 4 is below the cap
}

}  // namespace

int main()
{
    JoinLevelingFromThePool();
    LeaveLevelingAtTheCap();
    EmptyLevelingGuildIsDissolved();
    ReassignedLevelingBotMovesToUtility();
    UtilityStaysPut();
    RaidingNeedsQualification();
    QualifiedLevelingBotMovesStraightToRaiding();
    UnqualifiedLevelingBotLeavesAndWaits();
    FactionsDoNotShareAndMixedGuildsAreLeftAlone();
    ExternalGuildIsNotTouched();
    SizeAndNames();
    IdempotentRerunAfterFullScenario();
    RosterFiltersByPurposeFactionAndLevel();
    RosterRaidReady();
    if (failures == 0)
        std::printf("bot_guilds_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
