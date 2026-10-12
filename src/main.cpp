// forever-bots: asks the core for bots and keeps track of the ones it controls.
//
// Usage: forever-bots <tier1-endpoint>. The core passes the endpoint when it spawns the module.
// This slice only requests and tracks control. It does not drive the bots yet: Tier 2 actions are not
// implemented in the SDK, so tier2_participates stays false.

#include "bot_roster.h"

#include "forever_module/module_client.h"

#include <cstdint>
#include <iostream>

namespace {

// Spawn defaults until module config from the core is wired up (the core sends an empty config today).
constexpr std::uint32_t kDefaultMaxBots = 1;
constexpr std::uint32_t kDefaultRace = 1;    // Human
constexpr std::uint32_t kDefaultClass = 1;   // Warrior

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
    manifest.tier2_participates = false;

    forever_module::ModuleClient client(std::move(manifest));
    if (!client.Connect(argv[1]) || !client.Handshake())
    {
        std::cerr << "forever-bots: could not connect to the core: " << client.rejection_reason() << "\n";
        return 1;
    }

    // Shared between the callbacks below; all of them run on the thread that calls Run().
    BotRoster roster(kDefaultMaxBots);

    client.OnControlRevoked([&roster](std::uint64_t botGuid, std::string const& reason)
    {
        if (roster.OnRevoked(botGuid, reason))
            std::cerr << "forever-bots: control of bot " << botGuid << " revoked: " << reason << "\n";
    });

    forever_module::SpawnParams params;
    params.set_race(kDefaultRace);
    params.set_class_id(kDefaultClass);

    while (roster.CanRequest())
    {
        roster.NoteRequestSent(0);
        client.RequestControl(0, params, [&roster](forever_module::ControlResponse const& reply)
        {
            if (!reply.granted())
            {
                roster.OnDenied(0);
                std::cerr << "forever-bots: spawn refused: " << reply.reason() << "\n";
                return;
            }
            roster.OnGranted(0, reply.bot_guid());
            std::cerr << "forever-bots: controlling bot " << reply.bot_guid() << "\n";
        });
    }

    client.Run();
    return 0;
}
