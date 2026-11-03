// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the Trade-chat crafting ads (src/trade_ads.h, data/trade_ads.txt) and their policy step: off by default, the skill threshold, the
// interval, the placeholders, the shipped template file, and that no ad is posted in combat. No core needed.
#include "advisor_text.h"
#include "bot_policy.h"
#include "trade_ads.h"

#include <cstdio>
#include <fstream>
#include <set>

using namespace trade_ads;
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

Settings On()
{
    Settings settings;
    settings.Enabled = true;
    return settings;
}

std::vector<std::string> const kTemplates = { "{profession} here, {item} on offer.", "Need {item}? ({profession})" };
constexpr std::uint64_t kMin = 60000;

void OffByDefault()
{
    State state;
    std::uint32_t lines[2] = { 164, 0 }, values[2] = { 300, 0 };
    CHECK(!Settings{}.Enabled);
    for (std::uint64_t t = 0; t < 200 * kMin; t += kMin)
        CHECK(Next(Settings{}, state, kTemplates, lines, values, 1, 1, t).empty());
}

void NeedsACraftingSkillAtTheThreshold()
{
    State state;
    std::uint32_t lines[2] = { 164, 186 }, values[2] = { 149, 300 };   // blacksmithing 149; 186 (mining) is gathering
    for (std::uint64_t t = 0; t < 300 * kMin; t += kMin)
        CHECK(Next(On(), state, kTemplates, lines, values, 1, 1, t).empty());
    values[0] = 150;
    int posted = 0;
    for (std::uint64_t t = 300 * kMin; t < 600 * kMin; t += kMin)
        posted += !Next(On(), state, kTemplates, lines, values, 1, 1, t).empty();
    CHECK(posted >= 5);
}

void OnePostPerInterval()
{
    State state;
    std::uint32_t lines[2] = { 171, 0 }, values[2] = { 200, 0 };
    std::vector<std::uint64_t> at;
    for (std::uint64_t t = 0; t < 600 * kMin; t += kMin)
        if (!Next(On(), state, kTemplates, lines, values, 5, 9, t).empty())
            at.push_back(t);
    CHECK(at.size() >= 12 && at.size() <= 14);
    for (std::size_t i = 1; i < at.size(); ++i)
        CHECK(at[i] - at[i - 1] >= 45 * kMin);
    CHECK(at.empty() || at[0] <= 46 * kMin);   // the first one comes within one interval
}

void PlaceholdersAreFilledAndLinesAreShort()
{
    State state;
    std::uint32_t lines[2] = { 171, 0 }, values[2] = { 200, 0 };
    std::string line;
    for (std::uint64_t t = 0; t < 100 * kMin && line.empty(); t += kMin)
        line = Next(On(), state, kTemplates, lines, values, 5, 9, t);
    CHECK(!line.empty());
    CHECK(line.find('{') == std::string::npos);
    CHECK(line.find("Alchemy") != std::string::npos);
    CHECK(line.size() <= 255);
}

void BotsDoNotPostTogether()
{
    std::set<std::uint64_t> first;
    std::uint32_t lines[2] = { 171, 0 }, values[2] = { 200, 0 };
    for (std::uint64_t bot = 1; bot <= 30; ++bot)
    {
        State state;
        for (std::uint64_t t = 0; t < 100 * kMin; t += 1000)
            if (!Next(On(), state, kTemplates, lines, values, 5, bot, t).empty())
            {
                first.insert(t);
                break;
            }
    }
    CHECK(first.size() > 15);
}

void TheShippedTemplatesAreUsable()
{
    std::vector<std::string> pool = advisor::LoadLines(std::string(FOREVER_BOTS_SOURCE_DIR) + "/data/trade_ads.txt");
    CHECK(pool.size() >= 8 && pool.size() <= 10);
    for (std::string const& line : pool)
    {
        CHECK(line.size() <= 255);
        CHECK(line.find("{item}") != std::string::npos || line.find("{profession}") != std::string::npos);
    }
}

BotSnapshot Crafter()
{
    BotSnapshot bot;
    bot.guid = 7;
    bot.health = bot.max_health = 1000;
    bot.level = 30;
    bot.profession_skill[0] = 164;
    bot.profession_value[0] = 200;
    return bot;
}

WorldTickSnapshotView View(BotSnapshot bot)
{
    WorldTickSnapshotView view;
    view.max_nearby_units = 4;
    view.bots.push_back(bot);
    view.nearby.resize(4);
    return view;
}

int AdsIn(PolicyOptions options, BotSnapshot bot)
{
    BotPolicy policy(options);
    int ads = 0;
    for (std::uint64_t t = 1000; t < 150 * kMin; t += 5000)
    {
        policy.Decide(View(bot), t);
        for (advisor::Order const& order : policy.TakeAdvisorOrders())
            ads += order.What == advisor::Kind::TradeSay && order.Bot == 7 && !order.Text.empty();
    }
    return ads;
}

void PolicyPostsOnlyWhenOnAndOutOfCombat()
{
    PolicyOptions options;
    options.WanderWhenIdle = false;
    auto pools = std::make_shared<advisor::Pools>();
    pools->TradeAds = kTemplates;
    options.Pools = pools;
    CHECK(AdsIn(options, Crafter()) == 0);               // off
    options.TradeAds = On();
    int const ads = AdsIn(options, Crafter());
    CHECK(ads >= 2 && ads <= 4);                         // about one per 45 minutes over 150 minutes
    BotSnapshot fighting = Crafter();
    fighting.in_combat = 1;
    CHECK(AdsIn(options, fighting) == 0);
    BotSnapshot grouped = Crafter();
    grouped.group_role = 1;
    CHECK(AdsIn(options, grouped) == 0);
}

}  // namespace

int main()
{
    OffByDefault();
    NeedsACraftingSkillAtTheThreshold();
    OnePostPerInterval();
    PlaceholdersAreFilledAndLinesAreShort();
    BotsDoNotPostTogether();
    TheShippedTemplatesAreUsable();
    PolicyPostsOnlyWhenOnAndOutOfCombat();
    if (failures == 0)
        std::printf("trade_ads_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
