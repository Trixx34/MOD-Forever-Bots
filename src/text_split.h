// SPDX-License-Identifier: GPL-2.0-or-later
// A single string_view-splitter, shared so the module does not keep reimplementing the same
// find/substr loop, blank trim and unsigned parse for its delimited plain-text data files (crafting_foci.cpp, errand_trainers.cpp,
// gather_nodes.cpp, bot_brackets.cpp and dungeon_data.cpp had identical copies).
#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace text_split {

// Splits on `separator`, keeping empty fields (so "a,,b" is three fields, the middle one empty).
inline std::vector<std::string_view> Split(std::string_view text, char separator)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true)
    {
        std::size_t const at = text.find(separator, start);
        if (at == std::string_view::npos)
        {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
}

// Strips spaces, tabs and carriage returns from both ends (not newlines, not other whitespace).
inline std::string_view TrimBlanks(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

// A plain decimal number of at most 10 digits that fits 32 bits; anything else (sign, space, empty) is refused and `out` is left alone.
inline bool ParseUint32(std::string_view text, std::uint32_t& out)
{
    if (text.empty() || text.size() > 10)
        return false;
    std::uint64_t value = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + std::uint64_t(c - '0');
    }
    if (value > std::numeric_limits<std::uint32_t>::max())
        return false;
    out = std::uint32_t(value);
    return true;
}

} // namespace text_split
