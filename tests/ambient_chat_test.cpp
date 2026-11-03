// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the ambient chat lines (src/ambient_chat.h). No core or SDK needed.
#include "ambient_chat.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

std::vector<ambient::LineDef> Parse(std::string const& text, std::size_t& errors)
{
    std::istringstream in(text);
    return ambient::ParseLines(in, errors);
}

void ParsesFieldsAndSkipsCommentsAndBadLines()
{
    std::size_t errors = 0;
    auto const lines = Parse("# a comment\n"
                             "g|0|0|-|Hello {zone}\n"
                             "t|5|10|level|Level {level} a|b|c\n"
                             "x|0|0|-|bad channel\n"
                             "g|9|3|-|max below min\n"
                             "g|0|0|-|\n",
                             errors);
    CHECK(lines.size() == 2);
    CHECK(errors == 3);
    CHECK(lines[0].Channels == ambient::kGeneralBit);
    CHECK(lines[0].Event.empty());
    CHECK(lines[1].Channels == ambient::kTradeBit);
    CHECK(lines[1].Event == "level");
    CHECK(lines[1].MinLevel == 5 && lines[1].MaxLevel == 10);
    CHECK(lines[1].Text == "Level {level} a|b|c"); // the text keeps its own '|'
}

void FillsTokensAndRefusesMissingOnes()
{
    ambient::Context ctx;
    ctx.Level = 12;
    ctx.Class = "Mage";
    ctx.Zone = "Kalimdor";
    CHECK(ambient::Fill("{class} at {level} in {zone}", ctx) == "Mage at 12 in Kalimdor");
    ctx.Zone.clear();
    CHECK(ambient::Fill("in {zone}", ctx).empty());     // no zone known: the line is skipped
    CHECK(ambient::Fill("left over {brace", ctx).empty());
    CHECK(ambient::Fill(std::string(300, 'x'), ctx).empty()); // over the core's 255-byte limit
}

void ZoneAndClassNames()
{
    CHECK(ambient::ZoneName(1) == "Kalimdor");
    CHECK(ambient::ZoneName(999).empty());
    CHECK(ambient::ClassName(8) == "Mage");
    CHECK(ambient::ClassName(6).empty()); // not a class the bots play
}

void SpeakerRespectsCooldownRateAndEvents()
{
    std::size_t errors = 0;
    // three general lines: once two are in the dedupe ring, the third is the only one the minute can use
    auto const lines = Parse("g|0|0|-|Ambient one\n"
                             "g|0|0|-|Ambient two\n"
                             "g|0|0|-|Ambient three\n"
                             "g|0|0|level|Level {level}\n",
                             errors);
    ambient::Limits limits;
    limits.PerMinute = 2;
    limits.BotCooldownMs = 300000;

    ambient::Context ctx;
    ctx.Level = 5;
    ctx.Class = "Mage";
    ctx.Zone = "Kalimdor";

    ambient::Speaker speaker(lines, limits);
    CHECK(speaker.Pick(1, ambient::Channel::General, ctx, 0).has_value());
    CHECK(!speaker.Pick(1, ambient::Channel::General, ctx, 1000).has_value());  // the bot's cooldown
    CHECK(speaker.Pick(2, ambient::Channel::General, ctx, 2000).has_value());   // second line this minute: the cap of 2
    CHECK(!speaker.Pick(3, ambient::Channel::General, ctx, 3000).has_value());  // rate cap hit
    CHECK(speaker.Pick(3, ambient::Channel::General, ctx, 61000).has_value());  // the minute has passed

    // An event line is said once per bot, and an ambient line is never an event line.
    ambient::Speaker events(lines, limits);
    ambient::Context up = ctx;
    up.Event = "level";
    auto const level = events.Pick(7, ambient::Channel::General, up, 0);
    CHECK(level.has_value() && *level == "Level 5");
    CHECK(!events.Pick(7, ambient::Channel::General, up, 400000).has_value());
    auto const ambientLine = events.Pick(9, ambient::Channel::General, ctx, 0);
    CHECK(ambientLine.has_value() && *ambientLine != "Level 5");

    // A General-only line is not said on Trade.
    std::size_t generalErrors = 0;
    ambient::Speaker generalOnly(Parse("g|0|0|-|General only\n", generalErrors), limits);
    CHECK(!generalOnly.Pick(1, ambient::Channel::Trade, ctx, 0).has_value());
}

} // namespace

// Backlog item 7: the boss kill and local death events.
void WorldEventLinesAreSaidOnlyForTheirEvent()
{
    std::size_t errors = 0;
    auto const lines = Parse("g|0|0|-|An ambient line\n"
                             "g|0|0|boss_kill|The boss is down\n"
                             "g|0|0|local_death|Somebody fell\n",
                             errors);
    CHECK(errors == 0 && lines.size() == 3);
    ambient::Limits limits;
    limits.BotCooldownMs = 0;
    limits.PerMinute = 100;
    ambient::Context plain;
    plain.Level = 10;
    ambient::Speaker speaker(lines, limits);
    // An ambient context never says an event line.
    auto const line = speaker.Pick(100, ambient::Channel::General, plain, 0);
    CHECK(line.has_value() && *line == "An ambient line");
    ambient::Context boss = plain;
    boss.Event = ambient::kEventBossKill;
    auto const said = speaker.Pick(1, ambient::Channel::General, boss, 100000);
    CHECK(said.has_value() && *said == "The boss is down");
    ambient::Context death = plain;
    death.Event = ambient::kEventLocalDeath;
    auto const fell = speaker.Pick(2, ambient::Channel::General, death, 101000);
    CHECK(fell.has_value() && *fell == "Somebody fell");
    // The event line is said once per bot and event.
    CHECK(!speaker.Pick(1, ambient::Channel::General, boss, 200000).has_value());
    // With no line for the event, nothing is said.
    ambient::Context other = plain;
    other.Event = "server_notice";
    CHECK(!speaker.Pick(3, ambient::Channel::General, other, 300000).has_value());
}

void ShippedDataHasBossAndDeathLines()
{
    std::ifstream file(std::string(FOREVER_BOTS_DATA_DIR) + "/ambient_lines.txt");
    std::size_t errors = 0;
    auto const lines = ambient::ParseLines(file, errors);
    CHECK(errors == 0);
    int boss = 0, death = 0;
    for (auto const& line : lines)
    {
        boss += line.Event == ambient::kEventBossKill;
        death += line.Event == ambient::kEventLocalDeath;
    }
    CHECK(boss >= 3 && death >= 3);
}

void ADeathIsSeenByOneNearbyBotAtMost()
{
    ambient::DeathWatch watch;
    using S = ambient::LifeSample;
    CHECK(watch.Observe({ S{ 1, 0, 0, false }, S{ 2, 0, 0, false }, S{ 3, 1, 0, false } }).empty());   // first sight: nothing
    // Bot 1 dies: bot 2 (same map, alive, calm) can see it; bot 3 is on another map.
    auto const seen = watch.Observe({ S{ 1, 0, 1, false }, S{ 2, 0, 0, false }, S{ 3, 1, 0, false } });
    CHECK(seen.size() == 1 && seen[0] == 2);
    CHECK(watch.Observe({ S{ 1, 0, 1, false }, S{ 2, 0, 0, false }, S{ 3, 1, 0, false } }).empty());   // still dead: no second line
    // A bot in combat does not comment, and a lone bot has nobody to see it.
    ambient::DeathWatch busy;
    busy.Observe({ S{ 1, 0, 0, false }, S{ 2, 0, 0, true } });
    CHECK(busy.Observe({ S{ 1, 0, 1, false }, S{ 2, 0, 0, true } }).empty());
    ambient::DeathWatch alone;
    alone.Observe({ S{ 1, 0, 0, false } });
    CHECK(alone.Observe({ S{ 1, 0, 1, false } }).empty());
}

void ABossKillIsCountedOnce()
{
    ambient::BossWatch watch;
    CHECK(!watch.Grew(0, 0));      // first sight
    CHECK(!watch.Grew(0, 0));
    CHECK(watch.Grew(0, 1));
    CHECK(!watch.Grew(0, 1));
    CHECK(watch.Grew(0, 2));
    CHECK(!watch.Grew(1, 3));      // another party, first sight
}

int main()
{
    WorldEventLinesAreSaidOnlyForTheirEvent();
    ShippedDataHasBossAndDeathLines();
    ADeathIsSeenByOneNearbyBotAtMost();
    ABossKillIsCountedOnce();
    ParsesFieldsAndSkipsCommentsAndBadLines();
    FillsTokensAndRefusesMissingOnes();
    ZoneAndClassNames();
    SpeakerRespectsCooldownRateAndEvents();
    if (failures == 0)
        std::printf("ambient_chat_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
