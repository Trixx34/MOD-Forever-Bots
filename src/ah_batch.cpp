// SPDX-License-Identifier: GPL-2.0-or-later
#include "ah_batch.h"

#include "bot_random.h"

#include <algorithm>

namespace ah_batch {

std::uint32_t StackSize(std::uint32_t intervalHours, std::uint32_t itemEntry, std::uint32_t have, std::uint32_t cap)
{
    std::uint32_t const most = std::min(have, cap);
    if (intervalHours == 0 || most == 0)
        return most;
    std::uint32_t size = kStackSizes[bot_random::Mix(itemEntry) % 3];
    // The largest standard size that fits what the bot has, when the chosen one is too big.
    while (size > most && size > 1)
        size = size == 20 ? 5 : 1;
    return std::min(size, most);
}

std::uint64_t VisitEveryMs(std::uint32_t intervalHours, std::uint64_t oldEveryMs)
{
    return intervalHours == 0 ? oldEveryMs : std::uint64_t(intervalHours) * 3600ull * 1000;
}

}  // namespace ah_batch
