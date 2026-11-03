// SPDX-License-Identifier: GPL-2.0-or-later
#include "advisor_text.h"

#include <fstream>

namespace advisor {

std::vector<std::string> ParseLines(std::istream& in)
{
    std::vector<std::string> out;
    std::string line;
    while (std::getline(in, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        std::size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
            ++start;
        line.erase(0, start);
        if (line.empty() || line[0] == '#' || line.size() > 255)
            continue;
        out.push_back(line);
    }
    return out;
}

std::vector<std::string> LoadLines(std::string const& path)
{
    std::ifstream file(path);
    return file ? ParseLines(file) : std::vector<std::string>{};
}

std::string Replace(std::string text, std::string const& key, std::string const& value)
{
    std::string const token = "{" + key + "}";
    for (std::size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + value.size()))
        text.replace(at, token.size(), value);
    return text;
}

}  // namespace advisor
