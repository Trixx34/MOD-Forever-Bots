#include "bot_store.h"

#include <charconv>
#include <system_error>

namespace bot_store {

std::string KeyFor(std::uint64_t botGuid)
{
    return KeyPrefix + std::to_string(botGuid);
}

bool GuidFromKey(std::string const& key, std::uint64_t& botGuid)
{
    std::string const prefix = KeyPrefix;
    if (key.size() <= prefix.size() || key.compare(0, prefix.size(), prefix) != 0)
        return false;

    std::string const digits = key.substr(prefix.size());
    std::uint64_t guid = 0;
    auto const [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), guid);
    if (error != std::errc() || end != digits.data() + digits.size() || guid == 0)
        return false;

    botGuid = guid;
    return true;
}

} // namespace bot_store
