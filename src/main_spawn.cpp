// SPDX-License-Identifier: GPL-2.0-or-later
// main_detail helpers: the clock, the spawn ramp and the scenario spawns (declared in main_internal.h).
#include "main_internal.h"

namespace main_detail {

std::uint64_t nowMs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::uint32_t LocalSecondsOfDay()
{
    std::time_t const now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return static_cast<std::uint32_t>(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
}

forever_module::SpawnParams ParamsFor(RaceClass const& combo)
{
    forever_module::SpawnParams params;
    params.set_race(combo.Race);
    params.set_class_id(combo.Class);
    return params;
}

bool ParseRaceClass(std::string const& value, RaceClass& combo)
{
    auto const space = value.find(' ');
    if (space == std::string::npos)
        return false;
    try
    {
        combo.Race = static_cast<std::uint32_t>(std::stoul(value.substr(0, space)));
        combo.Class = static_cast<std::uint32_t>(std::stoul(value.substr(space + 1)));
    }
    catch (std::exception const&)
    {
        return false;
    }
    return true;
}

void RampTick(RunContext& run)
{
    std::uint64_t const now = nowMs();
    std::uint64_t const elapsed = now - run.StartMs;
    if (RunStopped(run.Settings.Ramp, run.StartSecOfDay, elapsed))
    {
        if (!run.StopLogged)
        {
            run.StopLogged = true;
            std::cerr << "forever-bots: stop time reached after " << elapsed / 60000 << " minutes; no more bots are added\n";
        }
        return;
    }
    if (run.Halted || now < run.BackoffUntilMs)
        return;

    std::uint32_t const target = std::min(RampTarget(run.Settings.Ramp, elapsed), run.Roster.MaxBots());
    // Brackets: the counts of the bots the feed has seen, and each spawn below adds to them, so one tick does not fill one bracket.
    bool const bracketed = run.Settings.Brackets.Enabled && run.Levels;
    brackets::Counts counts{};
    if (bracketed)
    {
        std::lock_guard<std::mutex> lock(run.Levels->Lock);
        for (auto const& [guid, level] : run.Levels->Levels)
            ++counts[brackets::BracketOf(level)];
    }
    std::uint32_t spawned = 0;
    while (spawned < kSpawnsPerRampTick && run.Roster.SpawnedCount() + run.Roster.PendingCount() < target &&
           run.Roster.CanRequest())
    {
        RaceClass combo{};
        std::string reason;
        if (!pick::Pick(run.Rng, run.Settings.Race, run.Settings.Class, combo, reason))
        {
            run.Halted = true;
            std::cerr << "forever-bots: " << reason << "; no bots are spawned\n";
            return;
        }

        forever_module::SpawnParams params = ParamsFor(combo);
        if (bracketed)
        {
            std::size_t const bracket = brackets::NextBracket(run.Settings.Brackets.Targets, counts);
            if (bracket < brackets::kCount)
            {
                params.set_level(brackets::LevelIn(bracket, double(run.Rng.Below(1000000)) / 1000000.0));
                ++counts[bracket];
            }
        }

        ++spawned;
        run.Roster.NoteRequestSent(0);
        run.Client.RequestControl(0, params, [&run, combo](forever_module::ControlResponse const& reply)
        {
            if (!reply.granted())
            {
                run.Roster.OnDenied(0);
                run.BackoffUntilMs = nowMs() + kSpawnBackoffMs;
                std::cerr << "forever-bots: spawn refused for race " << combo.Race << " class " << combo.Class << ": "
                          << reply.reason() << "; waiting 30 s\n";
                return;
            }

            std::uint64_t const guid = reply.bot_guid();
            run.Roster.OnGranted(0, guid);
            run.Classes.Set(guid, combo.Class);
            run.Races.Set(guid, combo.Race);
            run.Client.StoreSet(bot_store::KeyFor(guid), std::to_string(combo.Race) + " " + std::to_string(combo.Class));
            std::cerr << "forever-bots: spawned bot " << guid << " race " << combo.Race << " class " << combo.Class << "\n";
        });
    }
}

void ScenarioSpawnTick(RunContext& run, std::size_t& spawned)
{
    ScenarioSlot const slot = ScenarioSlotOf(run.Settings.Scenarios, spawned);
    if (!slot.Valid || !run.Roster.CanRequest() || nowMs() < run.BackoffUntilMs)
        return;

    ScenarioSettings const& scenario = run.Settings.Scenarios[slot.Dungeon];
    RaceClass const combo{ slot.Race, slot.Class };
    forever_module::SpawnParams params = ParamsFor(combo);
    params.set_level(scenario.Level);
    if (scenario.HasStaging)
    {
        params.set_place(true);
        params.set_map_id(static_cast<std::uint32_t>(scenario.Staging[0]));
        params.set_pos_x(scenario.Staging[1]);
        params.set_pos_y(scenario.Staging[2]);
        params.set_pos_z(scenario.Staging[3]);
    }
    ++spawned;
    run.Roster.NoteRequestSent(0);
    std::string const spec = slot.Spec;
    run.Client.RequestControl(0, params, [&run, combo, spec, &spawned](forever_module::ControlResponse const& reply)
    {
        if (!reply.granted())
        {
            run.Roster.OnDenied(0);
            run.BackoffUntilMs = nowMs() + kSpawnBackoffMs;
            if (spawned > 0)
                --spawned; // try this roster entry again
            std::cerr << "forever-bots: scenario spawn refused for race " << combo.Race << " class " << combo.Class << ": "
                      << reply.reason() << "; waiting 30 s\n";
            return;
        }
        std::uint64_t const guid = reply.bot_guid();
        run.Roster.OnGranted(0, guid);
        run.Classes.Set(guid, combo.Class);
        run.Races.Set(guid, combo.Race);
        if (!spec.empty())
        {
            if (rotation::SpecRole(combo.Class, spec) != 0)
                run.Specs.Set(guid, spec);
            else
                std::cerr << "forever-bots: scenario bot " << guid << ": class " << combo.Class << " has no spec '" << spec << "'; it plays the default spec\n";
        }
        std::cerr << "forever-bots: scenario bot " << guid << " race " << combo.Race << " class " << combo.Class << (spec.empty() ? "" : " spec " + spec) << "\n";
    });
}

} // namespace main_detail
