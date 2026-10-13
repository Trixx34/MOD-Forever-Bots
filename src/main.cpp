// forever-bots: asks the core for bots, then drives the ones it controls from Tier 2 snapshots.
//
// Usage: forever-bots <tier1-endpoint>. The core passes the endpoint when it spawns the module.
//
// Two loops run. Run() (the Tier 1 loop) handles control replies, revocations and the spawn ramp on this thread. A pump
// thread reads each new Tier 2 snapshot and publishes the bot actions BotPolicy decides on. The pump touches only the
// Tier 2 region and the ClassTable (locked), so the two loops share no other state.

#include "bot_pick.h"
#include "bot_policy.h"
#include "bot_ramp.h"
#include "bot_roster.h"
#include "bot_settings.h"
#include "bot_store.h"
#include "bot_telemetry.h"
#include "rotating_file.h"

#include "forever_module/module_client.h"
#include "forever_module/tier2_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// Tier 2 bounds asked for at handshake. The core may grant less. Handshake comes before config, so the bot count asked
// for here is the ceiling; MaxBots from config limits how many bots are actually spawned. Every bot may act each tick, so
// the action buffer holds two actions per bot ceiling slot.
constexpr std::uint32_t kRequestedNearbyUnits = 16;
constexpr std::uint32_t kRequestedActions = 2 * kMaxBotsCeiling;

constexpr std::chrono::milliseconds kPumpInterval(5);
constexpr std::uint64_t kPumpReportMs = 60000;
constexpr std::uint64_t kServiceNoticeMs = 600000; // a refused vendor or trainer is logged once per bot per ten minutes

// The spawn ramp. Every kRampInterval the module asks for up to kSpawnsPerRampTick new bots: 2 spawn requests a second at
// the defaults. The core logs bots in at most Bot.Login.MaxPerSecond a second, so a ramp step of 100 bots takes about 50
// seconds to spawn and about 10 seconds more to log in.
constexpr std::chrono::milliseconds kRampInterval(5000);
constexpr std::uint32_t kSpawnsPerRampTick = 10;
constexpr std::uint64_t kSpawnBackoffMs = 30000; // after a refused spawn, wait this long before the next

char const* ActionTypeName(std::uint8_t type)
{
    switch (static_cast<forever_module::ActionType>(type))
    {
        case forever_module::ActionType::kCastSpell: return "cast";
        case forever_module::ActionType::kMoveTo: return "move";
        case forever_module::ActionType::kAttackTarget: return "attack";
        case forever_module::ActionType::kFollow: return "follow";
        case forever_module::ActionType::kStop: return "stop";
        case forever_module::ActionType::kRest: return "rest";
        case forever_module::ActionType::kVendor: return "vendor";
        case forever_module::ActionType::kTrain: return "train";
        case forever_module::ActionType::kGroup: return "group";
    }
    return "action";
}

// Milliseconds on a monotonic clock, for the policy's cooldowns, the fight timer and the ramp.
std::uint64_t nowMs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Seconds since local midnight, for the ramp's stop time.
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

// The state the ramp and the reclaim share. Everything in it is used on the Run() thread only.
struct RunContext
{
    forever_module::ModuleClient& Client;
    BotRoster& Roster;
    pick::ClassTable& Classes;
    BotSettings& Settings; // updated by a config reload
    pick::Stream Rng;
    std::uint64_t StartMs;
    std::uint32_t StartSecOfDay;
    std::uint64_t BackoffUntilMs = 0;
    bool StopLogged = false;
    bool Halted = false; // a fixed race and class with no valid combination: nothing is spawned
};

forever_module::SpawnParams ParamsFor(RaceClass const& combo)
{
    forever_module::SpawnParams params;
    params.set_race(combo.Race);
    params.set_class_id(combo.Class);
    return params;
}

// Stored value of a bot's row: "race class". Old rows hold "1" and carry no race or class.
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

// One ramp step: spawns up to kSpawnsPerRampTick bots when the run's target is above the bots owned or pending. Stops
// for good at the stop time, and backs off for a while after a refused spawn.
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
    std::uint32_t spawned = 0;
    while (spawned < kSpawnsPerRampTick && run.Roster.Owned().size() + run.Roster.PendingCount() < target &&
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

        ++spawned;
        run.Roster.NoteRequestSent(0);
        run.Client.RequestControl(0, ParamsFor(combo), [&run, combo](forever_module::ControlResponse const& reply)
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
            run.Client.StoreSet(bot_store::KeyFor(guid), std::to_string(combo.Race) + " " + std::to_string(combo.Class));
            std::cerr << "forever-bots: spawned bot " << guid << " race " << combo.Race << " class " << combo.Class << "\n";
        });
    }
}

void PumpTier2(forever_module::Tier2Session& session, std::atomic<bool> const& running, PolicyOptions options,
               std::string const& telemetryPath, BotSettings const& settings)
{
    BotPolicy policy(options);
    bool sawSnapshot = false;
    std::uint64_t lastTick = 0;
    // Last action sequence seen per bot, so each result is reported once.
    std::unordered_map<std::uint64_t, std::uint32_t> seenSequence;
    std::unordered_map<std::uint64_t, std::uint64_t> serviceNoticeMs; // bot -> when a service refusal was last logged
    std::unordered_set<std::uint32_t> unknownSpellsReported; // spell ids the core does not know, reported once each

    // Timing of the policy, reported once a minute so the cost at the current bot count is in the log.
    std::uint64_t reportedAt = nowMs();
    std::uint64_t ticks = 0;
    std::uint64_t decideTotalUs = 0;
    std::uint64_t decideMaxUs = 0;
    std::uint64_t actionsTotal = 0;
    std::size_t lastBotCount = 0;

    // The telemetry file is appended to, so a restart adds to the same record. It rotates at TelemetryMaxMB.
    std::unique_ptr<RotatingFile> telemetryFile;
    std::unique_ptr<BotTelemetry> telemetry;
    if (!telemetryPath.empty())
    {
        telemetryFile = std::make_unique<RotatingFile>(telemetryPath,
                                                       std::uint64_t(settings.TelemetryMaxMB) * 1024 * 1024,
                                                       settings.TelemetryKeep);
        if (telemetryFile->IsOpen())
        {
            if (telemetryFile->CurrentBytes() == 0)
                telemetryFile->WriteLine(BotTelemetry::Header());
            BotTelemetry::Options telemetryOptions;
            telemetryOptions.SummaryEveryMs = std::uint64_t(settings.TelemetrySummarySec) * 1000;
            telemetry = std::make_unique<BotTelemetry>(
                [file = telemetryFile.get()](std::string const& line) { file->WriteLine(line); }, telemetryOptions);
            std::cerr << "forever-bots: telemetry to " << telemetryPath << " (summary every " << settings.TelemetrySummarySec
                      << " s, rotated at " << settings.TelemetryMaxMB << " MB, keeping " << settings.TelemetryKeep << ")\n";
        }
        else
        {
            std::cerr << "forever-bots: cannot open telemetry file " << telemetryPath << "; telemetry is off\n";
        }
    }

    while (running)
    {
        forever_module::WorldTickSnapshotView const& view = session.ReadLatestSnapshot();

        // Report what the core did with each action: a refusal is logged with its reason, once per action.
        for (forever_module::BotSnapshot const& bot : view.bots)
        {
            auto seen = seenSequence.find(bot.guid);
            if (bot.last_action_sequence == 0 || (seen != seenSequence.end() && seen->second == bot.last_action_sequence))
                continue;
            seenSequence[bot.guid] = bot.last_action_sequence;
            // A spell id the core does not know is a mistake in the rotation tables: say so once per id.
            if (bot.last_action_status == forever_module::BotActionStatus::kUnknownSpell && bot.last_action_spell != 0)
            {
                if (unknownSpellsReported.insert(bot.last_action_spell).second)
                    std::cerr << "forever-bots: spell " << bot.last_action_spell << " is not in the core's spell table; check the rotation id\n";
                continue;
            }
            if (bot.last_action_status != forever_module::BotActionStatus::kApplied)
            {
                // A vendor or trainer is refused whenever no NPC is in range, the normal state of an idle bot. Those are
                // logged at most once per bot every ten minutes, so 1500 bots do not flood the log.
                bool const service = bot.last_action_type == static_cast<std::uint8_t>(forever_module::ActionType::kVendor) ||
                                     bot.last_action_type == static_cast<std::uint8_t>(forever_module::ActionType::kTrain);
                if (service)
                {
                    std::uint64_t const now = nowMs();
                    auto const last = serviceNoticeMs.find(bot.guid);
                    if (last != serviceNoticeMs.end() && now - last->second < kServiceNoticeMs)
                        continue;
                    serviceNoticeMs[bot.guid] = now;
                }
                std::cerr << "forever-bots: bot " << bot.guid << " " << ActionTypeName(bot.last_action_type)
                          << (bot.last_action_spell ? " spell " + std::to_string(bot.last_action_spell) : std::string())
                          << " not done: " << ActionStatusName(bot.last_action_status) << "\n";
            }
        }

        if (!sawSnapshot || view.tick_number != lastTick)
        {
            sawSnapshot = true;
            lastTick = view.tick_number;

            if (telemetry)
                telemetry->Observe(view, nowMs());

            // One publish per snapshot tick: the core applies the newest action set it sees for each tick.
            auto const started = std::chrono::steady_clock::now();
            std::vector<forever_module::BotAction> actions = policy.Decide(view, nowMs());
            auto const decideUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                 std::chrono::steady_clock::now() - started)
                                                                 .count());
            ++ticks;
            decideTotalUs += decideUs;
            decideMaxUs = std::max(decideMaxUs, decideUs);
            actionsTotal += actions.size();
            lastBotCount = view.bots.size();

            std::string error;
            if (!session.PublishActions(std::move(actions), error))
                std::cerr << "forever-bots: could not publish actions: " << error << "\n";
        }

        std::uint64_t const now = nowMs();
        if (now - reportedAt >= kPumpReportMs && ticks > 0)
        {
            std::cerr << "forever-bots: pump: " << lastBotCount << " bots, decide avg " << decideTotalUs / ticks << " us, max "
                      << decideMaxUs << " us over " << ticks << " ticks, " << actionsTotal / ticks << " actions per tick\n";
            reportedAt = now;
            ticks = 0;
            decideTotalUs = 0;
            decideMaxUs = 0;
            actionsTotal = 0;
        }
        std::this_thread::sleep_for(kPumpInterval);
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: forever-bots <tier1-endpoint>\n";
        return 1;
    }

    forever_module::ModuleManifest manifest;
    manifest.module_name = "forever-bots";
    manifest.module_version = "0.1.0";
    manifest.tier2_participates = true;
    manifest.requested_max_bots = kMaxBotsCeiling;
    manifest.requested_max_nearby_units = kRequestedNearbyUnits;
    manifest.requested_max_actions = kRequestedActions;

    forever_module::ModuleClient client(std::move(manifest));
    if (!client.Connect(argv[1]) || !client.Handshake())
    {
        std::cerr << "forever-bots: could not connect to the core: " << client.rejection_reason() << "\n";
        return 1;
    }

    // Tier 2 is granted at handshake: there is a region only if the core accepted it.
    bool const tier2Granted = !client.tier2_region_name().empty();
    std::uint32_t const grantedBots = client.negotiated_bounds().max_bots;

    // Config is delivered right after the handshake, so the first callback has already run when OnConfig returns. A later
    // re-delivery (a core reload) updates the ramp, the random picks and the cap, on the Run() thread. The telemetry file
    // and the test spell take effect at restart.
    BotSettings settings;
    bool configured = false;
    BotRoster roster(0); // the cap is set once the settings are known
    client.OnConfig([&settings, &configured, &roster, tier2Granted, grantedBots](
                        std::unordered_map<std::string, std::string> const& config)
    {
        std::vector<std::string> warnings;
        BotSettings const parsed = ParseBotSettings(config, warnings);
        for (std::string const& warning : warnings)
            std::cerr << "forever-bots: config: " << warning << "\n";

        if (!configured)
        {
            settings = parsed;
            configured = true;
            return;
        }

        settings.MaxBots = parsed.MaxBots;
        settings.Race = parsed.Race;
        settings.Class = parsed.Class;
        settings.Ramp = parsed.Ramp;
        roster.SetMaxBots(EffectiveMaxBots(parsed.MaxBots, grantedBots, tier2Granted));
        std::cerr << "forever-bots: config reloaded: MaxBots " << roster.MaxBots() << ", ramp " << settings.Ramp.Initial << " +"
                  << settings.Ramp.Step << " every " << settings.Ramp.StepMinutes << " min to " << settings.Ramp.Cap
                  << " (owned " << roster.Owned().size() << ")\n";
    });

    // The bots we may control: MaxBots from config, capped at what the core granted for Tier 2. Without a Tier 2 region
    // there is nothing to drive, so no bots are requested. The core enforces the same bound (ModuleBotOwnership).
    std::uint32_t const maxBots = EffectiveMaxBots(settings.MaxBots, grantedBots, tier2Granted);
    if (maxBots != settings.MaxBots)
    {
        std::cerr << "forever-bots: MaxBots " << settings.MaxBots << " limited to " << maxBots << " (Tier 2 grant "
                  << grantedBots << (tier2Granted ? "" : ", no Tier 2 region") << ")\n";
    }
    roster.SetMaxBots(maxBots);

    pick::ClassTable classes;
    RunContext run{ client, roster, classes, settings, pick::Stream(settings.RandomSeed), nowMs(), LocalSecondsOfDay() };
    std::cerr << "forever-bots: random seed " << settings.RandomSeed << ", race " << settings.Race << ", class "
              << settings.Class << " (0 = random), ramp " << settings.Ramp.Initial << " +" << settings.Ramp.Step << " every "
              << settings.Ramp.StepMinutes << " min to " << settings.Ramp.Cap << ", stop at " << settings.Ramp.StopSecOfDay / 3600
              << ":" << (settings.Ramp.StopSecOfDay / 60) % 60 << " local\n";

    client.OnControlRevoked([&roster](std::uint64_t botGuid, std::string const& reason)
    {
        if (roster.OnRevoked(botGuid, reason))
            std::cerr << "forever-bots: control of bot " << botGuid << " revoked: " << reason << "\n";
    });

    // Bots from last run are rows in the module store (bot/<guid>). Each row's value is "race class", read before the
    // bot is requested so the rotation knows its class. A revoke keeps the row, since the core revokes every bot at
    // shutdown; only "character not found" removes one. Spawning for the rest is the ramp's job.
    auto reclaim = [&run](std::uint64_t guid)
    {
        run.Roster.NoteRequestSent(guid);
        run.Client.StoreGet(bot_store::KeyFor(guid), [&run, guid](forever_module::ActionReply const& stored)
        {
            RaceClass combo{};
            if (stored.found() && ParseRaceClass(stored.value(), combo))
                run.Classes.Set(guid, combo.Class);
            else
                std::cerr << "forever-bots: bot " << guid << " has no saved class; its rotation is off until it is set\n";

            run.Client.RequestControl(guid, {}, [&run, guid](forever_module::ControlResponse const& reply)
            {
                if (reply.granted())
                {
                    run.Roster.OnGranted(guid, reply.bot_guid());
                    std::cerr << "forever-bots: reclaimed bot " << guid << "\n";
                    return;
                }
                run.Roster.OnDenied(guid);
                if (reply.reason() == "character not found")
                    run.Client.StoreDelete(bot_store::KeyFor(guid));
                std::cerr << "forever-bots: could not reclaim bot " << guid << ": " << reply.reason() << "\n";
            });
        });
    };

    client.StoreList(bot_store::KeyPrefix, [&run, &reclaim](forever_module::ActionReply const& reply)
    {
        if (!reply.success())
        {
            std::cerr << "forever-bots: could not read saved bots (" << reply.reason() << "); spawning without reclaim\n";
            return;
        }
        for (int i = 0; i < reply.keys_size(); ++i)
        {
            std::uint64_t guid = 0;
            if (bot_store::GuidFromKey(reply.keys(i), guid) && run.Roster.CanRequest())
                reclaim(guid);
        }
    });

    client.AddTimer(kRampInterval, [&run] { RampTick(run); });

    // The policy's class lookup: a bot's own class, or the configured class (0 = none) for a bot the table does not know.
    PolicyOptions policyOptions;
    policyOptions.TestSpell = settings.TestSpell;
    policyOptions.ClassId = settings.Class;
    policyOptions.ServiceEveryMs = std::uint64_t(settings.ServiceEverySec) * 1000;
    policyOptions.TrainEveryMs = std::uint64_t(settings.TrainEverySec) * 1000;
    policyOptions.PartySize = settings.PartySize;
    policyOptions.PartyCount = settings.PartyCount;
    for (auto const& [mapId, spots] : settings.Hubs)
        for (auto const& spot : spots)
            policyOptions.HubsByMap[mapId].push_back({ spot[0], spot[1], spot[2] });
    policyOptions.ClassOf = [&classes](std::uint64_t botGuid) { return classes.Get(botGuid); };

    // Tier 2 is granted at handshake, so there is a region only if the core accepted it.
    std::atomic<bool> running{true};
    std::thread pump;
    if (forever_module::Tier2Session* session = client.AttachTier2())
        pump = std::thread(PumpTier2, std::ref(*session), std::cref(running), policyOptions, std::cref(settings.TelemetryFile),
                           std::cref(settings));
    else
        std::cerr << "forever-bots: no Tier 2 region (" << client.tier2_error() << "); bots will not be driven\n";

    client.Run();

    running = false;
    if (pump.joinable())
        pump.join();
    return 0;
}
