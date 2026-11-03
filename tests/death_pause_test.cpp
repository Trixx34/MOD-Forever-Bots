// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the death pause (src/death_pause.h): off by default, the pause lies in the configured range, it is per bot and counts from the death.
#include "death_pause.h"

#include <cstdio>
#include <set>

using namespace death_pause;

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

void OffByDefaultReleasesAtOnce()
{
    CHECK(!Settings{}.Enabled);
    CHECK(ReleaseAllowed(Settings{}, 1, 7, 5000, 5000));
}

void PauseStaysInTheRange()
{
    std::set<std::uint64_t> seen;
    for (std::uint64_t bot = 1; bot <= 200; ++bot)
    {
        std::uint64_t const ms = PauseMs(On(), 42, bot, 1000 * bot);
        CHECK(ms >= 10000 && ms <= 30000);
        seen.insert(ms);
    }
    CHECK(seen.size() > 100);   // bots do not all wait the same time
}

void ReleaseWaitsFromTheDeath()
{
    Settings const settings = On();
    std::uint64_t const death = 100000, pause = PauseMs(settings, 9, 3, death);
    CHECK(!ReleaseAllowed(settings, 9, 3, death, death));
    CHECK(!ReleaseAllowed(settings, 9, 3, death, death + pause - 1));
    CHECK(ReleaseAllowed(settings, 9, 3, death, death + pause));
    CHECK(PauseMs(settings, 9, 3, death) == pause);   // the same for the same death
}

void FixedRange()
{
    Settings settings = On();
    settings.MinSec = settings.MaxSec = 15;
    CHECK(PauseMs(settings, 1, 1, 1) == 15000);
    settings.MaxSec = 5;   // a reversed range is read as MinSec
    CHECK(PauseMs(settings, 1, 1, 1) == 15000);
}

}  // namespace

int main()
{
    OffByDefaultReleasesAtOnce();
    PauseStaysInTheRange();
    ReleaseWaitsFromTheDeath();
    FixedRange();
    if (failures == 0)
        std::printf("death_pause_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
