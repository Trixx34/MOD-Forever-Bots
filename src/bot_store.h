// SPDX-License-Identifier: GPL-2.0-or-later
// Store rows for the bots this module controls (doc 15, module store). Each controlled bot has a row bot/<guid>, so a
// restarted module can take its bots back. Pure key handling, unit-tested in tests/bot_store_test.cpp.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace bot_store {

inline constexpr char const* KeyPrefix = "bot/";

// The row key for a bot, for example "bot/42".
std::string KeyFor(std::uint64_t botGuid);

// Reads a bot guid back from a row key. False if the key is not a bot row, or the guid is not a positive number.
bool GuidFromKey(std::string const& key, std::uint64_t& botGuid);

// Whether a refused request was refused only because the core's queue was full ("core is busy, try again later"): such a request is sent again
// later, it is not an answer about the bot.
bool IsBusyReason(std::string const& reason);

// The saved bots a restarted module takes back, sent a few at a time. The core holds at most 256 waiting actions and 256 waiting control
// requests and refuses the rest with "core is busy"; the reclaim used to send a store read and a control request for every saved bot at once
// (1000 on the sim), so 712 of 1000 reads and 488 of 1000 control requests were refused at each sim start (2026-11-14 19:26 and 19:50). A
// refused read left the bot with no class (no rotation: a caster meleed with white hits and never cast), a refused control request left the
// bot offline for the run and the ramp made a new character in its place.
class ReclaimQueue
{
public:
    explicit ReclaimQueue(std::uint32_t maxInFlight) : _maxInFlight(maxInFlight == 0 ? 1 : maxInFlight) {}

    void Add(std::uint64_t botGuid) { _waiting.push_back(botGuid); }
    // The bots to send now: up to the in-flight cap, oldest first. Each one taken counts as in flight until Done or Retry.
    std::vector<std::uint64_t> Take();
    // A sent request has its answer.
    void Done();
    // A sent request came back "core is busy": the bot goes to the back of the queue, still in flight no more.
    void Retry(std::uint64_t botGuid);

    std::size_t Waiting() const { return _waiting.size(); }
    std::uint32_t InFlight() const { return _inFlight; }

private:
    std::deque<std::uint64_t> _waiting;
    std::uint32_t _inFlight = 0;
    std::uint32_t _maxInFlight;
};

} // namespace bot_store
