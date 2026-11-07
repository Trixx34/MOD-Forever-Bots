// SPDX-License-Identifier: GPL-2.0-or-later
// The cut of the bots into scenario parties, split out of the Tier 2 pump (main_pump.cpp) so the empty-directors guard can be unit-tested
// (tests/party_cut_test.cpp). Pure logic, no core needed.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace party_cut
{

// guids must be sorted. Bot i goes to party i / partySize, the last party taking any overflow. With no director (no scenario configured) the map is
// left empty so the callers answer "no party": directors[0] of an empty vector is a null dereference (the module crashed in a loop on it, 2026-11-04).
inline std::unordered_map<std::uint64_t, std::size_t> PartyOf(std::vector<std::uint64_t> const& guids, std::size_t partySize, std::size_t directorCount)
{
    std::unordered_map<std::uint64_t, std::size_t> partyOf;
    if (directorCount != 0)
        for (std::size_t i = 0; i < guids.size(); ++i)
            partyOf[guids[i]] = std::min(i / partySize, directorCount - 1);
    return partyOf;
}

} // namespace party_cut
