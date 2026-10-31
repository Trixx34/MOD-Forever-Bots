// SPDX-License-Identifier: GPL-2.0-or-later
#include "rotating_file.h"

#include <system_error>

namespace fs = std::filesystem;

RotatingFile::RotatingFile(fs::path path, std::uint64_t maxBytes, std::uint32_t keep)
    : _path(std::move(path)), _maxBytes(maxBytes), _keep(keep)
{
    Open();
}

void RotatingFile::Open()
{
    std::error_code ec;
    auto const size = fs::file_size(_path, ec);
    _bytes = ec ? 0 : size;
    // Binary, so '\n' is one byte on every platform. In text mode Windows writes CRLF, and the byte count drifts.
    _out.open(_path, std::ios::app | std::ios::binary);
}

void RotatingFile::WriteLine(std::string const& line)
{
    if (!_out)
        return;

    std::uint64_t const needed = line.size() + 1;
    // A line larger than the limit is written anyway, alone in its file, rather than dropped.
    if (_bytes > 0 && _bytes + needed > _maxBytes)
        Rotate();
    if (!_out)
        return;

    _out << line << '\n';
    _out.flush();
    _bytes += needed;
}

void RotatingFile::Rotate()
{
    _out.close();

    std::error_code ec;
    if (_keep == 0)
    {
        fs::remove(_path, ec);
    }
    else
    {
        // Shift the older files up by one, dropping the oldest, then move the current file to .1.
        fs::remove(fs::path(_path.string() + "." + std::to_string(_keep)), ec);
        for (std::uint32_t i = _keep; i > 1; --i)
        {
            fs::path const from(_path.string() + "." + std::to_string(i - 1));
            fs::path const to(_path.string() + "." + std::to_string(i));
            fs::rename(from, to, ec);
        }
        fs::rename(_path, fs::path(_path.string() + ".1"), ec);
    }

    Open();
    _bytes = 0;
}
