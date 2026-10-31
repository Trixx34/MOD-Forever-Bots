// SPDX-License-Identifier: GPL-2.0-or-later
// A single string_view-splitter, shared so the module does not keep reimplementing the same
// find/substr loop for its delimited plain-text data files (crafting_foci.cpp and errand_trainers.cpp
// had byte-for-byte identical copies).
#pragma once

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

} // namespace text_split
