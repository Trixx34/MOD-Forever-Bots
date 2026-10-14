#include "bot_live.h"

#include <cstdio>
#include <iostream>
#include <set>
#include <utility>

using forever_module::BotSnapshot;
using forever_module::NearbyUnit;
using forever_module::WorldTickSnapshotView;

namespace {

// Numbers only go into the file, never text from the game, so there is nothing to escape. Floats get one decimal; NaN and
// infinity (not valid JSON) become 0.
void AppendFloat(std::string& out, float value)
{
    if (!(value == value) || value > 1e9f || value < -1e9f)
        value = 0.0f;
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f", static_cast<double>(value));
    out += buffer;
}

void AppendUint(std::string& out, std::uint64_t value)
{
    out += std::to_string(value);
}

} // namespace

std::string BotLive::Build(WorldTickSnapshotView const& view, std::uint64_t unixMs, std::size_t maxUnits)
{
    std::string out;
    out.reserve(256 + view.bots.size() * 160 + maxUnits * 80);
    out += "{\"time_ms\":";
    AppendUint(out, unixMs);
    out += ",\"tick\":";
    AppendUint(out, view.tick_number);

    out += ",\"bots\":[";
    for (std::size_t i = 0; i < view.bots.size(); ++i)
    {
        BotSnapshot const& b = view.bots[i];
        if (i)
            out += ',';
        out += "{\"guid\":";
        AppendUint(out, b.guid);
        out += ",\"map\":";
        AppendUint(out, b.map_id);
        out += ",\"x\":";
        AppendFloat(out, b.pos_x);
        out += ",\"y\":";
        AppendFloat(out, b.pos_y);
        out += ",\"z\":";
        AppendFloat(out, b.pos_z);
        out += ",\"o\":";
        AppendFloat(out, b.orientation);
        out += ",\"hp\":";
        AppendUint(out, b.health);
        out += ",\"max_hp\":";
        AppendUint(out, b.max_health);
        out += ",\"level\":";
        AppendUint(out, b.level);
        out += ",\"combat\":";
        out += b.in_combat ? "1" : "0";
        out += ",\"target\":";
        AppendUint(out, b.threat_target_guid);
        out += ",\"group\":";
        AppendUint(out, b.group_role);
        out += '}';
    }
    out += "],\"units\":[";

    std::set<std::pair<std::uint32_t, std::uint64_t>> seen; // (map, unit reference): one entry per unit
    std::size_t written = 0;
    for (std::size_t i = 0; i < view.bots.size() && written < maxUnits; ++i)
    {
        std::uint32_t const map = view.bots[i].map_id;
        for (NearbyUnit const& u : view.NearbyOf(i))
        {
            if (written >= maxUnits)
                break;
            if (!seen.emplace(map, u.guid).second)
                continue;
            if (written)
                out += ',';
            out += "{\"guid\":";
            AppendUint(out, u.guid);
            out += ",\"map\":";
            AppendUint(out, map);
            out += ",\"x\":";
            AppendFloat(out, u.pos_x);
            out += ",\"y\":";
            AppendFloat(out, u.pos_y);
            out += ",\"hp\":";
            AppendUint(out, u.health);
            out += ",\"max_hp\":";
            AppendUint(out, u.max_health);
            out += ",\"hostile\":";
            out += u.is_hostile ? "1" : "0";
            out += ",\"level\":";
            AppendUint(out, u.level);
            out += ",\"elite\":";
            AppendUint(out, u.elite);
            out += ",\"casting\":";
            AppendUint(out, u.casting);
            out += '}';
            ++written;
        }
    }
    out += "]}\n";
    return out;
}

bool BotLive::WriteAtomic(std::string const& path, std::string const& text)
{
    std::string const temp = path + ".tmp";
    std::FILE* file = std::fopen(temp.c_str(), "wb");
    if (!file)
        return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    ok = (std::fclose(file) == 0) && ok;
    if (!ok || std::rename(temp.c_str(), path.c_str()) != 0)
    {
        std::remove(temp.c_str());
        return false;
    }
    return true;
}

bool BotLive::Maybe(WorldTickSnapshotView const& view, std::uint64_t nowMs, std::uint64_t unixMs)
{
    if (!Enabled() || (_written && nowMs - _lastMs < _options.EveryMs))
        return false;
    _lastMs = nowMs;
    _written = true;
    if (WriteAtomic(_options.Path, Build(view, unixMs, _options.MaxUnits)))
        return true;
    if (!_failedLogged)
    {
        _failedLogged = true;
        std::cerr << "forever-bots: cannot write the live file " << _options.Path << "; it is skipped until it works\n";
    }
    return false;
}
