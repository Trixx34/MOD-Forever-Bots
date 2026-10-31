// SPDX-License-Identifier: GPL-2.0-or-later
// Shared line-tokenizer for the module's whitespace-separated plain-text data files (rotations, grind
// spots, tamed-beast lists): strips a trailing "# ..." comment, splits the rest on whitespace, and skips
// blank/comment-only lines. combat_rotation.cpp, grind_data.cpp and tame_beasts.cpp each had their own
// copy of this preamble; it is now one place so the line-counting and comment rule can't drift between
// them.
#pragma once

#include <istream>
#include <sstream>
#include <string>
#include <vector>

namespace text_lines {

// Advances `lineNo` (every physical line read, including skipped ones) and fills `tokens` with the next
// non-blank line's whitespace-separated tokens. Returns false (tokens left empty) once `in` is exhausted.
inline bool NextTokens(std::istream& in, std::size_t& lineNo, std::vector<std::string>& tokens)
{
    std::string line;
    while (std::getline(in, line))
    {
        ++lineNo;
        if (auto const hash = line.find('#'); hash != std::string::npos)
            line.erase(hash);
        std::istringstream words(line);
        tokens.clear();
        for (std::string token; words >> token;)
            tokens.push_back(token);
        if (!tokens.empty())
            return true;
    }
    tokens.clear();
    return false;
}

} // namespace text_lines
