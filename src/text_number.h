// Shared single-token numeric parsing for the module's plain-text data files (rotations, grind lists,
// tamed-beast lists, dungeon events, ground-avoid tables). Every one of these formats is whitespace-split
// into tokens first, then each token is parsed as a whole number or a decimal: this header is the one
// place that logic lives, so a parsing bug (or a format change) is fixed once instead of per file.
//
// Both functions require the ENTIRE token to be consumed: "12abc" is not a number, it is a typo. A few
// call sites used to tolerate trailing garbage (or route through std::stof's exceptions); that was an
// inconsistency across files rather than a deliberate choice, so it is not preserved here.
#pragma once

#include <charconv>
#include <cstdint>
#include <string>

namespace text_number {

// Parses a whole non-negative number (no sign, no fraction, no exponent) that uses every character of `text`.
inline bool ParseWhole(std::string const& text, std::uint32_t& out)
{
    if (text.empty())
        return false;
    auto const result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

// Parses a decimal number (sign, fraction and exponent all allowed by std::from_chars) that uses every
// character of `text`.
inline bool ParseDecimal(std::string const& text, float& out)
{
    if (text.empty())
        return false;
    auto const result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

} // namespace text_number
