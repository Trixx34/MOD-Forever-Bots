// SPDX-License-Identifier: GPL-2.0-or-later
// Tests of AuctionErrandPriority (src/errand_policy.h, doc/auction-errand-priority.md): train no longer starves the auction errand when the key is on.
#include "bot_settings.h"
#include "errand_policy.h"
#include "log_throttle.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace errand;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

constexpr std::uint64_t kMin = 60000;

Settings On(std::uint32_t priority)
{
    Settings settings;
    settings.Enabled = true;
    settings.GapMinutes = 0;
    settings.AuctionPriority = priority;
    return settings;
}

// Due both, goods and full bags; training cities {1}, auction cities {2, 3}.
Bot Both(std::uint64_t guid, bool ready = true)
{
    Bot bot;
    bot.Guid = guid;
    bot.Available = true;
    bot.TrainDue = true;
    bot.AuctionDue = ready;
    bot.AuctionReady = ready;
    bot.Cities = { 1 };
    if (ready)
        bot.AuctionCities = { 2, 3 };
    return bot;
}

void KeyOffTrainStaysFirst()
{
    Scheduler scheduler(On(0));
    Decisions const d = scheduler.Tick(0, { Both(10) });
    CHECK(d.Starts.size() == 1 && d.Starts[0].What == Kind::kTrain && d.Starts[0].City == 1);
}

void PriorityOneAuctionFirstToAuctionCity()
{
    Scheduler scheduler(On(1));
    Decisions d = scheduler.Tick(0, { Both(10) });
    CHECK(d.Starts.size() == 1 && d.Starts[0].What == Kind::kAuction && (d.Starts[0].City == 2 || d.Starts[0].City == 3));
    // The next errand is again the auction while the bot stays ready.
    scheduler.Finish(10, kMin);
    d = scheduler.Tick(2 * kMin, { Both(10) });
    CHECK(d.Starts.size() == 1 && d.Starts[0].What == Kind::kAuction);
}

void PriorityTwoAlternates()
{
    Scheduler scheduler(On(2));
    Decisions d = scheduler.Tick(0, { Both(10) });
    CHECK(d.Starts.size() == 1 && d.Starts[0].What == Kind::kTrain && d.Starts[0].City == 1);
    scheduler.Finish(10, kMin);
    d = scheduler.Tick(2 * kMin, { Both(10) });
    CHECK(d.Starts.size() == 1 && d.Starts[0].What == Kind::kAuction);
    scheduler.Finish(10, 3 * kMin);
    d = scheduler.Tick(4 * kMin, { Both(10) });
    CHECK(d.Starts.size() == 1 && d.Starts[0].What == Kind::kTrain);
}

void NotReadyIsTrainingAsBefore()
{
    for (std::uint32_t priority : { 1u, 2u })
    {
        Scheduler scheduler(On(priority));
        Decisions const d = scheduler.Tick(0, { Both(10, false) });
        CHECK(d.Starts.size() == 1 && d.Starts[0].What == Kind::kTrain && d.Starts[0].City == 1);
    }
}

void AuctionOnlyBotUnchanged()
{
    Bot bot;
    bot.Guid = 5;
    bot.Available = true;
    bot.AuctionDue = true;
    bot.Cities = { 4 };
    Scheduler scheduler(On(1));
    Decisions const d = scheduler.Tick(0, { bot });
    CHECK(d.Starts.size() == 1 && d.Starts[0].What == Kind::kAuction && d.Starts[0].City == 4);
}

void FullCityFallsBackNotToTheOtherKind()
{
    Scheduler scheduler(On(1));
    Settings s = On(1);
    s.MaxPerCity = 1;
    scheduler.SetSettings(s);
    Bot a = Both(1), b = Both(2);
    a.AuctionCities = b.AuctionCities = { 2 };
    Decisions const d = scheduler.Tick(0, { a, b });
    CHECK(d.Starts.size() == 1 && d.Starts[0].Guid == 1 && d.Starts[0].What == Kind::kAuction);
}

void BagPercent()
{
    CHECK(BagPct(16, 16) == 0);
    CHECK(BagPct(0, 16) == 100);
    CHECK(BagPct(6, 16) == 62);
    CHECK(BagPct(40, 16) == 0);
    CHECK(BagPct(3, 0) == 0);
}

void SettingKey()
{
    std::vector<std::string> warnings;
    BotSettings const off = ParseBotSettings({}, warnings);
    CHECK(off.Errand.AuctionPriority == 0);
    BotSettings const on = ParseBotSettings({ { "AuctionErrandPriority", "2" } }, warnings);
    CHECK(on.Errand.AuctionPriority == 2);
    std::vector<std::string> bad;
    BotSettings const high = ParseBotSettings({ { "AuctionErrandPriority", "9" } }, bad);
    CHECK(high.Errand.AuctionPriority == 0 && !bad.empty());
}

void DiagnosticLineIsThrottledPerBot()
{
    log_throttle::KeyedThrottle throttle;
    std::uint32_t held = 0;
    int written = 0;
    for (std::uint64_t now = 0; now < 180000; now += 5000)
        written += throttle.Allow(7, "auction_due", now, 60000, held) ? 1 : 0;
    CHECK(written == 3);
    CHECK(throttle.Allow(8, "auction_due", 1000, 60000, held));
}

} // namespace

int main()
{
    KeyOffTrainStaysFirst();
    PriorityOneAuctionFirstToAuctionCity();
    PriorityTwoAlternates();
    NotReadyIsTrainingAsBefore();
    AuctionOnlyBotUnchanged();
    FullCityFallsBackNotToTheOtherKind();
    BagPercent();
    SettingKey();
    DiagnosticLineIsThrottledPerBot();
    if (failures == 0)
        std::printf("auction_errand_priority_test: ok\n");
    return failures == 0 ? 0 : 1;
}
