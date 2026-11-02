// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for bot_brackets: the level brackets, the targets parser, the choice of the next bracket, the spot cap and the stall watch.
#include "bot_brackets.h"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

#define CHECK(cond)                                                            \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

constexpr std::uint64_t kMinuteMs = 60000;

void TestBrackets()
{
    CHECK(brackets::BracketOf(0) == 0);
    CHECK(brackets::BracketOf(1) == 0);
    CHECK(brackets::BracketOf(9) == 0);
    CHECK(brackets::BracketOf(10) == 1);
    CHECK(brackets::BracketOf(19) == 1);
    CHECK(brackets::BracketOf(50) == 5);
    CHECK(brackets::BracketOf(59) == 5);
    CHECK(brackets::BracketOf(60) == 6);
    CHECK(brackets::BracketOf(80) == 6);

    CHECK(brackets::Label(0) == "1-9");
    CHECK(brackets::Label(4) == "40-49");
    CHECK(brackets::Label(6) == "60");
    CHECK(brackets::LevelIn(0, 0.0) == 1);
    CHECK(brackets::LevelIn(0, 0.999999) == 9);
    CHECK(brackets::LevelIn(2, 0.5) == 25);
    CHECK(brackets::LevelIn(6, 0.3) == 60);
    CHECK(brackets::LevelIn(3, 1.0) == 39); // a draw of 1 is clamped inside the bracket
}

void TestParseTargets()
{
    brackets::Shares shares{};
    std::string error;
    CHECK(brackets::ParseTargets(brackets::kDefaultTargets, shares, error));
    double sum = 0.0;
    for (double const share : shares)
        sum += share;
    CHECK(std::fabs(sum - 1.0) < 1e-9);
    CHECK(shares[0] > shares[6]);

    // Weights need not sum to 100: they are normalized.
    CHECK(brackets::ParseTargets("1-9:1, 10-19:1", shares, error));
    CHECK(std::fabs(shares[0] - 0.5) < 1e-9);
    CHECK(std::fabs(shares[1] - 0.5) < 1e-9);
    CHECK(shares[2] == 0.0);

    brackets::Shares untouched = shares;
    CHECK(!brackets::ParseTargets("1-9:1,1-9:2", shares, error)); // listed twice
    CHECK(!brackets::ParseTargets("70-79:1", shares, error));     // not a bracket
    CHECK(!brackets::ParseTargets("1-9:-1", shares, error));      // negative
    CHECK(!brackets::ParseTargets("1-9:abc", shares, error));     // not a number
    CHECK(!brackets::ParseTargets("1-9", shares, error));         // no weight
    CHECK(!brackets::ParseTargets("1-9:0,60:0", shares, error));  // every weight zero
    CHECK(!brackets::ParseTargets("", shares, error));
    CHECK(shares == untouched); // a failed parse leaves the targets as they were
}

void TestRetire()
{
    brackets::Retire retire = brackets::Retire::kRelease;
    std::string error;
    CHECK(brackets::ParseRetire("off", retire, error) && retire == brackets::Retire::kOff);
    CHECK(brackets::ParseRetire("release", retire, error) && retire == brackets::Retire::kRelease);
    CHECK(!brackets::ParseRetire("retire", retire, error)); // needs a core delete action, which does not exist
    CHECK(!error.empty());
}

void TestNextBracket()
{
    brackets::Shares shares{};
    std::string error;
    brackets::ParseTargets("1-9:1,10-19:1", shares, error); // half and half

    brackets::Counts counts{};
    CHECK(brackets::NextBracket(shares, counts) == 0); // lowest short bracket first

    counts[0] = 1; // 1 of 1: with the bot about to be placed, bracket 1 is short
    CHECK(brackets::NextBracket(shares, counts) == 1);

    counts[1] = 1; // balanced: bracket 0 is again the lowest short one
    CHECK(brackets::NextBracket(shares, counts) == 0);

    // A bracket with a zero share is never chosen, even when empty.
    CHECK(brackets::NextBracket(shares, brackets::Counts{}) != 2);

    brackets::Shares none{};
    CHECK(brackets::NextBracket(none, counts) == brackets::kCount);

    // Over a long run each bracket converges on its share.
    brackets::Counts run{};
    for (int i = 0; i < 1000; ++i)
        ++run[brackets::NextBracket(shares, run)];
    CHECK(run[0] >= 499 && run[0] <= 501);
    CHECK(run[1] >= 499 && run[1] <= 501);
}

void TestWeightedSpot()
{
    std::vector<std::uint32_t> weights = { 1, 3 };
    std::vector<std::uint32_t> load = { 0, 0 };
    CHECK(brackets::WeightedSpot(weights, load, 0, 0.0) == 0u);
    CHECK(brackets::WeightedSpot(weights, load, 0, 0.5) == 1u); // the heavier spot takes three quarters of the draws
    CHECK(brackets::WeightedSpot(weights, load, 0, 0.99) == 1u);

    // A full spot is skipped; when all are full nothing is chosen.
    load = { 2, 0 };
    CHECK(brackets::WeightedSpot(weights, load, 2, 0.0) == 1u);
    load = { 2, 2 };
    CHECK(!brackets::WeightedSpot(weights, load, 2, 0.0).has_value());

    // A weight of 0 counts as 1, so the spot is still reachable.
    std::vector<std::uint32_t> zero = { 0, 0 };
    std::vector<std::uint32_t> noLoad = { 0, 0 };
    CHECK(brackets::WeightedSpot(zero, noLoad, 0, 0.75) == 1u);
}

void TestUnderCap()
{
    std::vector<std::size_t> const spots = { 4, 7, 9 };
    std::vector<std::uint32_t> const load = { 0, 0, 0, 0, 3, 1, 0, 0, 0, 2 };
    bool fellBack = true;
    // Spot 4 is full at cap 3; spots 7 and 9 are below it.
    auto const open = brackets::UnderCap(spots, load, 3, fellBack);
    CHECK(!fellBack);
    CHECK((open == std::vector<std::size_t>{ 7, 9 }));

    // Cap 0 means no cap at all.
    CHECK(brackets::UnderCap(spots, load, 0, fellBack) == spots);
    CHECK(!fellBack);

    // Every spot full: all are returned, and the caller is told it fell back.
    std::vector<std::uint32_t> full = { 5, 5, 5, 5, 5, 5, 5, 5, 5, 5 };
    auto const everything = brackets::UnderCap(spots, full, 3, fellBack);
    CHECK(fellBack);
    CHECK(everything == spots);
}

void TestLevelWatch()
{
    brackets::LevelWatch watch;
    std::uint64_t const guid = 42;
    CHECK(!watch.Stalled(guid, 10 * kMinuteMs, 30)); // unknown bots are never stalled

    watch.Observe(guid, 12, 0);
    CHECK(!watch.Stalled(guid, 29 * kMinuteMs, 30));
    CHECK(watch.Stalled(guid, 30 * kMinuteMs, 30));

    watch.Observe(guid, 12, 20 * kMinuteMs); // same level: the clock keeps running
    CHECK(watch.Stalled(guid, 30 * kMinuteMs, 30));

    watch.Observe(guid, 13, 25 * kMinuteMs); // a level gained: the clock restarts
    CHECK(!watch.Stalled(guid, 30 * kMinuteMs, 30));

    CHECK(!watch.Stalled(guid, 100 * kMinuteMs, 0)); // stall minutes 0 = off

    // A move down is allowed once, then again after a period.
    CHECK(watch.MayMoveDown(guid, 60 * kMinuteMs, 20 * kMinuteMs));
    watch.NoteMoveDown(guid, 60 * kMinuteMs);
    CHECK(!watch.MayMoveDown(guid, 70 * kMinuteMs, 20 * kMinuteMs));
    CHECK(watch.MayMoveDown(guid, 80 * kMinuteMs, 20 * kMinuteMs));
    // A bot moved down gets a full period before it counts as stalled again.
    CHECK(!watch.Stalled(guid, 70 * kMinuteMs, 30));

    watch.Forget(guid);
    CHECK(!watch.MayMoveDown(guid, 200 * kMinuteMs, 20 * kMinuteMs));
}

} // namespace

int main()
{
    TestBrackets();
    TestParseTargets();
    TestRetire();
    TestNextBracket();
    TestWeightedSpot();
    TestUnderCap();
    TestLevelWatch();
    if (failures == 0)
        std::printf("bracket_test: all checks passed\n");
    return failures;
}
