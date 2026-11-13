// What the scenario director tells the policy about a wipe recovery (scenario.cpp UpdateWipeRecovery, BotPolicy::SetWipeHoldFor).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

struct WipeHold
{
    bool Hold = false;                  // members rest and pull nothing
    bool LeaderFree = false;            // the route leader (the tank) may walk on: it goes first, the members follow when Hold ends
    std::uint32_t Epoch = 0;            // counts the backoffs
    std::optional<std::size_t> RewindTo; // after a backoff: the route point the leader is set back to (once per Epoch)
};
