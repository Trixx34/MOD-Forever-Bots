// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the bot store row keys. No core needed.
#include "bot_store.h"

#include <cstdio>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

void RoundTrip()
{
    CHECK(bot_store::KeyFor(42) == "bot/42");
    std::uint64_t guid = 0;
    CHECK(bot_store::GuidFromKey(bot_store::KeyFor(18446744073709551615ull), guid));
    CHECK(guid == 18446744073709551615ull);
}

void RejectsOtherKeys()
{
    std::uint64_t guid = 7;
    CHECK(!bot_store::GuidFromKey("bot/", guid));
    CHECK(!bot_store::GuidFromKey("bot/0", guid));
    CHECK(!bot_store::GuidFromKey("bot/12x", guid));
    CHECK(!bot_store::GuidFromKey("bot/-3", guid));
    CHECK(!bot_store::GuidFromKey("guid/12", guid));
    CHECK(!bot_store::GuidFromKey("", guid));
    CHECK(guid == 7); // unchanged by failed parses
}

void BusyReason()
{
    CHECK(bot_store::IsBusyReason("core is busy, try again later"));
    CHECK(!bot_store::IsBusyReason("character not found"));
    CHECK(!bot_store::IsBusyReason(""));
    CHECK(!bot_store::IsBusyReason("the core is busy"));
}

// 1000 saved bots: never more than the cap in flight, a busy answer goes to the back, every bot is sent in the end.
void ReclaimPaced()
{
    bot_store::ReclaimQueue queue(32);
    for (std::uint64_t guid = 1; guid <= 1000; ++guid)
        queue.Add(guid);
    std::vector<std::uint64_t> first = queue.Take();
    CHECK(first.size() == 32);
    CHECK(first.front() == 1 && first.back() == 32);
    CHECK(queue.InFlight() == 32);
    CHECK(queue.Take().empty()); // the cap is reached: nothing more until answers come
    queue.Retry(1);              // bot 1 was refused "core is busy"
    for (std::size_t i = 1; i < first.size(); ++i)
        queue.Done();
    CHECK(queue.InFlight() == 0);
    CHECK(queue.Waiting() == 1000 - 32 + 1);
    std::size_t sent = 0;
    std::uint64_t last = 0;
    for (;;)
    {
        std::vector<std::uint64_t> batch = queue.Take();
        if (batch.empty())
            break;
        CHECK(batch.size() <= 32);
        sent += batch.size();
        last = batch.back();
        for (std::size_t i = 0; i < batch.size(); ++i)
            queue.Done();
    }
    CHECK(sent == 1000 - 32 + 1);
    CHECK(last == 1); // the retried bot comes after the rest
    CHECK(queue.Waiting() == 0 && queue.InFlight() == 0);
    queue.Done(); // an extra answer does not wrap the count
    CHECK(queue.InFlight() == 0);
    bot_store::ReclaimQueue zero(0); // a cap of 0 is raised to 1
    zero.Add(5);
    CHECK(zero.Take().size() == 1);
}

} // namespace

int main()
{
    RoundTrip();
    RejectsOtherKeys();
    BusyReason();
    ReclaimPaced();

    if (failures == 0)
        std::printf("bot_store_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
