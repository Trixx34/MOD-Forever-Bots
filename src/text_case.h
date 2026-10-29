// A single lower-casing helper, shared so the module does not keep reimplementing the same
// std::transform/tolower loop (main.cpp's LowerCopy and trade_policy.cpp's Lower were identical).
#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace text_case {

inline std::string ToLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

} // namespace text_case
