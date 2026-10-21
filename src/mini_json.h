// A small JSON reader for the dungeon files (doc/dungeon-strategy.md): objects, arrays, strings, numbers, true/false/null. No SDK dependency.
// Numbers keep their text, so a coordinate is turned into a float exactly as the old text files did (strtof of the same characters).
// Limits: 1 MB of text, 32 levels of nesting. An error carries the line and column.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mini_json {

struct Value
{
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };
    Kind What = Kind::Null;
    bool Boolean = false;
    std::string Text;                                  // Number: the characters of the number; String: the decoded text
    std::vector<Value> Items;                          // Array
    std::vector<std::pair<std::string, Value>> Members; // Object, in file order

    bool IsObject() const { return What == Kind::Object; }
    bool IsArray() const { return What == Kind::Array; }
    bool IsString() const { return What == Kind::String; }
    bool IsNumber() const { return What == Kind::Number; }
    Value const* Find(std::string_view key) const; // Object member, or nullptr
};

struct ParseResult
{
    bool Ok = false;
    Value Root;
    std::string Error; // "line 3 column 7: ..." when not Ok
};

// A duplicate key in an object is an error (a typo that would silently win).
ParseResult Parse(std::string_view text);

// Numbers: false (and `out` untouched) unless the value is a number that fits.
bool ToFloat(Value const& value, float& out);                                    // strtof of the number's own characters
bool ToUint(Value const& value, std::uint32_t& out, std::uint32_t max = 1000000); // a whole number 0..max, no fraction or exponent

} // namespace mini_json
