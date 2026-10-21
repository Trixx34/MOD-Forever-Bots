#include "mini_json.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace mini_json {

namespace {

constexpr std::size_t kMaxBytes = 1024 * 1024;
constexpr int kMaxDepth = 32;

class Reader
{
public:
    explicit Reader(std::string_view text) : _text(text) {}

    ParseResult Run()
    {
        ParseResult result;
        if (_text.size() > kMaxBytes)
        {
            result.Error = "the file is larger than 1 MB";
            return result;
        }
        // a UTF-8 byte order mark, as some editors write one
        if (_text.size() >= 3 && static_cast<unsigned char>(_text[0]) == 0xEF && static_cast<unsigned char>(_text[1]) == 0xBB && static_cast<unsigned char>(_text[2]) == 0xBF)
            _at = 3;
        SkipSpace();
        if (!ReadValue(result.Root, 0))
        {
            result.Error = _error;
            return result;
        }
        SkipSpace();
        if (_at != _text.size())
        {
            Fail("unexpected text after the end of the value");
            result.Error = _error;
            return result;
        }
        result.Ok = true;
        return result;
    }

private:
    bool Fail(std::string const& why)
    {
        if (_error.empty())
        {
            std::size_t line = 1, column = 1;
            for (std::size_t i = 0; i < _at && i < _text.size(); ++i)
            {
                if (_text[i] == '\n')
                {
                    ++line;
                    column = 1;
                }
                else
                    ++column;
            }
            _error = "line " + std::to_string(line) + " column " + std::to_string(column) + ": " + why;
        }
        return false;
    }

    // Spaces and comments between tokens. Comments are an extension for the strategy files, so that people can explain their edits: "// to the end of the
    // line" and "/* ... */" (an unterminated block comment runs to the end of the text, and the reader then reports the missing value).
    void SkipSpace()
    {
        while (_at < _text.size())
        {
            char const c = _text[_at];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++_at;
            else if (c == '/' && _at + 1 < _text.size() && _text[_at + 1] == '/')
            {
                while (_at < _text.size() && _text[_at] != '\n')
                    ++_at;
            }
            else if (c == '/' && _at + 1 < _text.size() && _text[_at + 1] == '*')
            {
                std::size_t const end = _text.find("*/", _at + 2);
                _at = end == std::string_view::npos ? _text.size() : end + 2;
            }
            else
                break;
        }
    }

    bool Literal(char const* word, Value& out, Value::Kind kind, bool boolean)
    {
        std::string_view const w(word);
        if (_text.substr(_at, w.size()) != w)
            return Fail("unexpected text");
        _at += w.size();
        out.What = kind;
        out.Boolean = boolean;
        return true;
    }

    bool ReadValue(Value& out, int depth)
    {
        if (depth > kMaxDepth)
            return Fail("nested too deeply");
        if (_at >= _text.size())
            return Fail("the text ends where a value is expected");
        char const c = _text[_at];
        if (c == '{')
            return ReadObject(out, depth);
        if (c == '[')
            return ReadArray(out, depth);
        if (c == '"')
        {
            out.What = Value::Kind::String;
            return ReadString(out.Text);
        }
        if (c == 't')
            return Literal("true", out, Value::Kind::Bool, true);
        if (c == 'f')
            return Literal("false", out, Value::Kind::Bool, false);
        if (c == 'n')
            return Literal("null", out, Value::Kind::Null, false);
        if (c == '-' || (c >= '0' && c <= '9'))
            return ReadNumber(out);
        return Fail(std::string("unexpected character '") + c + "'");
    }

    bool ReadNumber(Value& out)
    {
        std::size_t const start = _at;
        if (_text[_at] == '-')
            ++_at;
        std::size_t digits = 0;
        while (_at < _text.size() && _text[_at] >= '0' && _text[_at] <= '9')
            ++_at, ++digits;
        if (digits == 0)
            return Fail("a number needs digits");
        if (digits > 1 && _text[start + (_text[start] == '-' ? 1 : 0)] == '0')
            return Fail("a number does not start with 0 followed by more digits");
        if (_at < _text.size() && _text[_at] == '.')
        {
            ++_at;
            std::size_t fraction = 0;
            while (_at < _text.size() && _text[_at] >= '0' && _text[_at] <= '9')
                ++_at, ++fraction;
            if (fraction == 0)
                return Fail("a number needs digits after the point");
        }
        if (_at < _text.size() && (_text[_at] == 'e' || _text[_at] == 'E'))
        {
            ++_at;
            if (_at < _text.size() && (_text[_at] == '+' || _text[_at] == '-'))
                ++_at;
            std::size_t exponent = 0;
            while (_at < _text.size() && _text[_at] >= '0' && _text[_at] <= '9')
                ++_at, ++exponent;
            if (exponent == 0)
                return Fail("a number needs digits in its exponent");
        }
        out.What = Value::Kind::Number;
        out.Text = std::string(_text.substr(start, _at - start));
        return true;
    }

    static void AppendUtf8(std::string& out, std::uint32_t cp)
    {
        if (cp < 0x80)
            out += char(cp);
        else if (cp < 0x800)
        {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
        else
        {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(std::uint32_t& out)
    {
        if (_at + 4 > _text.size())
            return Fail("a \\u escape needs four hex digits");
        out = 0;
        for (int i = 0; i < 4; ++i)
        {
            char const c = _text[_at + i];
            out <<= 4;
            if (c >= '0' && c <= '9')
                out |= std::uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f')
                out |= std::uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                out |= std::uint32_t(c - 'A' + 10);
            else
                return Fail("a \\u escape needs four hex digits");
        }
        _at += 4;
        return true;
    }

    bool ReadString(std::string& out)
    {
        ++_at; // the opening quote
        out.clear();
        while (_at < _text.size())
        {
            char const c = _text[_at++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
            {
                --_at;
                return Fail("a line break or control character inside a string");
            }
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (_at >= _text.size())
                break;
            char const e = _text[_at++];
            switch (e)
            {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u':
                {
                    std::uint32_t cp = 0;
                    if (!Hex4(cp))
                        return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && _text.substr(_at, 2) == "\\u")
                    {
                        _at += 2;
                        std::uint32_t low = 0;
                        if (!Hex4(low))
                            return false;
                        if (low < 0xDC00 || low > 0xDFFF)
                            return Fail("a broken surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    --_at;
                    return Fail(std::string("unknown escape \\") + e);
            }
        }
        return Fail("a string is not closed");
    }

    bool ReadArray(Value& out, int depth)
    {
        ++_at;
        out.What = Value::Kind::Array;
        SkipSpace();
        if (_at < _text.size() && _text[_at] == ']')
        {
            ++_at;
            return true;
        }
        while (true)
        {
            SkipSpace();
            out.Items.emplace_back();
            if (!ReadValue(out.Items.back(), depth + 1))
                return false;
            SkipSpace();
            if (_at >= _text.size())
                return Fail("an array is not closed");
            if (_text[_at] == ',')
            {
                ++_at;
                continue;
            }
            if (_text[_at] == ']')
            {
                ++_at;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    bool ReadObject(Value& out, int depth)
    {
        ++_at;
        out.What = Value::Kind::Object;
        SkipSpace();
        if (_at < _text.size() && _text[_at] == '}')
        {
            ++_at;
            return true;
        }
        while (true)
        {
            SkipSpace();
            if (_at >= _text.size() || _text[_at] != '"')
                return Fail("expected a key in quotes");
            std::string key;
            if (!ReadString(key))
                return false;
            for (auto const& member : out.Members)
                if (member.first == key)
                    return Fail("the key \"" + key + "\" appears twice in one object");
            SkipSpace();
            if (_at >= _text.size() || _text[_at] != ':')
                return Fail("expected ':' after the key \"" + key + "\"");
            ++_at;
            SkipSpace();
            out.Members.emplace_back(key, Value());
            if (!ReadValue(out.Members.back().second, depth + 1))
                return false;
            SkipSpace();
            if (_at >= _text.size())
                return Fail("an object is not closed");
            if (_text[_at] == ',')
            {
                ++_at;
                continue;
            }
            if (_text[_at] == '}')
            {
                ++_at;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    std::string_view _text;
    std::size_t _at = 0;
    std::string _error;
};

} // namespace

Value const* Value::Find(std::string_view key) const
{
    if (What != Kind::Object)
        return nullptr;
    for (auto const& member : Members)
        if (member.first == key)
            return &member.second;
    return nullptr;
}

ParseResult Parse(std::string_view text)
{
    return Reader(text).Run();
}

bool ToFloat(Value const& value, float& out)
{
    if (!value.IsNumber())
        return false;
    errno = 0;
    char* end = nullptr;
    float const parsed = std::strtof(value.Text.c_str(), &end);
    if (end == value.Text.c_str() || *end != '\0' || !std::isfinite(parsed))
        return false;
    out = parsed;
    return true;
}

bool ToUint(Value const& value, std::uint32_t& out, std::uint32_t max)
{
    if (!value.IsNumber() || value.Text.empty() || value.Text[0] == '-')
        return false;
    for (char c : value.Text)
        if (c < '0' || c > '9')
            return false;
    if (value.Text.size() > 10)
        return false;
    unsigned long long const parsed = std::strtoull(value.Text.c_str(), nullptr, 10);
    if (parsed > max)
        return false;
    out = static_cast<std::uint32_t>(parsed);
    return true;
}

} // namespace mini_json
