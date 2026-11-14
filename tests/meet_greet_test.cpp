// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the meeting greeting (src/meet_greet.h): off by default, needs a quiet free bot in range; per-bot, per-pair and per-map limits; line variety.
#include "meet_greet.h"

#include <cstdio>
#include <set>

using namespace meet_greet;

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

Settings On(std::uint32_t chance = 100) { Settings s; s.Enabled = true; s.ChancePct = chance; return s; }
Facts Good() { Facts f; f.Quiet = true; f.Free = true; f.Distance = 8.0f; return f; }

void Conditions()
{
    Greeting g;
    Greeter gr;
    CHECK(!Settings{}.Enabled);
    CHECK(!gr.Try(Settings{}, Good(), 1, 1, 9, 0, 1000, g));
    Facts f = Good(); f.Quiet = false;   CHECK(!gr.Try(On(), f, 1, 1, 9, 0, 1000, g));
    f = Good(); f.Free = false;          CHECK(!gr.Try(On(), f, 1, 1, 9, 0, 1000, g));
    f = Good(); f.Distance = 16.0f;      CHECK(!gr.Try(On(), f, 1, 1, 9, 0, 1000, g));
    CHECK(!gr.Try(On(0), Good(), 1, 1, 9, 0, 1000, g));
    CHECK(!gr.Try(On(), Good(), 1, 1, 1, 0, 1000, g));   // never itself
    CHECK(gr.Try(On(), Good(), 1, 1, 9, 0, 1000, g));
    CHECK(g.Say || g.Emote);
}

void Limits()
{
    Greeting g;
    Greeter gr;
    CHECK(gr.Try(On(), Good(), 1, 1, 9, 0, 1000, g));
    CHECK(!gr.Try(On(), Good(), 1, 1, 10, 0, 2000, g));                       // same bot within its cooldown
    CHECK(!gr.Try(On(), Good(), 1, 9, 1, 0, 2000, g));                        // the other greets back: same pair
    CHECK(gr.Try(On(), Good(), 1, 1, 10, 0, 1000 + 600 * 1000, g));           // after the bot cooldown, another target
    CHECK(!gr.Try(On(), Good(), 1, 1, 9, 0, 1000 + 1200 * 1000, g));         // the first pair is still cooling
    CHECK(gr.Try(On(), Good(), 1, 1, 9, 0, 1000 + 1800 * 1000 + 1, g));
    Settings s = On(); s.ZoneMax = 1;
    Greeter z;
    CHECK(z.Try(s, Good(), 1, 1, 10, 5, 1000, g));
    CHECK(!z.Try(s, Good(), 1, 2, 11, 5, 1500, g));                           // the map's rate is used up
    CHECK(z.Try(s, Good(), 1, 3, 12, 6, 1500, g));                            // another map
}

void Mix()
{
    Settings s = On(); s.WavePct = 100;
    Greeting g;
    Greeter a;
    CHECK(a.Try(s, Good(), 1, 1, 9, 0, 1000, g) && g.Emote && !g.Say && (g.EmoteKind == 3 || g.EmoteKind == 4));
    s.WavePct = 0; s.SayPct = 100;
    Greeter b;
    CHECK(b.Try(s, Good(), 1, 1, 9, 0, 1000, g) && g.Say && !g.Emote);
    s.SayPct = 0;
    Greeter c;
    CHECK(c.Try(s, Good(), 1, 1, 9, 0, 1000, g) && !g.Say && g.Emote);
}

void Lines()
{
    Greeter gr;
    // One bot with a pool of 20 never repeats within its last 8, however the draws fall.
    std::size_t last[8]; std::size_t n = 0;
    for (std::uint32_t i = 0; i < 200; ++i)
    {
        std::size_t const pick = gr.PickLine(7, 20, (i * 7) % 3);   // a poor draw that keeps asking for the same few lines
        CHECK(pick < 20);
        for (std::size_t k = 0; k < n && k < 8; ++k)
            CHECK(last[k] != pick);
        last[n % 8] = pick; ++n;
        if (n >= 8) { /* window is the last 8 */ }
        if (n > 8) { n = 8; for (int k = 0; k < 7; ++k) last[k] = last[k + 1]; last[7] = pick; }
    }
    // Two bots in turn use the whole pool before a repeat by anyone (24 lines of memory, pool 30).
    Greeter two;
    std::set<std::size_t> seen;
    for (std::uint32_t i = 0; i < 24; ++i)
        seen.insert(two.PickLine(1 + i % 2, 30, 5));
    CHECK(seen.size() == 24);
    // A one-line pool still answers.
    Greeter one;
    CHECK(one.PickLine(1, 1, 99) == 0);
    CHECK(one.PickLine(1, 0, 99) == 0);
}

void Bounded()
{
    Greeting g;
    Greeter gr;
    Settings s = On(); s.ZoneMax = 100000; s.BotCooldownSec = 0; s.PairCooldownSec = 1;
    for (std::uint64_t i = 0; i < 10000; ++i)
        gr.Try(s, Good(), 1, 1 + i, 100000 + i, 0, 1000 + i * 2000, g);
    CHECK(gr.Tracked() < 3 * Greeter::kMaxTracked + 10);
}
}  // namespace

int main()
{
    Conditions();
    Limits();
    Mix();
    Lines();
    Bounded();
    if (failures == 0) std::printf("meet_greet_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
