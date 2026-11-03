// SPDX-License-Identifier: GPL-2.0-or-later
// Batched auction listing (config AhBatchIntervalHours, 0 = off): bots visit the auction house to relist or cancel only once per interval, and list goods in
// stacks of 1, 5 or 20 chosen per item. With the key off the existing rules (auction_policy.h) are unchanged. Pure logic, tests/ah_batch_test.cpp.
#pragma once

#include <cstdint>

namespace ah_batch {

constexpr std::uint32_t kStackSizes[3] = { 1, 5, 20 };

// The stack size for an item kind (the same for the same item on every visit), at most `have` and at most `cap`; 0 when have is 0. With the feature off
// (intervalHours 0) it returns min(have, cap), the old behaviour.
std::uint32_t StackSize(std::uint32_t intervalHours, std::uint32_t itemEntry, std::uint32_t have, std::uint32_t cap);

// The least time between two visits: the interval when the feature is on, else the old per-bot period.
std::uint64_t VisitEveryMs(std::uint32_t intervalHours, std::uint64_t oldEveryMs);

}  // namespace ah_batch
