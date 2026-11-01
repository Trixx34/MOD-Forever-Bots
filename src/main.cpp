// SPDX-License-Identifier: GPL-2.0-or-later
// forever-bots: asks the core for bots, then drives the ones it controls from Tier 2 snapshots.
//
// Usage: forever-bots <tier1-endpoint>. The core passes the endpoint when it spawns the module.
//
// Two loops run. Run() (the Tier 1 loop) handles control replies, revocations and the spawn ramp on this thread. A pump
// thread reads each new Tier 2 snapshot and publishes the bot actions BotPolicy decides on. The pump touches only the
// Tier 2 region and the ClassTable (locked), so the two loops share no other state.
#include "main_internal.h"

namespace main_detail {

// Config: kept on the first delivery, and on a re-delivery (a core reload) the ramp, the random picks, the cap and the dungeon files are updated.
void WatchConfig(forever_module::ModuleClient& client, BotSettings& settings, bool& configured, BotRoster& roster, bool tier2Granted, std::uint32_t grantedBots,
                 std::shared_ptr<DungeonReloadBox> const& dungeonBox)
{
    client.OnConfig([&settings, &configured, &roster, tier2Granted, grantedBots, dungeonBox](
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

        {
            // the dungeon files are read again by the pump (doc/dungeon-strategy.md, "Reloading")
            std::lock_guard<std::mutex> lock(dungeonBox->Lock);
            dungeonBox->Sources = SourcesFromConfig(config);
        }
        dungeonBox->Generation.fetch_add(1);
        settings.MaxBots = parsed.MaxBots;
        settings.Race = parsed.Race;
        settings.Class = parsed.Class;
        settings.Ramp = parsed.Ramp;
        roster.SetMaxBots(EffectiveMaxBots(parsed.MaxBots, grantedBots, tier2Granted));
        std::cerr << "forever-bots: config reloaded: MaxBots " << roster.MaxBots() << ", ramp " << settings.Ramp.Initial << " +"
                  << settings.Ramp.Step << " every " << settings.Ramp.StepMinutes << " min to " << settings.Ramp.Cap
                  << " (owned " << roster.Owned().size() << ")\n";
    });
}

// A bot the core takes back: out of the roster, and out of the alts when it was one.
void WatchRevocations(forever_module::ModuleClient& client, BotRoster& roster, std::shared_ptr<AltRegistry> const& alts)
{
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
}

// Bots from last run are rows in the module store (bot/<guid>). Each row's value is "race class", read before the
// bot is requested so the rotation knows its class. A revoke keeps the row, since the core revokes every bot at
// shutdown; only "character not found" removes one. Spawning for the rest is the ramp's job.
void ReclaimBot(RunContext& run, std::uint64_t guid)
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
}

// The spawns: a scenario's roster one bot at a time, or else the bots of the last run reclaimed and the ramp for the rest.
void StartSpawns(forever_module::ModuleClient& client, RunContext& run, BotRoster& roster, BotSettings& settings, std::uint32_t maxBots, bool scenarioOn,
                 std::size_t& scenarioSpawned)
{
    if (scenarioOn)
    {
        roster.SetMaxBots(std::min<std::uint32_t>(ScenarioBotCount(settings.Scenarios), maxBots));
        for (ScenarioSettings const& scenario : settings.Scenarios)
            std::cerr << "forever-bots: scenario " << scenario.Name << ": " << scenario.Parties << " part" << (scenario.Parties == 1 ? "y" : "ies") << " of " << scenario.Roster.size() << " bots at level "
                      << scenario.Level << ", route of " << scenario.Route.size() << " point(s), " << scenario.DurationSec << " s" << (scenario.Raid ? " (raid, 8 subgroups)" : "") << "\n";
        if (settings.Scenarios.size() > 1)
            std::cerr << "forever-bots: scenario of " << settings.Scenarios.size() << " dungeons: " << ScenarioPartyCount(settings.Scenarios) << " parties, "
                      << ScenarioBotCount(settings.Scenarios) << " bots\n";
        client.AddTimer(std::chrono::milliseconds(3000), [&run, &scenarioSpawned] { ScenarioSpawnTick(run, scenarioSpawned); });
    }

    if (!scenarioOn)
    client.StoreList(bot_store::KeyPrefix, [&run](forever_module::ActionReply const& reply)
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
                ReclaimBot(run, guid);
        }
    });

    if (!scenarioOn)
        client.AddTimer(kRampInterval, [&run] { RampTick(run); });
}

} // namespace main_detail

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: forever-bots <tier1-endpoint>\n";
        return 1;
    }

    forever_module::ModuleManifest manifest;
    manifest.module_name = "forever-bots";
    manifest.module_version = FOREVER_MODULE_VERSION; // from module.json, see CMakeLists.txt
    manifest.tier2_participates = true;
    manifest.requested_max_bots = kMaxBotsCeiling;
    manifest.requested_max_nearby_units = kRequestedNearbyUnits;
    manifest.requested_max_actions = kRequestedActions;
    manifest.tier1_hooks = { "CommandScript", "PlayerScript.OnChat", "PlayerScript.OnBotTrade", "PlayerScript.OnLootItem" };

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
    // Ordering note: settings/roster/alts/commands below are captured by reference into the OnConfig/OnControlRevoked/
    // OnRequest callbacks registered further down this function. Each callback's captures must outlive the ModuleClient
    // (they do: client, settings, roster and the shared_ptr registries are all declared in this same main() frame and
    // destructed in reverse order after client, so there is no dangling-reference risk), but it does mean a callback must
    // not be registered before the state it captures is constructed. Keep new captured state declared above its callback.
    BotSettings settings;
    bool configured = false;
    BotRoster roster(0); // the cap is set once the settings are known
    auto const dungeonBox = std::make_shared<DungeonReloadBox>();
    WatchConfig(client, settings, configured, roster, tier2Granted, grantedBots, dungeonBox);

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
    WatchRevocations(client, roster, alts);

    // .autogear: a player in a party types it (as a chat command) and the bots that player leads are dressed for their level.
    auto commands = std::make_shared<CommandQueue>();
    client.OnRequest("CommandScript", [commands, alts, &run, &settings](forever_module::EventRequest const& request) -> forever_module::EventReply
    {
        return CommandScriptReply(request, commands, alts, run, settings);
    });

    // Bot trade (trade_policy.h). Everything below runs on the client's thread (notify callbacks, timers and action replies), so the TradeBook
    // needs no lock. The core only sends the events of bots this module controls, and only when Bot.Trade.Enabled is on there.
    auto trades = std::make_shared<trade::TradeBook>();
    auto runTrade = std::make_shared<std::function<void(std::vector<trade::Step> const&)>>();
    SetUpTrade(client, trades, runTrade, commands);
    SetUpChat(client, commands, settings, trades, runTrade);

    bool const scenarioOn = !settings.Scenarios.empty();
    std::size_t scenarioSpawned = 0;
    StartSpawns(client, run, roster, settings, maxBots, scenarioOn, scenarioSpawned);

    // The policy's class lookup: a bot's own class, or the configured class (0 = none) for a bot the table does not know.
    PolicyOptions policyOptions;
    PolicyFromSettings(policyOptions, settings);
    std::vector<dungeon_events::Event> const allEvents = LoadDungeons(policyOptions, settings, dungeonBox);
    LoadPolicyTables(policyOptions, settings);
    ConnectPolicyLookups(policyOptions, settings, races, classes, run, alts);
    PumpExtras extras;
    extras.Dungeons = dungeonBox;
    extras.Commands = commands;
    extras.Alts = alts;
    if (settings.Presence.Enabled && !scenarioOn)
        SetUpPresence(client, run, settings, extras, trades);
    // Loot of the bots, kept as listing candidates for the Auction House (green or better, not equipment, not a quest item) and as sale candidates
    // for the vendors (white, not equipment, not a quest item). One handler for both: the client keeps one handler per hook.
    auto book = std::make_shared<AuctionBook>();
    auto sells = std::make_shared<SellBook>();
    if ((settings.Auction.Enabled || settings.Sell.Enabled) && !scenarioOn)
        SetUpLootBooks(client, run, settings, book, sells, extras);
    if (settings.Auction.Enabled && !scenarioOn)
        SetUpAuction(client, run, settings, book, extras);
    if (settings.Errand.Enabled && !scenarioOn)
        SetUpErrands(client, settings, commands, book, extras);
    if (scenarioOn)
        SetUpScenario(policyOptions, extras, settings, allEvents);

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