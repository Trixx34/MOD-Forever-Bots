// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the shared plain-text parsing helpers (text_number.h, text_split.h, text_case.h, text_lines.h).
#include "text_case.h"
#include "text_lines.h"
#include "text_number.h"
#include "text_split.h"

#include <cstdio>
#include <sstream>
#include <string>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

void ParseWholeRequiresTheEntireToken()
{
    std::uint32_t out = 0;
    CHECK(text_number::ParseWhole("42", out) && out == 42);
    CHECK(!text_number::ParseWhole("", out));
    CHECK(!text_number::ParseWhole("12abc", out)); // trailing garbage: not a whole number
    CHECK(!text_number::ParseWhole("-1", out));    // no sign allowed
}

void ParseDecimalRequiresTheEntireToken()
{
    float out = 0.0f;
    CHECK(text_number::ParseDecimal("3.5", out) && out == 3.5f);
    CHECK(text_number::ParseDecimal("-2", out) && out == -2.0f);
    CHECK(!text_number::ParseDecimal("", out));
    CHECK(!text_number::ParseDecimal("1.5x", out)); // trailing garbage: not a decimal
}

void ParseFloatsRejectsTooFewOrTooManyTokens()
{
    float xyz[3] = {};
    CHECK(text_number::ParseFloats(std::string("1 2 3"), xyz, 3));
    CHECK(xyz[0] == 1.0f && xyz[1] == 2.0f && xyz[2] == 3.0f);
    CHECK(!text_number::ParseFloats(std::string("1 2"), xyz, 3));      // too few
    CHECK(!text_number::ParseFloats(std::string("1 2 3 4"), xyz, 3));  // trailing garbage: a typo, not three numbers
}

void SplitKeepsEmptyFields()
{
    auto const parts = text_split::Split("a,,b", ',');
    CHECK(parts.size() == 3);
    CHECK(parts[0] == "a");
    CHECK(parts[1].empty());
    CHECK(parts[2] == "b");
}

void SplitOfTextWithNoSeparatorIsOneField()
{
    auto const parts = text_split::Split("solo", ',');
    CHECK(parts.size() == 1);
    CHECK(parts[0] == "solo");
}

void ToLowerLowersAsciiLettersOnly()
{
    CHECK(text_case::ToLower("Hello World!") == "hello world!");
    CHECK(text_case::ToLower("") == "");
}

void NextTokensSkipsBlankAndCommentLines()
{
    std::istringstream in("# a comment\n\n1 2 3\n   \n4 5 # trailing comment\n");
    std::size_t lineNo = 0;
    std::vector<std::string> tokens;

    CHECK(text_lines::NextTokens(in, lineNo, tokens));
    CHECK(tokens.size() == 3 && tokens[0] == "1" && tokens[1] == "2" && tokens[2] == "3");
    CHECK(lineNo == 3); // comment line, blank line, then the token line

    CHECK(text_lines::NextTokens(in, lineNo, tokens));
    CHECK(tokens.size() == 2 && tokens[0] == "4" && tokens[1] == "5");

    CHECK(!text_lines::NextTokens(in, lineNo, tokens));
    CHECK(tokens.empty());
}

} // namespace

int main()
{
    ParseWholeRequiresTheEntireToken();
    ParseDecimalRequiresTheEntireToken();
    ParseFloatsRejectsTooFewOrTooManyTokens();
    SplitKeepsEmptyFields();
    SplitOfTextWithNoSeparatorIsOneField();
    ToLowerLowersAsciiLettersOnly();
    NextTokensSkipsBlankAndCommentLines();

    if (failures == 0)
        std::printf("text_helpers_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
