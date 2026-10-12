// Store rows for the bots this module controls (doc 15, module store). Each controlled bot has a row bot/<guid>, so a
// restarted module can take its bots back. Pure key handling, unit-tested in tests/bot_store_test.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace bot_store {

inline constexpr char const* KeyPrefix = "bot/";

// The row key for a bot, for example "bot/42".
std::string KeyFor(std::uint64_t botGuid);

// Reads a bot guid back from a row key. False if the key is not a bot row, or the guid is not a positive number.
bool GuidFromKey(std::string const& key, std::uint64_t& botGuid);

} // namespace bot_store
