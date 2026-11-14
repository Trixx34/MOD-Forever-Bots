// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for off-map recovery (off_map.h): the known-world box, the nearest home, the timing and cooldown, and the policy's teleport of a bot
// left far off its map (prod 2026-11-14: Jorordan on map 2991 at (954, -1574), zone 0). No core needed.
#include "bot_policy.h"
#include "grind_data.h"
#include "off_map.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotSnapshot;
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

// A grid of 4 x 4 places on map 2991 covering x 2000..3500, y 500..2000 (Zephras Isle's quest anchors span about x 1760..4144, y 347..2100),
// the first row Alliance, the last row Horde, the rest both.
off_map::KnownWorld Island()
{
    off_map::KnownWorld world;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            world.Add({ 2991, 2000.0f + 500.0f * i, 500.0f + 500.0f * j, 700.0f, std::uint8_t(j == 0 ? 1 : j == 3 ? 2 : 0) });
    return world;
}

void BoxAndHome()
{
    off_map::KnownWorld const world = Island();
    CHECK(world.Knows(2991));
    CHECK(!world.Knows(0));                                    // no places: never judged
    CHECK(!world.Outside(0, 99999.0f, 99999.0f, 500.0f));
    CHECK(!world.Outside(2991, 2600.0f, 1200.0f, 500.0f));    // inside
    CHECK(!world.Outside(2991, 1600.0f, 1200.0f, 500.0f));    // 400 yards west of the box: within the margin
    CHECK(world.Outside(2991, 954.0f, -1574.0f, 500.0f));     // Jorordan's place
    CHECK(world.Outside(2991, 2600.0f, 2600.0f, 500.0f));     // 600 yards north

    off_map::KnownWorld few;
    for (std::size_t i = 0; i + 1 < off_map::kMinPlaces; ++i)
        few.Add({ 389, float(i), 0.0f, 0.0f, 0 });            // an instance with a quest giver or two: too few places
    CHECK(!few.Knows(389) && !few.Outside(389, 5000.0f, 5000.0f, 500.0f));

    // The nearest place of the bot's side: (2000, 500) is Alliance, so a Horde bot gets (2000, 1000).
    off_map::Place const* alliance = world.Home(2991, 954.0f, -1574.0f, 1);
    off_map::Place const* horde = world.Home(2991, 954.0f, -1574.0f, 2);
    off_map::Place const* any = world.Home(2991, 954.0f, -1574.0f, 0);
    CHECK(alliance && alliance->X == 2000.0f && alliance->Y == 500.0f);
    CHECK(horde && horde->X == 2000.0f && horde->Y == 1000.0f);
    CHECK(any && any->X == 2000.0f && any->Y == 500.0f);
    CHECK(world.Home(0, 0.0f, 0.0f, 0) == nullptr);
}

void TimingAndCooldown()
{
    off_map::KnownWorld const world = Island();
    off_map::Options options;
    options.Enabled = true;
    off_map::Watch watch;
    off_map::Facts off;
    off.Map = 2991;
    off.X = 954.0f;
    off.Y = -1574.0f;

    CHECK(off_map::Check(options, world, watch, off, 1000) == off_map::Verdict::kWaiting);
    CHECK(off_map::Check(options, world, watch, off, 60999) == off_map::Verdict::kWaiting);
    // Fighting: not sent (the core refuses a teleport in combat), the time keeps counting.
    off_map::Facts fighting = off;
    fighting.InCombat = true;
    CHECK(off_map::Check(options, world, watch, fighting, 61000) == off_map::Verdict::kBlocked);
    off_map::Facts held = off;
    held.Held = true;
    CHECK(off_map::Check(options, world, watch, held, 61000) == off_map::Verdict::kBlocked);
    CHECK(off_map::Check(options, world, watch, off, 61000) == off_map::Verdict::kSend);
    CHECK(watch.Tries == 1);
    // Still off (the teleport was refused): not again before the cooldown.
    CHECK(off_map::Check(options, world, watch, off, 62000) == off_map::Verdict::kCooldown);
    CHECK(off_map::Check(options, world, watch, off, 61000 + options.CooldownMs - 1) == off_map::Verdict::kCooldown);
    CHECK(off_map::Check(options, world, watch, off, 61000 + options.CooldownMs) == off_map::Verdict::kSend);
    CHECK(watch.Tries == 2);

    // Back on the map: the watch is reset, a new spell off the map waits the whole time again.
    off_map::Facts home = off;
    home.X = 2600.0f;
    home.Y = 1200.0f;
    CHECK(off_map::Check(options, world, watch, home, 700000) == off_map::Verdict::kOnMap);
    CHECK(watch.OffSinceMs == 0 && watch.Tries == 0 && watch.NextTryMs == 0);
    CHECK(off_map::Check(options, world, watch, off, 700001) == off_map::Verdict::kWaiting);

    // On a flight or a ship: never judged, and the watch starts over.
    off_map::Facts flying = off;
    flying.Travelling = true;
    CHECK(off_map::Check(options, world, watch, flying, 800000) == off_map::Verdict::kTravelling);
    CHECK(watch.OffSinceMs == 0);

    // A dead bot that was never seen off is not judged; one that was keeps its time, and is sent once alive.
    off_map::Watch deadWatch;
    off_map::Facts dead = off;
    dead.Alive = false;
    CHECK(off_map::Check(options, world, deadWatch, dead, 1000) == off_map::Verdict::kOnMap);
    CHECK(deadWatch.OffSinceMs == 0);
    CHECK(off_map::Check(options, world, deadWatch, off, 2000) == off_map::Verdict::kWaiting);
    CHECK(off_map::Check(options, world, deadWatch, dead, 70000) == off_map::Verdict::kBlocked);
    CHECK(off_map::Check(options, world, deadWatch, off, 70001) == off_map::Verdict::kSend);
}

// The policy: a grind spot list that knows map 2991, a bot far off it. It is teleported after OffMapSeconds to the nearest spot of its side, once per
// cooldown, and not at all with the switch off or inside the island.
std::shared_ptr<GrindData> IslandSpots()
{
    auto grind = std::make_shared<GrindData>();
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            GrindSpot spot;
            spot.Map = 2991;
            spot.X = 2000.0f + 500.0f * i;
            spot.Y = 500.0f + 500.0f * j;
            spot.Z = 700.0f;
            spot.MinLevel = 50;   // no spot for the level 12 bot: it does not walk anywhere by itself
            spot.MaxLevel = 60;
            spot.Side = std::uint8_t(j == 0 ? 2 : 0);
            grind->Spots.push_back(spot);
        }
    return grind;
}

WorldTickSnapshotView LostBot(float x, float y)
{
    WorldTickSnapshotView view;
    view.tick_number = 1;
    view.max_nearby_units = 2;
    BotSnapshot bot;
    bot.guid = 654;
    bot.health = 1000;
    bot.max_health = 1000;
    bot.level = 12;
    bot.map_id = 2991;
    bot.pos_x = x;
    bot.pos_y = y;
    bot.pos_z = 80.0f;
    view.bots.push_back(bot);
    view.nearby.resize(2);
    return view;
}

BotAction const* TeleportIn(std::vector<BotAction> const& actions)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kTeleport)
            return &action;
    return nullptr;
}

PolicyOptions IslandOptions(std::vector<std::string>* events)
{
    PolicyOptions options;
    options.Grind = IslandSpots();
    options.WanderWhenIdle = false;
    options.SideOf = [](std::uint64_t) -> std::uint8_t { return 0; };
    options.PartySideOf = [](std::uint64_t) -> std::uint8_t { return 1; };   // Skyborne (Alliance): the Horde row is not a home
    options.OffMap.Enabled = true;
    options.OffMap.OffForMs = 60000;
    options.OffMap.CooldownMs = 600000;
    if (events)
        options.OnEvent = [events](std::string const& line) { events->push_back(line); };
    return options;
}

bool HasEvent(std::vector<std::string> const& events, char const* prefix)
{
    for (std::string const& line : events)
        if (line.rfind(prefix, 0) == 0)
            return true;
    return false;
}

void PolicyTeleportsALostBotHome()
{
    std::vector<std::string> events;
    BotPolicy policy(IslandOptions(&events));
    WorldTickSnapshotView const view = LostBot(954.0f, -1574.0f);
    std::uint64_t now = 1000;
    bool early = false;
    for (; now < 60000; now += 1000)
        early = early || TeleportIn(policy.Decide(view, now)) != nullptr;
    CHECK(!early);
    CHECK(HasEvent(events, "off_map_seen: bot=654 map=2991 pos=954,-1574,80"));
    BotAction const* teleport = nullptr;
    std::vector<BotAction> actions;
    for (; now <= 62000 && !teleport; now += 1000)
    {
        actions = policy.Decide(view, now);
        teleport = TeleportIn(actions);
    }
    CHECK(teleport != nullptr);
    if (teleport)
    {
        CHECK(teleport->spell_id == 2991);
        CHECK(teleport->dest_x == 2000.0f && teleport->dest_y == 1000.0f && teleport->dest_z == 700.0f);   // (2000, 500) is Horde
        CHECK(teleport->bot_guid == 654);
    }
    CHECK(HasEvent(events, "off_map_home: bot=654 map=2991 pos=954,-1574,80 to=2000,1000,700"));
    // The core refused it (Bot.Actions.Teleport 0): the bot is still there. Nothing more for the cooldown.
    bool again = false;
    std::uint64_t const sentAt = now - 1000;
    for (; now < sentAt + 600000; now += 5000)
        again = again || TeleportIn(policy.Decide(view, now)) != nullptr;
    CHECK(!again);
    bool retried = false;
    for (std::uint64_t until = now + 10000; now <= until && !retried; now += 1000)
        retried = TeleportIn(policy.Decide(view, now)) != nullptr;
    CHECK(retried);
}

void PolicyLeavesBotsOnTheMapAlone()
{
    // Inside the island, on a flight off it, fighting off it, or with the switch off: no teleport.
    {
        BotPolicy policy(IslandOptions(nullptr));
        WorldTickSnapshotView const view = LostBot(2600.0f, 1200.0f);
        bool sent = false;
        for (std::uint64_t now = 1000; now < 200000; now += 1000)
            sent = sent || TeleportIn(policy.Decide(view, now)) != nullptr;
        CHECK(!sent);
    }
    {
        BotPolicy policy(IslandOptions(nullptr));
        WorldTickSnapshotView view = LostBot(954.0f, -1574.0f);
        view.bots[0].in_flight = 1;
        bool sent = false;
        for (std::uint64_t now = 1000; now < 200000; now += 1000)
            sent = sent || TeleportIn(policy.Decide(view, now)) != nullptr;
        CHECK(!sent);
    }
    {
        PolicyOptions options = IslandOptions(nullptr);
        options.OffMap.Enabled = false;
        BotPolicy policy(options);
        WorldTickSnapshotView const view = LostBot(954.0f, -1574.0f);
        bool sent = false;
        for (std::uint64_t now = 1000; now < 200000; now += 1000)
            sent = sent || TeleportIn(policy.Decide(view, now)) != nullptr;
        CHECK(!sent);
    }
    {
        // A map the module knows nothing of (an instance): never judged.
        BotPolicy policy(IslandOptions(nullptr));
        WorldTickSnapshotView view = LostBot(954.0f, -1574.0f);
        view.bots[0].map_id = 389;
        bool sent = false;
        for (std::uint64_t now = 1000; now < 200000; now += 1000)
            sent = sent || TeleportIn(policy.Decide(view, now)) != nullptr;
        CHECK(!sent);
    }
}

} // namespace

int main()
{
    BoxAndHome();
    TimingAndCooldown();
    PolicyTeleportsALostBotHome();
    PolicyLeavesBotsOnTheMapAlone();
    if (failures == 0)
        std::printf("off_map_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
