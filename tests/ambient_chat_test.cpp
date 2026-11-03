// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the ambient chat lines (src/ambient_chat.h). No core or SDK needed.
#include "ambient_chat.h"

#include <cstdio>
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

int main()
{
    ParsesFieldsAndSkipsCommentsAndBadLines();
    FillsTokensAndRefusesMissingOnes();
    ZoneAndClassNames();
    SpeakerRespectsCooldownRateAndEvents();
    if (failures == 0)
        std::printf("ambient_chat_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
