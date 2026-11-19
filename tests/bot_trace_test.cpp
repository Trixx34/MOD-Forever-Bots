// SPDX-License-Identifier: GPL-2.0-or-later
// TraceBots (bot_trace.h): the hostile summary, the state line, and the per-bot files (events, results, actions, state on change and heartbeat, the cap).
#include "bot_trace.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;
using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotActionStatus;
using forever_module::BotSnapshot;
using forever_module::MakeUnitRef;
using forever_module::NearbyUnit;
using forever_module::WorldTickSnapshotView;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

std::string Read(fs::path const& path)
{
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

std::size_t Count(std::string const& text, std::string const& part)
{
    std::size_t n = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1))
        ++n;
    return n;
}

NearbyUnit Mob(std::uint32_t entry, std::uint64_t counter, float x, std::uint32_t level, bool hostile = true, std::uint32_t health = 100)
{
    NearbyUnit unit;
    unit.guid = MakeUnitRef(8, entry, counter);
    unit.pos_x = x;
    unit.level = level;
    unit.health = health;
    unit.max_health = 100;
    unit.is_hostile = hostile;
    return unit;
}

WorldTickSnapshotView OneBot(std::uint64_t guid, std::vector<NearbyUnit> const& nearby)
{
    WorldTickSnapshotView view;
    view.max_nearby_units = 8;
    BotSnapshot bot;
    bot.guid = guid;
    bot.level = 8;
    bot.health = 520;
    bot.max_health = 1000;
    bot.map_id = 0;
    bot.pos_x = -10606.4f;
    bot.pos_y = 294.2f;
    bot.nearby_count = std::uint32_t(nearby.size());
    view.bots.push_back(bot);
    view.nearby.resize(view.max_nearby_units);
    for (std::size_t i = 0; i < nearby.size(); ++i)
    {
        view.nearby[i] = nearby[i];
        view.nearby[i].pos_x += bot.pos_x;   // the units' x is relative to the bot
        view.nearby[i].pos_y += bot.pos_y;
    }
    return view;
}

void HostileSummary()
{
    WorldTickSnapshotView const view = OneBot(235, { Mob(202, 1, 6.0f, 10), Mob(202, 2, 30.0f, 11), Mob(300, 3, 50.0f, 20), Mob(400, 4, 3.0f, 5, false),
                                                     Mob(500, 5, 2.0f, 30, true, 0) });
    bot_trace::Hostiles const h = bot_trace::SummarizeHostiles(view.bots[0], view.NearbyOf(0), 40.0f);
    CHECK(h.Count == 2);           // the 50-yard one, the friendly and the dead one are out
    CHECK(h.MaxLevel == 11);
    CHECK(h.AboveLevel == 1);      // level 11 against a level 8 bot (10 is +2)
    CHECK(h.Nearest == 202 && h.NearestLevel == 10 && int(h.NearestYards) == 6);
    std::string const line = bot_trace::StateLine(view.bots[0], h);
    CHECK(line.rfind("state life=0 hp=52% level=8 map=0 pos=-10606,294,0 combat=0", 0) == 0);
    CHECK(line.find(" hostiles=2 hostile_max=11 above=1 elites=0 nearest=202:10@6") != std::string::npos);
    CHECK(bot_trace::StateLine(view.bots[0], bot_trace::Hostiles{}).find("nearest=none") != std::string::npos);
}

void FilesPerBot()
{
    fs::path const dir = fs::temp_directory_path() / ("bot_trace_test_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    bot_trace::Options options;
    options.Bots = { 235 };
    options.Dir = (dir / "traces").string();   // made when missing
    options.HeartbeatMs = 5000;
    options.Stamp = [] { return std::string("T"); };
    bot_trace::Tracer tracer(options);
    CHECK(tracer.Wants(235) && !tracer.Wants(66));
    CHECK(fs::is_directory(dir / "traces"));

    tracer.Event("died: bot=235 life=1 map=0");
    tracer.Event("died: bot=66 life=1 map=0");   // not traced
    tracer.Event("boss_kill: map=1");             // no bot

    WorldTickSnapshotView view = OneBot(235, { Mob(202, 1, 6.0f, 10) });
    view.bots.push_back(view.bots[0]);
    view.bots[1].guid = 66;
    view.nearby.resize(16);
    view.bots[0].last_action_sequence = 7;
    view.bots[0].last_action_type = std::uint8_t(ActionType::kTeleport);
    view.bots[0].last_action_status = BotActionStatus::kRefused;
    view.bots[0].results[0].sequence = 7;
    view.bots[0].results[0].reason = 3;
    BotAction move;
    move.bot_guid = 235;
    move.type = ActionType::kMoveTo;
    move.dest_x = 1.5f;
    move.dest_y = 2.0f;
    BotAction other = move;
    other.bot_guid = 66;
    tracer.Observe(view, { move, other }, 1000);
    tracer.Observe(view, {}, 2000);            // nothing new: no result again, no state line (unchanged, before the heartbeat)
    tracer.Observe(view, {}, 7000);            // heartbeat: the state again
    view.bots[0].in_combat = 1;
    tracer.Observe(view, {}, 7100);            // changed: the state again
    tracer.Note(235, "note hello");

    std::string const text = Read(tracer.PathOf(235));
    CHECK(text.find("T event died: bot=235 life=1 map=0\n") != std::string::npos);
    CHECK(Count(text, "refused seq=7 ") == 1);
    CHECK(text.find("status=refused reason=3") != std::string::npos);
    CHECK(Count(text, " action type=") == 1);
    CHECK(text.find("action type=" + std::to_string(unsigned(ActionType::kMoveTo)) + " spell=0 target=0 dest=1,2,0") != std::string::npos);
    CHECK(Count(text, " state ") == 3);
    CHECK(text.find("combat=1") != std::string::npos);
    CHECK(text.find("T note hello") != std::string::npos);
    CHECK(!fs::exists(tracer.PathOf(66)));

    // The cap: rotated at half of it, one old file kept.
    bot_trace::Options small = options;
    small.Dir = (dir / "small").string();
    small.MaxBytesPerBot = 8192;
    bot_trace::Tracer capped(small);
    for (int i = 0; i < 2000; ++i)
        capped.Note(235, "note " + std::string(40, 'x'));
    std::uintmax_t const current = fs::file_size(capped.PathOf(235));
    std::uintmax_t const old = fs::file_size(capped.PathOf(235) + ".1");
    CHECK(current <= 4096 && old <= 4096);
    CHECK(!fs::exists(capped.PathOf(235) + ".2"));
    fs::remove_all(dir);
}

void EmptyTracerIsQuiet()
{
    bot_trace::Tracer none(bot_trace::Options{});
    CHECK(none.Empty() && !none.Wants(1));
    none.Event("died: bot=1");
    WorldTickSnapshotView const view = OneBot(1, {});
    none.Observe(view, {}, 0);
}

} // namespace

int main()
{
    HostileSummary();
    FilesPerBot();
    EmptyTracerIsQuiet();
    if (failures == 0)
        std::printf("bot_trace_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
