#include "bot_state_file.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <system_error>

BotStateFile::BotStateFile(std::string path) : _path(std::move(path))
{
}

bool BotStateFile::Load(std::vector<std::string>& warnings, std::string& error)
{
    _guids.clear();

    std::error_code ec;
    if (!std::filesystem::exists(_path, ec))
        return true;

    std::ifstream in(_path);
    if (!in)
    {
        error = "cannot read '" + _path + "'";
        return false;
    }

    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(in, line))
    {
        ++lineNumber;
        if (line.empty())
            continue;

        std::uint64_t guid = 0;
        auto const [end, parse] = std::from_chars(line.data(), line.data() + line.size(), guid);
        if (parse != std::errc() || end != line.data() + line.size() || guid == 0)
        {
            warnings.push_back(_path + " line " + std::to_string(lineNumber) + ": '" + line + "' is not a bot guid, skipped");
            continue;
        }
        if (std::find(_guids.begin(), _guids.end(), guid) == _guids.end())
            _guids.push_back(guid);
    }
    return true;
}

bool BotStateFile::Add(std::uint64_t guid, std::string& error)
{
    if (std::find(_guids.begin(), _guids.end(), guid) == _guids.end())
        _guids.push_back(guid);
    return Save(error);
}

bool BotStateFile::Remove(std::uint64_t guid, std::string& error)
{
    _guids.erase(std::remove(_guids.begin(), _guids.end(), guid), _guids.end());
    return Save(error);
}

bool BotStateFile::Save(std::string& error) const
{
    std::string const temp = _path + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out)
        {
            error = "cannot write '" + temp + "'";
            return false;
        }
        for (std::uint64_t guid : _guids)
            out << guid << '\n';
        out.flush();
        if (!out)
        {
            error = "cannot write '" + temp + "'";
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(temp, _path, ec);
    if (ec)
    {
        error = "cannot replace '" + _path + "': " + ec.message();
        return false;
    }
    return true;
}
