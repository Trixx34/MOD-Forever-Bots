// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the per-bot randomness helpers: repeatable, spread over the range, and different for different bots, uses and counters.
#include "bot_random.h"

#include <cstdio>
#include <set>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

void SameInputsGiveTheSameDraw()
{
    CHECK(bot_random::Draw(7, 100, bot_random::kSaltParty, 3) == bot_random::Draw(7, 100, bot_random::kSaltParty, 3));
}

void SeedBotSaltAndCounterEachChangeTheDraw()
{
    std::uint64_t const base = bot_random::Draw(7, 100, bot_random::kSaltParty, 3);
    CHECK(base != bot_random::Draw(8, 100, bot_random::kSaltParty, 3));      // another run
    CHECK(base != bot_random::Draw(7, 101, bot_random::kSaltParty, 3));      // another bot
    CHECK(base != bot_random::Draw(7, 100, bot_random::kSaltIdle, 3));       // another use
    CHECK(base != bot_random::Draw(7, 100, bot_random::kSaltParty, 4));      // the next draw of the same use
    CHECK(base != bot_random::Draw(7, 100, bot_random::kSaltParty, 3 + (1ull << 32))); // the high half of the counter counts too
}

void BelowStaysInRangeAndCoversIt()
{
    std::set<std::uint32_t> seen;
    for (std::uint64_t bot = 0; bot < 500; ++bot)
    {
        std::uint32_t const value = bot_random::Below(bot_random::Draw(1, bot, bot_random::kSaltGrindPick), 4);
        CHECK(value < 4);
        seen.insert(value);
    }
    CHECK(seen.size() == 4);
    CHECK(bot_random::Below(123, 0) == 0);
}

void ShareOfBotsIsAboutWhatWasAskedFor()
{
    int chosen = 0;
    for (std::uint64_t bot = 1; bot <= 2000; ++bot)
        chosen += bot_random::Below(bot_random::Draw(42, bot, bot_random::kSaltParty), 100) < 30;
    CHECK(chosen > 500 && chosen < 700);   // 30 % of 2000 is 600
}

void UnitIsBelowOneAndSpread()
{
    double low = 1.0, high = 0.0;
    for (std::uint64_t bot = 0; bot < 1000; ++bot)
    {
        double const value = bot_random::Unit(bot_random::Draw(5, bot, bot_random::kSaltJitter));
        CHECK(value >= 0.0 && value < 1.0);
        low = value < low ? value : low;
        high = value > high ? value : high;
    }
    CHECK(low < 0.05 && high > 0.95);
}

void VaryStaysWithinThePercentAndZeroChangesNothing()
{
    CHECK(bot_random::Vary(30000, 0, 12345) == 30000);
    CHECK(bot_random::Vary(0, 50, 12345) == 0);
    std::uint64_t smallest = ~0ull, largest = 0;
    for (std::uint64_t bot = 0; bot < 1000; ++bot)
    {
        std::uint64_t const value = bot_random::Vary(30000, 50, bot_random::Draw(3, bot, bot_random::kSaltIdle));
        CHECK(value >= 15000 && value <= 45000);
        smallest = value < smallest ? value : smallest;
        largest = value > largest ? value : largest;
    }
    CHECK(smallest < 16500 && largest > 43500);   // the whole range is used
}

} // namespace

int main()
{
    SameInputsGiveTheSameDraw();
    SeedBotSaltAndCounterEachChangeTheDraw();
    BelowStaysInRangeAndCoversIt();
    ShareOfBotsIsAboutWhatWasAskedFor();
    UnitIsBelowOneAndSpread();
    VaryStaysWithinThePercentAndZeroChangesNothing();
    if (failures == 0)
        std::printf("bot_random_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
