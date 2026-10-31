// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the bot store row keys. No core needed.
#include "bot_store.h"

#include <cstdio>

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

} // namespace

int main()
{
    RoundTrip();
    RejectsOtherKeys();

    if (failures == 0)
        std::printf("bot_store_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
