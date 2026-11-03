// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the whisper reply matrix (src/whisper_matrix.h, data/whisper_replies.txt) and its policy step (BotPolicy::ApplyWhisper): off by default,
// the keyword lists, the state pools, no template twice to one player in 10 minutes, and the shared whisper limiter. No core needed.
#include "advisor_text.h"
#include "bot_policy.h"
#include "whisper_matrix.h"

#include <cstdio>

using namespace whisper_matrix;
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

void KeywordsMatchWholeWords()
{
    CHECK(Match("Where are you?") == Keyword::Where);
    CHECK(Match("hey, where r u") == Keyword::Where);
    CHECK(Match("inv pls") == Keyword::Invite);
    CHECK(Match("INV") == Keyword::Invite);
    CHECK(Match("can I join your group") == Keyword::Group);
    CHECK(Match("lfg?") == Keyword::Lfg);
    CHECK(Match("what level are you") == Keyword::Level);
    CHECK(Match("need help") == Keyword::Help);
    CHECK(Match("invasion of the murlocs") == Keyword::None);   // "inv" inside a word is not "inv"
    CHECK(Match("hello there") == Keyword::None);
    CHECK(Match("") == Keyword::None);
}

std::vector<Template> Pool()
{
    return Parse({ "where|grinding|Grinding here.", "where|dungeon|In a dungeon.", "where|any|Somewhere.", "level|any|Level {level}.", "bogus|any|x", "inv|nonsense|x" });
}

void ParseKeepsGoodLinesOnly()
{
    CHECK(Pool().size() == 4);
}

void StatePicksThePool()
{
    Matrix matrix;
    CHECK(matrix.Reply(On(), Pool(), "where are you", State::Grinding, 20, 1, 5, 7, 1000) == "Grinding here.");
    CHECK(matrix.Reply(On(), Pool(), "where are you", State::Dungeon, 20, 2, 5, 7, 1000) == "In a dungeon.");
    CHECK(matrix.Reply(On(), Pool(), "where are you", State::Traveling, 20, 3, 5, 7, 1000) == "Somewhere.");   // the "any" line
    CHECK(matrix.Reply(On(), Pool(), "what level", State::Grinding, 33, 4, 5, 7, 1000) == "Level 33.");
    CHECK(matrix.Reply(On(), Pool(), "hello", State::Grinding, 33, 5, 5, 7, 1000).empty());
    CHECK(matrix.Reply(On(), Pool(), "inv", State::Grinding, 33, 6, 5, 7, 1000).empty());                       // no inv template
}

void OffByDefault()
{
    Matrix matrix;
    CHECK(!Settings{}.Enabled);
    CHECK(matrix.Reply(Settings{}, Pool(), "where are you", State::Grinding, 20, 1, 5, 7, 1000).empty());
}

void NoTemplateTwiceToOnePlayerInTenMinutes()
{
    Matrix matrix;
    std::vector<Template> const pool = Parse({ "where|any|One.", "where|any|Two." });
    std::string const a = matrix.Reply(On(), pool, "where are you", State::Grinding, 1, 9, 5, 7, 1000);
    std::string const b = matrix.Reply(On(), pool, "where are you", State::Grinding, 1, 9, 5, 7, 2000);
    CHECK(!a.empty() && !b.empty() && a != b);
    CHECK(matrix.Reply(On(), pool, "where are you", State::Grinding, 1, 9, 5, 7, 3000).empty());               // both used
    CHECK(!matrix.Reply(On(), pool, "where are you", State::Grinding, 1, 10, 5, 7, 3000).empty());             // another player
    CHECK(!matrix.Reply(On(), pool, "where are you", State::Grinding, 1, 9, 5, 7, 1000 + 10 * 60 * 1000).empty());   // after ten minutes
}

void TheShippedFileParsesAndCoversEveryKeyword()
{
    std::vector<Template> const all = Parse(advisor::LoadLines(std::string(FOREVER_BOTS_SOURCE_DIR) + "/data/whisper_replies.txt"));
    CHECK(all.size() >= 20);
    for (Keyword k : { Keyword::Where, Keyword::Invite, Keyword::Group, Keyword::Lfg, Keyword::Level, Keyword::Help })
    {
        int n = 0;
        for (Template const& t : all)
            n += t.Key == k && t.State_ < 0;
        CHECK(n >= 1);   // an "any" line, so a bot in any state has an answer
    }
    for (Template const& t : all)
        CHECK(t.Text.size() <= 255);
}

BotSnapshot Bot()
{
    BotSnapshot bot;
    bot.guid = 7;
    bot.health = bot.max_health = 1000;
    bot.level = 41;
    bot.map_id = 0;
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

PolicyOptions Options(bool matrix, bool replies)
{
    PolicyOptions options;
    options.WhisperMatrix.Enabled = matrix;
    options.WhisperReplies = replies;
    auto pools = std::make_shared<advisor::Pools>();
    pools->WhisperReplies = { "where|grinding|Grinding here.", "where|any|Somewhere.", "level|any|Level {level}." };
    options.Pools = pools;
    return options;
}

void PolicyAnswersFromTheMatrix()
{
    BotPolicy policy(Options(true, false));
    auto const view = View(Bot());
    auto replies = policy.ApplyWhisper(view, 99, 7, "where are you?", 1000);
    CHECK(replies.size() == 1 && replies[0].Text == "Grinding here." && replies[0].Bot == 7 && replies[0].Player == 99);
    CHECK(policy.ApplyWhisper(view, 99, 7, "hello", 1000).empty());                      // no keyword, and WhisperReplies is off
    CHECK(policy.ApplyWhisper(view, 99, 7, "what level are you", 1000).empty());         // the per (player, bot) cooldown of the shared limiter
    replies = policy.ApplyWhisper(view, 98, 7, "what level are you", 1000);
    CHECK(replies.size() == 1 && replies[0].Text == "Level 41.");
}

void PolicyIsSilentWhenOffAndKeepsTheOldRepliesOtherwise()
{
    BotPolicy off(Options(false, false));
    CHECK(off.ApplyWhisper(View(Bot()), 99, 7, "where are you?", 1000).empty());
    BotPolicy old(Options(false, true));
    CHECK(old.ApplyWhisper(View(Bot()), 99, 7, "where are you?", 1000).size() == 1);   // the existing reply
}

}  // namespace

int main()
{
    KeywordsMatchWholeWords();
    ParseKeepsGoodLinesOnly();
    StatePicksThePool();
    OffByDefault();
    NoTemplateTwiceToOnePlayerInTenMinutes();
    TheShippedFileParsesAndCoversEveryKeyword();
    PolicyAnswersFromTheMatrix();
    PolicyIsSilentWhenOffAndKeepsTheOldRepliesOtherwise();
    if (failures == 0)
        std::printf("whisper_matrix_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
