// forever-bots: asks the core for bots, then drives the ones it controls from Tier 2 snapshots.
//
// Usage: forever-bots <tier1-endpoint>. The core passes the endpoint when it spawns the module.
//
// Two loops run. Run() (the Tier 1 loop) handles control replies and revocations on this thread. A pump thread reads
// each new Tier 2 snapshot and publishes the bot actions BotPolicy decides on. The pump touches only the Tier 2
// region, so the two loops share no state.

#include "bot_policy.h"
#include "bot_roster.h"
#include "bot_settings.h"
#include "bot_store.h"
#include "bot_telemetry.h"

#include "forever_module/module_client.h"
#include "forever_module/tier2_session.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// Tier 2 bounds asked for at handshake. The core may grant less. Handshake comes before config, so the bot
// count asked for here is the ceiling; MaxBots from config limits how many bots are actually spawned.
constexpr std::uint32_t kRequestedNearbyUnits = 16;
constexpr std::uint32_t kRequestedActions = 16;

constexpr std::chrono::milliseconds kPumpInterval(5);

char const* ActionTypeName(std::uint8_t type)
{
    switch (static_cast<forever_module::ActionType>(type))
    {
        case forever_module::ActionType::kCastSpell: return "cast";
        case forever_module::ActionType::kMoveTo: return "move";
        case forever_module::ActionType::kAttackTarget: return "attack";
        case forever_module::ActionType::kFollow: return "follow";
        case forever_module::ActionType::kStop: return "stop";
    }
    return "action";
}

// Milliseconds on a monotonic clock, for the policy's cooldowns and fight timer.
std::uint64_t nowMs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

void PumpTier2(forever_module::Tier2Session& session, std::atomic<bool> const& running, PolicyOptions options,
               std::string const& telemetryPath, std::uint32_t summarySec)
{
    BotPolicy policy(options);
    bool sawSnapshot = false;
    std::uint64_t lastTick = 0;
    // Last action sequence seen per bot, so each result is reported once.
    std::unordered_map<std::uint64_t, std::uint32_t> seenSequence;

    // The telemetry file is appended to, so a restart adds to the same record. Every line is flushed as it is written.
    std::ofstream telemetryOut;
    std::unique_ptr<BotTelemetry> telemetry;
    if (!telemetryPath.empty())
    {
        telemetryOut.open(telemetryPath, std::ios::app);
        if (telemetryOut)
        {
            telemetryOut.seekp(0, std::ios::end);
            if (telemetryOut.tellp() == 0)
                telemetryOut << BotTelemetry::Header() << '\n';
            telemetryOut.flush();
            BotTelemetry::Options telemetryOptions;
            telemetryOptions.SummaryEveryMs = std::uint64_t(summarySec) * 1000;
            telemetry = std::make_unique<BotTelemetry>(
                [&telemetryOut](std::string const& line)
                {
                    telemetryOut << line << '\n';
                    telemetryOut.flush();
                },
                telemetryOptions);
            std::cerr << "forever-bots: telemetry to " << telemetryPath << " (summary every " << summarySec << " s)\n";
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
            if (bot.last_action_status != forever_module::BotActionStatus::kApplied)
            {
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
            std::string error;
            if (!session.PublishActions(policy.Decide(view, nowMs()), error))
                std::cerr << "forever-bots: could not publish actions: " << error << "\n";
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

    // Config is delivered right after the handshake, so the callback has already run once when OnConfig returns.
    // Later re-deliveries come on the Run() thread; they are logged but do not change the bots already spawned.
    BotSettings settings;
    bool configured = false;
    client.OnConfig([&settings, &configured](std::unordered_map<std::string, std::string> const& config)
    {
        std::vector<std::string> warnings;
        BotSettings const parsed = ParseBotSettings(config, warnings);
        for (std::string const& warning : warnings)
            std::cerr << "forever-bots: config: " << warning << "\n";
        if (!configured)
            settings = parsed;
        configured = true;
    });

    PolicyOptions policyOptions;
    policyOptions.TestSpell = settings.TestSpell;
    policyOptions.ClassId = settings.Class;

    // The bots we may control: MaxBots from config, capped at what the core granted for Tier 2. Without a Tier 2 region
    // there is nothing to drive, so no bots are requested. The core enforces the same bound (ModuleBotOwnership).
    bool const tier2Granted = !client.tier2_region_name().empty();
    std::uint32_t const grantedBots = client.negotiated_bounds().max_bots;
    std::uint32_t const maxBots = EffectiveMaxBots(settings.MaxBots, grantedBots, tier2Granted);
    if (maxBots != settings.MaxBots)
    {
        std::cerr << "forever-bots: MaxBots " << settings.MaxBots << " limited to " << maxBots << " (Tier 2 grant "
                  << grantedBots << (tier2Granted ? "" : ", no Tier 2 region") << ")\n";
    }

    // Shared between the callbacks below; all of them run on the thread that calls Run().
    BotRoster roster(maxBots);

    client.OnControlRevoked([&roster](std::uint64_t botGuid, std::string const& reason)
    {
        if (roster.OnRevoked(botGuid, reason))
            std::cerr << "forever-bots: control of bot " << botGuid << " revoked: " << reason << "\n";
    });

    forever_module::SpawnParams params;
    params.set_race(settings.Race);
    params.set_class_id(settings.Class);

    // Bots from last run are rows in the module store (bot/<guid>). They are listed first, on the Run() thread, and the
    // bots are requested when the list arrives: reclaim up to the cap, then spawn for the rest. A revoke keeps the row,
    // since the core revokes every bot at shutdown; only "character not found" removes one.
    auto startBots = [&roster, &client, &params](std::vector<std::uint64_t> const& saved)
    {
        for (std::uint64_t guid : saved)
        {
            if (!roster.CanRequest())
                break;
            roster.NoteRequestSent(guid);
            client.RequestControl(guid, {}, [&roster, &client, guid](forever_module::ControlResponse const& reply)
            {
                if (reply.granted())
                {
                    roster.OnGranted(guid, reply.bot_guid());
                    std::cerr << "forever-bots: reclaimed bot " << guid << "\n";
                    return;
                }
                roster.OnDenied(guid);
                if (reply.reason() == "character not found")
                    client.StoreDelete(bot_store::KeyFor(guid));
                std::cerr << "forever-bots: could not reclaim bot " << guid << ": " << reply.reason() << "\n";
            });
        }

        while (roster.CanRequest())
        {
            roster.NoteRequestSent(0);
            client.RequestControl(0, params, [&roster, &client](forever_module::ControlResponse const& reply)
            {
                if (!reply.granted())
                {
                    roster.OnDenied(0);
                    std::cerr << "forever-bots: spawn refused: " << reply.reason() << "\n";
                    return;
                }
                roster.OnGranted(0, reply.bot_guid());
                client.StoreSet(bot_store::KeyFor(reply.bot_guid()), "1");
                std::cerr << "forever-bots: controlling bot " << reply.bot_guid() << "\n";
            });
        }
    };

    client.StoreList(bot_store::KeyPrefix, [&client, &startBots](forever_module::ActionReply const& reply)
    {
        std::vector<std::uint64_t> saved;
        if (!reply.success())
        {
            std::cerr << "forever-bots: could not read saved bots (" << reply.reason() << "); spawning without reclaim\n";
        }
        else
        {
            for (int i = 0; i < reply.keys_size(); ++i)
            {
                std::uint64_t guid = 0;
                if (bot_store::GuidFromKey(reply.keys(i), guid))
                    saved.push_back(guid);
            }
        }
        startBots(saved);
    });

    // Tier 2 is granted at handshake, so there is a region only if the core accepted it.
    std::atomic<bool> running{true};
    std::thread pump;
    if (forever_module::Tier2Session* session = client.AttachTier2())
        pump = std::thread(PumpTier2, std::ref(*session), std::cref(running), policyOptions, std::cref(settings.TelemetryFile),
                           settings.TelemetrySummarySec);
    else
        std::cerr << "forever-bots: no Tier 2 region (" << client.tier2_error() << "); bots will not be driven\n";

    client.Run();

    running = false;
    if (pump.joinable())
        pump.join();
    return 0;
}
