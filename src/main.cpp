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
#include "bot_live.h"
#include "bot_telemetry.h"
#include "rotating_file.h"
#include "dungeon_data.h"
#include "scenario.h"

#include "forever_module/module_client.h"
#include "forever_module/tier2_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
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
        case forever_module::ActionType::kLoot: return "loot";
        case forever_module::ActionType::kQuest: return "quest";
        case forever_module::ActionType::kTeleport: return "teleport";
        case forever_module::ActionType::kGrant: return "grant";
        case forever_module::ActionType::kAutoGear: return "autogear";
        case forever_module::ActionType::kReleaseSpirit: return "release";
        case forever_module::ActionType::kResurrect: return "resurrect";
        case forever_module::ActionType::kReclaimCorpse: return "reclaim";
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
    pick::ClassTable& Races;   // race of each bot (same table type; the value is a race id), for its faction
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
            run.Races.Set(guid, combo.Race);
            run.Client.StoreSet(bot_store::KeyFor(guid), std::to_string(combo.Race) + " " + std::to_string(combo.Class));
            std::cerr << "forever-bots: spawned bot " << guid << " race " << combo.Race << " class " << combo.Class << "\n";
        });
    }
}

// Issuers of the .autogear command, handed from the Run() thread (where the command arrives) to the pump thread.
struct CommandQueue
{
    std::mutex Lock;
    std::vector<std::uint64_t> AutoGear; // player guids that typed .autogear
    // Chat commands parsed on the Run() thread, applied on the pump thread; and the lines to send back to the speakers.
    struct ChatItem
    {
        BotPolicy::ChatContext Context;
        ChatCommand Command;
    };
    std::vector<ChatItem> Chat;
    std::vector<std::pair<std::uint64_t, std::string>> Replies; // speaker character guid, text
};

// The alts this module controls (characters of players' accounts playing as bots): name (lower case) to bot guid. Read by the pump thread
// (IsAlt) and written on the Run() thread.
struct AltRegistry
{
    std::mutex Lock;
    std::map<std::string, std::uint64_t> ByName;
    std::map<std::uint64_t, std::string> ByGuid;
    bool Has(std::uint64_t guid)
    {
        std::lock_guard<std::mutex> lock(Lock);
        return ByGuid.count(guid) != 0;
    }
};

std::string LowerCopy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

// What the pump needs beyond the policy: the scenario (if one is configured) and the command queue.
struct PumpExtras
{
    std::optional<ScenarioPlan> Scenario;
    std::string ScenarioResultFile;
    std::shared_ptr<CommandQueue> Commands;
};

// The scenario's bot to spawn next, one per tick of the spawn timer, with its level and its staging place.
void ScenarioSpawnTick(RunContext& run, std::size_t& spawned)
{
    ScenarioSettings const& scenario = run.Settings.Scenario;
    if (spawned >= scenario.Roster.size() || !run.Roster.CanRequest() || nowMs() < run.BackoffUntilMs)
        return;

    RaceClass const combo{ scenario.Roster[spawned][0], scenario.Roster[spawned][1] };
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
    run.Client.RequestControl(0, params, [&run, combo, &spawned](forever_module::ControlResponse const& reply)
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
        std::cerr << "forever-bots: scenario bot " << guid << " race " << combo.Race << " class " << combo.Class << "\n";
    });
}

void PumpTier2(forever_module::Tier2Session& session, std::atomic<bool> const& running, PolicyOptions options,
               std::string const& telemetryPath, BotSettings const& settings, PumpExtras extras)
{
    BotPolicy policy(options);
    std::optional<ScenarioDirector> director;
    if (extras.Scenario)
        director.emplace(*extras.Scenario);
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

    // The live picture for the dashboard's map (LiveFile): rewritten in place, so nothing piles up.
    BotLive live(BotLive::Options{ settings.LiveFile, settings.LiveEveryMs, 400 });
    if (live.Enabled())
        std::cerr << "forever-bots: live picture to " << settings.LiveFile << " every " << settings.LiveEveryMs << " ms\n";

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
            if (live.Enabled())
                live.Maybe(view, nowMs(), std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch()).count()));

            // One publish per snapshot tick: the core applies the newest action set it sees for each tick.
            auto const started = std::chrono::steady_clock::now();
            // .autogear commands: the bots led by the player who typed it are dressed at this tick.
            if (extras.Commands)
            {
                std::vector<std::uint64_t> issuers;
                {
                    std::lock_guard<std::mutex> lock(extras.Commands->Lock);
                    issuers.swap(extras.Commands->AutoGear);
                }
                for (std::uint64_t issuer : issuers)
                {
                    std::uint64_t const ref = forever_module::MakeUnitRef(2, 0, issuer & forever_module::kUnitRefCounterMask);
                    for (forever_module::BotSnapshot const& bot : view.bots)
                        if (bot.group_leader_guid == ref)
                            policy.RequestAutoGear(bot.guid);
                }
            }
            // Chat commands: applied before this tick's decisions; the lines for the speaker go back through the Run() thread.
            if (extras.Commands)
            {
                std::vector<CommandQueue::ChatItem> chat;
                {
                    std::lock_guard<std::mutex> lock(extras.Commands->Lock);
                    chat.swap(extras.Commands->Chat);
                }
                for (CommandQueue::ChatItem const& item : chat)
                {
                    std::vector<std::string> const lines = policy.ApplyChat(view, item.Context, item.Command, nowMs());
                    std::lock_guard<std::mutex> lock(extras.Commands->Lock);
                    for (std::string const& line : lines)
                        extras.Commands->Replies.emplace_back(item.Context.Issuer, line);
                }
            }
            if (director)
                policy.SetRouteActive(director->RouteActive());
            std::vector<forever_module::BotAction> actions = policy.Decide(view, nowMs());
            if (director)
            {
                std::size_t const before = actions.size();
                director->Decide(view, nowMs(), actions);
                policy.SetRouteActive(director->RouteActive());

                // The scenario's progress, in the module's log: each phase as it starts, and every teleport the director sends.
                static ScenarioPhase lastPhase = ScenarioPhase::Done;
                static bool phaseLogged = false;
                if (!phaseLogged || director->Phase() != lastPhase)
                {
                    static char const* const names[] = { "waiting for the roster", "dressing", "grouping", "entering", "running", "done" };
                    std::cerr << "forever-bots: scenario " << extras.Scenario->Name << ": phase " << names[int(director->Phase())] << " ("
                              << view.bots.size() << " bots in view)\n";
                    lastPhase = director->Phase();
                    phaseLogged = true;
                }
                for (std::size_t i = before; i < actions.size(); ++i)
                    if (actions[i].type == forever_module::ActionType::kTeleport)
                        std::cerr << "forever-bots: scenario " << extras.Scenario->Name << ": teleport bot " << actions[i].bot_guid << " to map "
                                  << actions[i].spell_id << " (" << actions[i].dest_x << ", " << actions[i].dest_y << ", " << actions[i].dest_z << ")\n";
                for (std::string const& boss : director->TakeNewBossKills())
                    std::cerr << "forever-bots: scenario " << extras.Scenario->Name << ": boss killed: " << boss << "\n";
                if (auto const result = director->TakeResult())
                {
                    std::string const line = FormatScenarioResult(*result);
                    std::cerr << "forever-bots: " << line << "\n";
                    if (!extras.ScenarioResultFile.empty())
                    {
                        std::ofstream out(extras.ScenarioResultFile, std::ios::app);
                        if (out)
                            out << line << "\n";
                    }
                }
            }
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
    manifest.tier1_hooks = { "CommandScript", "PlayerScript.OnChat" };

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
        // A dungeon file fills the scenario keys the config leaves out (dungeon_data.h).
        BotSettings const parsed = ParseBotSettings(MergeDungeonFile(config, warnings), warnings);
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
    pick::ClassTable races;
    RunContext run{ client, roster, classes, races, settings, pick::Stream(settings.RandomSeed), nowMs(), LocalSecondsOfDay() };
    std::cerr << "forever-bots: random seed " << settings.RandomSeed << ", race " << settings.Race << ", class "
              << settings.Class << " (0 = random), ramp " << settings.Ramp.Initial << " +" << settings.Ramp.Step << " every "
              << settings.Ramp.StepMinutes << " min to " << settings.Ramp.Cap << ", stop at " << settings.Ramp.StopSecOfDay / 3600
              << ":" << (settings.Ramp.StopSecOfDay / 60) % 60 << " local\n";

    auto alts = std::make_shared<AltRegistry>();
    client.OnControlRevoked([&roster, alts](std::uint64_t botGuid, std::string const& reason)
    {
        {
            std::lock_guard<std::mutex> lock(alts->Lock);
            auto const it = alts->ByGuid.find(botGuid);
            if (it != alts->ByGuid.end())
            {
                alts->ByName.erase(it->second);
                alts->ByGuid.erase(it);
            }
        }
        if (roster.OnRevoked(botGuid, reason))
            std::cerr << "forever-bots: control of bot " << botGuid << " revoked: " << reason << "\n";
    });

    // .autogear: a player in a party types it (as a chat command) and the bots that player leads are dressed for their level.
    auto commands = std::make_shared<CommandQueue>();
    client.OnRequest("CommandScript", [commands, alts, &run, &settings](forever_module::EventRequest const& request) -> forever_module::EventReply
    {
        forever_module::EventReply reply;
        std::string text = request.command().command_text();
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        // .bots <verb>: the chat commands (chat_commands.h) as a dot command, for the bots of the player's group. goto here, pull and heal need
        // the speaker's position or target, which a dot command does not carry: those are for party chat.
        if (text == "bots" || text.rfind("bots ", 0) == 0)
        {
            std::string const rest = text.size() > 5 ? text.substr(5) : std::string();
            auto const set = [&reply](bool ok, std::string const& message)
            {
                reply.mutable_command()->set_success(ok);
                reply.mutable_command()->set_result_text(message);
            };
            // .bots alt add <name> | alt remove <name> | alt list, .bots spawn <race> <class> [level], .bots list. The answer comes back as a message
            // to the player when the core has decided.
            std::uint64_t const issuerCounter = request.command().issuer_guid() & forever_module::kUnitRefCounterMask;
            auto const words = [&rest]
            {
                std::vector<std::string> out;
                std::istringstream in(rest);
                std::string word;
                while (in >> word)
                    out.push_back(word);
                return out;
            }();
            auto const tell = [&run, issuerCounter](std::string const& text) { run.Client.SendPlayerMessage(issuerCounter, text); };
            if (!words.empty() && words[0] == "alt")
            {
                if (words.size() == 2 && words[1] == "list")
                {
                    std::lock_guard<std::mutex> lock(alts->Lock);
                    std::string names;
                    for (auto const& [name, guid] : alts->ByName)
                        names += (names.empty() ? "" : ", ") + name;
                    set(true, names.empty() ? "No alts are playing as bots." : "Alts as bots: " + names);
                    return reply;
                }
                if (words.size() == 3 && words[1] == "add")
                {
                    std::string const name = words[2];
                    if (!run.Roster.CanRequest())
                    {
                        set(false, "The module cannot take another bot (its bot limit is reached).");
                        return reply;
                    }
                    forever_module::SpawnParams params;
                    params.set_alt_name(name);
                    params.set_owner_guid(issuerCounter);
                    run.Roster.NoteRequestSent(0);
                    run.Client.RequestControl(0, params, [&run, alts, tell, name](forever_module::ControlResponse const& response)
                    {
                        if (!response.granted())
                        {
                            run.Roster.OnDenied(0);
                            tell("Cannot make " + name + " a bot: " + response.reason());
                            return;
                        }
                        std::uint64_t const guid = response.bot_guid();
                        run.Roster.OnGranted(0, guid);
                        if (response.class_id() != 0)
                            run.Classes.Set(guid, response.class_id());
                        if (response.race() != 0)
                            run.Races.Set(guid, response.race());
                        {
                            std::lock_guard<std::mutex> lock(alts->Lock);
                            alts->ByName[LowerCopy(name)] = guid;
                            alts->ByGuid[guid] = LowerCopy(name);
                        }
                        std::cerr << "forever-bots: alt " << name << " is bot " << guid << "\n";
                        tell(name + " is logging in as a bot. Invite it to your group to command it.");
                    });
                    set(true, "Asking the server to log " + name + " in as a bot.");
                    return reply;
                }
                if (words.size() == 3 && words[1] == "remove")
                {
                    std::uint64_t guid = 0;
                    {
                        std::lock_guard<std::mutex> lock(alts->Lock);
                        auto const it = alts->ByName.find(LowerCopy(words[2]));
                        if (it != alts->ByName.end())
                        {
                            guid = it->second;
                            alts->ByGuid.erase(guid);
                            alts->ByName.erase(it);
                        }
                    }
                    if (guid == 0)
                    {
                        set(false, words[2] + " is not an alt playing as a bot.");
                        return reply;
                    }
                    run.Roster.OnRevoked(guid, "alt removed");
                    run.Client.ReleaseControl(guid); // the core logs an alt out when the module lets it go
                    set(true, words[2] + " is being logged out.");
                    return reply;
                }
                set(false, "Usage: .bots alt add <name> | alt remove <name> | alt list");
                return reply;
            }
            if (!words.empty() && words[0] == "spawn")
            {
                std::uint32_t race = 0, cls = 0, level = 0;
                auto const number = [](std::string const& text, std::uint32_t& out)
                {
                    return std::from_chars(text.data(), text.data() + text.size(), out).ec == std::errc{};
                };
                if (words.size() < 3 || words.size() > 4 || !number(words[1], race) || !number(words[2], cls) || (words.size() == 4 && !number(words[3], level)) ||
                    race == 0 || cls == 0)
                {
                    set(false, "Usage: .bots spawn <race id> <class id> [level]");
                    return reply;
                }
                if (!run.Roster.CanRequest())
                {
                    set(false, "The module cannot take another bot (its bot limit is reached).");
                    return reply;
                }
                forever_module::SpawnParams params = ParamsFor(RaceClass{ race, cls });
                if (level != 0)
                    params.set_level(level);
                run.Roster.NoteRequestSent(0);
                run.Client.RequestControl(0, params, [&run, tell, race, cls](forever_module::ControlResponse const& response)
                {
                    if (!response.granted())
                    {
                        run.Roster.OnDenied(0);
                        tell("Spawn refused: " + response.reason());
                        return;
                    }
                    std::uint64_t const guid = response.bot_guid();
                    run.Roster.OnGranted(0, guid);
                    run.Classes.Set(guid, cls);
                    run.Races.Set(guid, race);
                    run.Client.StoreSet(bot_store::KeyFor(guid), std::to_string(race) + " " + std::to_string(cls));
                    tell("Spawned bot " + std::to_string(guid) + " (race " + std::to_string(race) + ", class " + std::to_string(cls) + ").");
                });
                set(true, "Spawning a bot.");
                return reply;
            }
            if (words.size() == 1 && words[0] == "list")
            {
                set(true, "This module controls " + std::to_string(run.Roster.Owned().size()) + " bot(s), " + std::to_string(run.Roster.PendingCount()) +
                              " request(s) pending (limit " + std::to_string(run.Roster.MaxBots()) + ").");
                return reply;
            }
            if (rest == "autogear")
            {
                {
                    std::lock_guard<std::mutex> lock(commands->Lock);
                    commands->AutoGear.push_back(request.command().issuer_guid());
                }
                set(true, "Your party's bots are being dressed for their level.");
                return reply;
            }
            if (!settings.ChatCommands)
            {
                set(false, "Bot commands are off (ChatCommands = 0 in the module's config).");
                return reply;
            }
            auto const parsed = ParseChatCommand(rest, settings.ChatOrders, settings.ChatControl);
            if (!parsed)
            {
                set(false, "Usage: .bots [all|tank|healer|dps|<class>] follow [off] | stay [off] | release | status | autogear" +
                               std::string(settings.ChatOrders ? " | stop | passive | aggressive | summon | revive | mount | dismount" : "") +
                               std::string(settings.ChatControl ? " | role <tank|healer|dps|auto> | stance <aggressive|defensive|passive> | distance <yards>|default" : "") +
                               ". goto here, pull, heal and focus: say them in party chat.");
                return reply;
            }
            if (parsed->Verb == ChatVerb::GotoHere || parsed->Verb == ChatVerb::Pull || parsed->Verb == ChatVerb::Heal ||
                parsed->Verb == ChatVerb::Focus)
            {
                set(false, std::string(ChatVerbName(parsed->Verb)) + " needs your position or target: say it in party chat.");
                return reply;
            }
            CommandQueue::ChatItem item;
            item.Command = *parsed;
            item.Context.Issuer = request.command().issuer_guid() & forever_module::kUnitRefCounterMask;
            {
                std::lock_guard<std::mutex> lock(commands->Lock);
                if (commands->Chat.size() < 64)
                    commands->Chat.push_back(std::move(item));
            }
            set(true, "Sent to your group's bots.");
            return reply;
        }
        if (text != "autogear")
        {
            reply.mutable_command()->set_success(false); // not ours: no text, so the core tries the next module
            return reply;
        }
        {
            std::lock_guard<std::mutex> lock(commands->Lock);
            commands->AutoGear.push_back(request.command().issuer_guid());
        }
        reply.mutable_command()->set_success(true);
        reply.mutable_command()->set_result_text("Your party's bots are being dressed for their level.");
        return reply;
    });

    // Chat commands (chat_commands.h): a group leader's party or raid line, or a whisper to a bot. Parsed here, applied on the pump thread.
    client.OnNotify("PlayerScript.OnChat", [commands, &settings](forever_module::EventNotify const& notify)
    {
        if (!settings.ChatCommands || notify.event_payload_case() != forever_module::EventNotify::kPlayerChat)
            return;
        forever_module::PlayerChatEvent const& chat = notify.player_chat();
        auto const parsed = ParseChatCommand(chat.text(), settings.ChatOrders, settings.ChatControl);
        if (!parsed)
            return; // ordinary chat
        CommandQueue::ChatItem item;
        item.Command = *parsed;
        item.Context.Issuer = chat.player_guid();
        item.Context.Recipient = chat.channel() == 3 ? chat.recipient_guid() : 0;
        item.Context.MapId = chat.map_id();
        item.Context.Pos = Point{ chat.pos_x(), chat.pos_y(), chat.pos_z() };
        item.Context.TargetRef = chat.target_ref();
        std::lock_guard<std::mutex> lock(commands->Lock);
        if (commands->Chat.size() < 64)
            commands->Chat.push_back(std::move(item));
    });
    // The lines the pump thread has for speakers are sent from this thread (client calls belong to it).
    client.AddTimer(std::chrono::milliseconds(500), [commands, &client]
    {
        std::vector<std::pair<std::uint64_t, std::string>> replies;
        {
            std::lock_guard<std::mutex> lock(commands->Lock);
            replies.swap(commands->Replies);
        }
        for (auto const& [player, text] : replies)
            client.SendPlayerMessage(player, text);
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
                run.Classes.Set(guid, combo.Class), run.Races.Set(guid, combo.Race);
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

    bool const scenarioOn = !settings.Scenario.Name.empty();
    std::size_t scenarioSpawned = 0;
    if (scenarioOn)
    {
        roster.SetMaxBots(std::min<std::uint32_t>(std::uint32_t(settings.Scenario.Roster.size()), maxBots));
        std::cerr << "forever-bots: scenario " << settings.Scenario.Name << ": " << settings.Scenario.Roster.size() << " bots at level "
                  << settings.Scenario.Level << ", route of " << settings.Scenario.Route.size() << " point(s), "
                  << settings.Scenario.DurationSec << " s\n";
        client.AddTimer(std::chrono::milliseconds(3000), [&run, &scenarioSpawned] { ScenarioSpawnTick(run, scenarioSpawned); });
    }

    if (!scenarioOn)
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

    if (!scenarioOn)
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
    if (!settings.QuestFile.empty())
    {
        std::vector<std::string> questWarnings;
        auto quests = std::make_shared<QuestData>(LoadQuestData(settings.QuestFile, questWarnings));
        for (std::string const& warning : questWarnings)
            std::cerr << "forever-bots: " << warning << "\n";
        std::cerr << "forever-bots: quests: " << quests->Starts.size() << " start(s) from " << settings.QuestFile << "\n";
        policyOptions.Quests = std::move(quests);
    }
    policyOptions.QuestTeleport = settings.QuestTeleport;
    if (!settings.RotationFile.empty())
    {
        std::vector<std::string> rotationWarnings;
        auto rotations = std::make_shared<rotation::RotationData>(rotation::LoadRotationData(settings.RotationFile, rotationWarnings));
        for (std::string const& warning : rotationWarnings)
            std::cerr << "forever-bots: " << warning << "\n";
        std::cerr << "forever-bots: rotations for " << rotations->ByClass.size() << " class(es) from " << settings.RotationFile << "\n";
        policyOptions.Rotations = std::move(rotations);
    }
    if (!settings.GrindFile.empty())
    {
        std::vector<std::string> grindWarnings;
        auto grind = std::make_shared<GrindData>(LoadGrindData(settings.GrindFile, grindWarnings));
        for (std::string const& warning : grindWarnings)
            std::cerr << "forever-bots: " << warning << "\n";
        std::cerr << "forever-bots: grind spots: " << grind->Spots.size() << " from " << settings.GrindFile << "\n";
        policyOptions.Grind = std::move(grind);
    }
    policyOptions.RaceOf = [&races](std::uint64_t botGuid) { return races.Get(botGuid); };
    policyOptions.SideOf = [&races](std::uint64_t botGuid) { return SideOfRace(races.Get(botGuid)); };
    policyOptions.ClassOf = [&classes](std::uint64_t botGuid) { return classes.Get(botGuid); };
    policyOptions.AutoGearing = settings.AutoGearing;
    policyOptions.AcceptInvites = settings.ChatCommands;
    policyOptions.IsAlt = [alts](std::uint64_t guid) { return alts->Has(guid); };
    policyOptions.OnEvent = [](std::string const& line) { std::cerr << "forever-bots: event " << line << "\n"; };
    policyOptions.AutoGearEveryLevels = settings.AutoGearEveryLevels;
    PumpExtras extras;
    extras.Commands = commands;
    if (scenarioOn)
    {
        // The scenario takes over the party, the route and the dressing: no vendor walks, no ramp, one party of the whole roster.
        policyOptions.PartySize = std::uint32_t(settings.Scenario.Roster.size());
        policyOptions.PartyCount = 1;
        policyOptions.ServiceEveryMs = 0;
        policyOptions.AutoGearing = false;
        for (auto const& point : settings.Scenario.Route)
            policyOptions.Route.push_back({ point[0], point[1], point[2] });

        ScenarioPlan plan;
        plan.Name = settings.Scenario.Name;
        plan.RosterSize = std::uint32_t(settings.Scenario.Roster.size());
        plan.EntryMap = static_cast<std::uint32_t>(settings.Scenario.Entry[0]);
        plan.Entry = { settings.Scenario.Entry[1], settings.Scenario.Entry[2], settings.Scenario.Entry[3] };
        for (auto const& point : settings.Scenario.Route)
            plan.Route.push_back({ point[0], point[1], point[2] });
        plan.DurationMs = std::uint64_t(settings.Scenario.DurationSec) * 1000;
        plan.Gear = settings.Scenario.Gear;
        plan.Bosses = settings.Scenario.Bosses;
        plan.ClassOf = policyOptions.ClassOf;
        plan.WipeRecovery = settings.Scenario.WipeRecovery;
        plan.WipeRecoveryMs = std::uint64_t(settings.Scenario.WipeRecoverySec) * 1000;
        plan.RequireBosses = settings.Scenario.RequireBosses;
        plan.MinAlive = settings.Scenario.MinAlive;
        plan.LeaderDeadAbandonMs = std::uint64_t(settings.Scenario.LeaderDeadAbandonSec) * 1000;
        if (settings.Scenario.HasPortal)
        {
            policyOptions.HasPortal = true;
            policyOptions.PortalMap = static_cast<std::uint32_t>(settings.Scenario.Portal[0]);
            policyOptions.PortalPoint = { settings.Scenario.Portal[1], settings.Scenario.Portal[2], settings.Scenario.Portal[3] };
            policyOptions.PortalInsideMap = plan.EntryMap;
        }
        extras.Scenario = std::move(plan);
        extras.ScenarioResultFile = settings.Scenario.ResultFile;
    }

    // Tier 2 is granted at handshake, so there is a region only if the core accepted it.
    std::atomic<bool> running{true};
    std::thread pump;
    if (forever_module::Tier2Session* session = client.AttachTier2())
        pump = std::thread(PumpTier2, std::ref(*session), std::cref(running), policyOptions, std::cref(settings.TelemetryFile),
                           std::cref(settings), std::move(extras));
    else
        std::cerr << "forever-bots: no Tier 2 region (" << client.tier2_error() << "); bots will not be driven\n";

    client.Run();

    running = false;
    if (pump.joinable())
        pump.join();
    return 0;
}
