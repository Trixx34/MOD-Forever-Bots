// SPDX-License-Identifier: GPL-2.0-or-later
// Shared helpers of the policy tests (split out of policy_test.cpp); every policy_test_NN.cpp includes this header.
#pragma once
// Unit tests for BotPolicy, on hand-built snapshots. No core needed.
#include "bot_policy.h"
#include <cmath>
#include "grind_data.h"

#include "combat_rotation.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <set>
#include <functional>
#include <sstream>
#include <string>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotActionStatus;
using forever_module::BotSnapshot;
using forever_module::MakeUnitRef;
using forever_module::NearbyUnit;
using forever_module::WorldTickSnapshotView;


// One counter for all the part files.
inline int failures = 0;

namespace {

// The pull and combat tests were written for bots that start Aggressive; the defensive default has its own test.
inline PolicyOptions AggressiveOptions()
{
    PolicyOptions options;
    options.AggressiveStart = true;
    return options;
}


#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

constexpr std::uint64_t kBot = 42;

// One bot at the origin, with the given nearby units. Each unit sits at (x, 0).
inline WorldTickSnapshotView View(std::uint64_t tick, std::uint64_t threat, std::vector<NearbyUnit> nearby)
{
    WorldTickSnapshotView view;
    view.tick_number = tick;
    view.max_nearby_units = 8;
    BotSnapshot bot;
    bot.guid = kBot;
    bot.health = 1000;
    bot.max_health = 1000;
    bot.threat_target_guid = threat;
    bot.nearby_count = static_cast<std::uint32_t>(nearby.size());
    view.bots.push_back(bot);
    view.nearby.resize(view.max_nearby_units);
    for (std::size_t i = 0; i < nearby.size(); ++i)
        view.nearby[i] = nearby[i];
    return view;
}

inline NearbyUnit Unit(std::uint8_t type, std::uint64_t counter, float x, bool hostile, std::uint32_t health = 100)
{
    NearbyUnit unit;
    unit.guid = MakeUnitRef(type, 1, counter);
    unit.pos_x = x;
    unit.health = health;
    unit.is_hostile = hostile;
    return unit;
}






// A bot's health and max health, with the bot's own threat target. Builds on View().
inline WorldTickSnapshotView Hurt(std::uint32_t health, std::vector<NearbyUnit> nearby, std::uint64_t threat = 0)
{
    WorldTickSnapshotView view = View(1, threat, std::move(nearby));
    view.bots[0].health = health;
    view.bots[0].life_state = health == 0 ? 1 : 0;
    return view;
}











// A hostile at `x` yards with the given health percent (max_health is set, so the percent is real).
inline NearbyUnit Hostile(std::uint64_t counter, float x, std::uint32_t healthPct)
{
    NearbyUnit unit = Unit(8, counter, x, true, healthPct);
    unit.max_health = 100;
    return unit;
}

// A warrior fighting one hostile, with the rotation on.
inline BotPolicy WarriorPolicy()
{
    PolicyOptions options = AggressiveOptions();
    options.ClassId = 1;
    return BotPolicy(options);
}

inline bool HasCast(std::vector<BotAction> const& actions, std::uint32_t spell)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kCastSpell && action.spell_id == spell)
            return true;
    return false;
}











// The rest action in `actions`: its spell_id (1 sit, 0 stand) and its position in the list, or -1 if there is none.
inline int RestAt(std::vector<BotAction> const& actions, std::uint32_t& spellId)
{
    for (std::size_t i = 0; i < actions.size(); ++i)
    {
        if (actions[i].type == ActionType::kRest)
        {
            spellId = actions[i].spell_id;
            return static_cast<int>(i);
        }
    }
    return -1;
}






// The skill lines of the kSkillUp actions sent in a tick (these views have a single bot).
inline std::vector<std::uint32_t> CraftsOf(std::vector<BotAction> const& actions)
{
    std::vector<std::uint32_t> skills;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kSkillUp)
            skills.push_back(action.spell_id);
    return skills;
}


// Ground effects (SDK 0.19, layout 21): a bot inside a harmful one walks out of it.
inline std::vector<BotAction> GroundMoves(std::vector<BotAction> const& actions)
{
    std::vector<BotAction> out;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.bot_guid == kBot)
            out.push_back(action);
    return out;
}

inline void SetGroundEffect(WorldTickSnapshotView& view, std::uint32_t spell, float x, float y, float radius, std::uint8_t hostile)
{
    view.bots[0].ground_effect_count = 1;
    view.bots[0].ground_effects[0] = forever_module::GroundEffect{};
    view.bots[0].ground_effects[0].spell_id = spell;
    view.bots[0].ground_effects[0].x = x;
    view.bots[0].ground_effects[0].y = y;
    view.bots[0].ground_effects[0].radius = radius;
    view.bots[0].ground_effects[0].kind = 1;
    view.bots[0].ground_effects[0].hostile = hostile;
}





// Whether `actions` holds a kVendor with this spell_id, or a kTrain. The white junk bit (1024, set for a bot with nearly full bags, which a default test
// bot with no free slots is) is not part of what these tests compare.
inline bool HasVendor(std::vector<BotAction> const& actions, std::uint32_t services)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kVendor && (action.spell_id & ~1024u) == services)
            return true;
    return false;
}

// The services of the first kVendor of a tick (0 when there is none).
inline std::uint32_t VendorServices(std::vector<BotAction> const& actions)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kVendor)
            return action.spell_id;
    return 0;
}


inline bool HasTrain(std::vector<BotAction> const& actions)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kTrain)
            return true;
    return false;
}




// A policy for one class with its rotation on, and no per-bot class table.
inline BotPolicy ClassPolicy(std::uint32_t classId)
{
    PolicyOptions options = AggressiveOptions();
    options.ClassId = classId;
    options.WanderWhenIdle = false;       // these tests look at casts and rests: the bot stays where it is
    return BotPolicy(options);
}

// The guid of the cast with this spell, or 0 if there is none.
inline std::uint64_t CastTargetOf(std::vector<BotAction> const& actions, std::uint32_t spell)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kCastSpell && action.spell_id == spell)
            return action.target_guid;
    return 0;
}


// A snapshot of the bot (class 8, a mage) with the result of its last cast set.
inline WorldTickSnapshotView CastResultView(std::uint64_t tick, std::vector<NearbyUnit> nearby, std::uint64_t sequence, std::uint32_t spell,
                                     BotActionStatus status)
{
    WorldTickSnapshotView view = View(tick, 0, std::move(nearby));
    view.bots[0].last_action_sequence = sequence;
    view.bots[0].last_action_type = uint8_t(ActionType::kCastSpell);
    view.bots[0].last_action_spell = spell;
    view.bots[0].last_action_status = status;
    return view;
}















// Parties (M8). Bots on the origin with four nearby slots each; callers set positions and the party fields.
// Mana users get power/max_power set so they can cast spells.
inline WorldTickSnapshotView PartyView(std::vector<std::uint64_t> const& guids, std::function<std::uint32_t(std::uint64_t)> classOf = nullptr)
{
    WorldTickSnapshotView view;
    view.tick_number = 1;
    view.max_nearby_units = 4;
    for (std::uint64_t guid : guids)
    {
        BotSnapshot bot;
        bot.guid = guid;
        bot.health = 1000;
        bot.max_health = 1000;
        std::uint32_t cls = classOf ? classOf(guid) : 0;
        if (cls == 5 || cls == 2 || cls == 7 || cls == 11) // priest, paladin, shaman, druid
        {
            bot.power_type = 0;
            bot.power = 5000;
            bot.max_power = 5000;
        }
        else if (cls == 4 || cls == 8 || cls == 9) // rogue, mage, warlock
        {
            bot.power_type = 0;
            bot.power = 5000;
            bot.max_power = 5000;
        }
        else // warrior, hunter
        {
            bot.power_type = 1; // rage
            bot.power = 0;
            bot.max_power = 100;
        }
        view.bots.push_back(bot);
    }
    view.nearby.resize(guids.size() * view.max_nearby_units);
    return view;
}

// A pre-formed dungeon party: bot 12 is the warrior leader, the rest follow. A test that needs another class
// for a member (a rogue to show that a member defends itself, for instance) passes its own class map.
inline WorldTickSnapshotView DungeonView(std::vector<std::uint64_t> const& guids,
                                  std::function<std::uint32_t(std::uint64_t)> classOf = nullptr)
{
    if (!classOf)
        classOf = [](std::uint64_t guid) { return guid == 12 ? 1u : 5u; };
    WorldTickSnapshotView view = PartyView(guids, classOf);
    // Pre-form the party: core layout group_role 2 is leader, 1 is member; leader reference is a Player unit ref.
    // group_leader_guid must be a full unit reference (HighGuid::Player << 56 | counter).
    constexpr std::uint64_t kLeaderRef = (std::uint64_t(2) << 56) | 12; // HighGuid::Player = 2
    for (BotSnapshot& bot : view.bots)
    {
        if (bot.guid == 12)
        {
            bot.group_role = 2;
            bot.group_leader_guid = kLeaderRef; // leader's own GUID as unit reference
        }
        else
        {
            bot.group_role = 1;
            bot.group_leader_guid = kLeaderRef;
        }
    }
    return view;
}

// A walk longer than the mount threshold spends its first tick mounting up: kMount, then kStop instead of the walk. The walk to the
// destination follows on the next tick. The tests below are about where the bot walks, so they read the tick after the mount;
// BotMountsForALongWalk and its neighbours cover the mount itself.
inline std::vector<BotAction> AfterMount(BotPolicy& policy, WorldTickSnapshotView const& view, std::uint64_t nowMs)
{
    policy.Decide(view, nowMs);
    return policy.Decide(view, nowMs + 1);
}

inline void AddNearby(WorldTickSnapshotView& view, std::size_t botIndex, NearbyUnit const& unit)
{
    view.nearby[botIndex * view.max_nearby_units + view.bots[botIndex].nearby_count++] = unit;
}

// Bot 12 is the warrior; the other bots are priests (class 5) and so follow it.
inline std::uint32_t WarriorLeads(std::uint64_t guid)
{
    return guid == 12 ? 1 : 5;
}

inline BotPolicy PartyPolicy(std::uint32_t size, std::uint32_t count = 1)
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = size;
    options.PartyCount = count;
    options.ClassOf = WarriorLeads;
    return BotPolicy(options);
}

inline std::uint64_t PlayerOf(std::uint64_t counter)
{
    return MakeUnitRef(2, 0, counter);
}

inline bool HasGroup(std::vector<BotAction> const& actions, std::uint64_t bot, std::uint32_t spell, std::uint64_t target = 0)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kGroup && action.bot_guid == bot && action.spell_id == spell &&
            (target == 0 || action.target_guid == target))
            return true;
    return false;
}

inline bool HasAnyGroup(std::vector<BotAction> const& actions, std::uint64_t bot)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kGroup && action.bot_guid == bot)
            return true;
    return false;
}

// An action of `type` for `bot` with `target` as its target (any target when 0).
inline bool HasAction(std::vector<BotAction> const& actions, std::uint64_t bot, ActionType type, std::uint64_t target = 0)
{
    for (BotAction const& action : actions)
        if (action.type == type && action.bot_guid == bot && (target == 0 || action.target_guid == target))
            return true;
    return false;
}








// The invites of a tick as (leader, invited member) pairs.
inline std::set<std::pair<std::uint64_t, std::uint64_t>> InvitesOf(std::vector<BotAction> const& actions)
{
    std::set<std::pair<std::uint64_t, std::uint64_t>> invites;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kGroup && action.spell_id == 1)
            invites.emplace(action.bot_guid, action.target_guid);
    return invites;
}

























// Travel (M6). The core gives each bot its nearest vendor on its map (layout 6); services are on every 60 s. The vendor
// stands at x = 100.
inline void SetVendor(WorldTickSnapshotView& view, float x)
{
    for (BotSnapshot& bot : view.bots)
    {
        bot.vendor_x = x;
        bot.vendor_y = 0.0f;
        bot.vendor_z = 0.0f;
        bot.vendor_valid = 1;
    }
}

inline BotPolicy VendorPolicy(bool party = false, bool sellGear = false)
{
    PolicyOptions options = AggressiveOptions();
    options.VendorSellGear = sellGear;
    options.ServiceEveryMs = 60000;
    if (party)
    {
        options.PartySize = 5;
        options.PartyCount = 1;
        options.ClassOf = WarriorLeads;
    }
    return BotPolicy(options);
}









// Hubs (M7). A bot on map 1 with two hubs on that map. Services are off, so the walk is the only move.
inline BotPolicy HubPolicy()
{
    PolicyOptions options = AggressiveOptions();
    options.HubsByMap[1] = { Point{ 500.0f, 0.0f, 0.0f }, Point{ 900.0f, 0.0f, 0.0f } };
    return BotPolicy(options);
}

inline WorldTickSnapshotView OnMap(std::uint32_t map)
{
    WorldTickSnapshotView view = PartyView({ 10 });
    view.bots[0].map_id = map;
    return view;
}

// The destination of the bot's walk in the actions, or NaN-free -1 when it does not walk.
inline float WalkTo(std::vector<BotAction> const& actions, std::uint64_t bot)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.bot_guid == bot)
            return action.dest_x;
    return -1.0f;
}







// SDK 0.18: a crafting profession's line is asked for, a recipe that needs an anvil sends the bot to the nearest one, and it asks again there.
inline PolicyOptions CraftingWithAnAnvil()
{
    PolicyOptions options = AggressiveOptions();
    options.Crafting = true;
    auto foci = std::make_shared<std::vector<crafting::FocusPoint>>();
    crafting::FocusPoint anvil;
    anvil.City = "Test";
    anvil.Map = 0;
    anvil.Focus = crafting::kFocusAnvil;
    anvil.X = 300.0f;
    anvil.Radius = 10;
    anvil.Name = "Test Anvil";
    foci->push_back(anvil);
    options.Foci = foci;
    return options;
}

// The craft answer of the core for the line of the skill, as the snapshot carries it.
inline void CraftAnswer(WorldTickSnapshotView& view, BotActionStatus status, std::uint32_t sequence)
{
    view.bots[0].last_action_type = std::uint8_t(ActionType::kSkillUp);
    view.bots[0].last_action_spell = 0; // the core need not report the spell: the answer is for the skill that was sent
    view.bots[0].last_action_status = status;
    view.bots[0].last_action_sequence = sequence;
}



inline std::vector<std::uint32_t> GathersOf(std::vector<BotAction> const& actions)
{
    std::vector<std::uint32_t> skills;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kGather)
            skills.push_back(action.spell_id);
    return skills;
}




















// Dungeon party control (doc 19): a five-bot party with the route active. Bot 12 is the warrior leader, the others priests.
inline bool CastsSpell(std::vector<BotAction> const& actions, std::uint64_t bot, std::uint32_t spell, std::uint64_t target = 0)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kCastSpell && action.bot_guid == bot && action.spell_id == spell &&
            (target == 0 || action.target_guid == target))
            return true;
    return false;
}

inline BotPolicy DungeonPolicy(std::function<std::uint32_t(std::uint64_t)> classOf = nullptr)
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = classOf ? std::move(classOf) : WarriorLeads;
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    return policy;
}


// The tank's own health (below kTankRestPct, 80 %) holds its pulls on its own clock, past the party hold's 45 s cap and up to kPullHoldAbsMaxMs (180 s), and the
// pull comes once its health is back. Run 5 of 2026-11-08: the tank, revived at 49 %, pulled a Warder in the second it came back, 47 deaths in the hour.





// A boss point is not passed until its boss is dead, and is never skipped for lack of a route.
inline float LeaderWalk(std::vector<BotAction> const& actions)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo && action.bot_guid == 12)
            return action.dest_x;
    return -1.0f;
}





// ---- "tank lead" (chat) and dungeon events ----
inline std::optional<BotAction> OfTank(std::vector<BotAction> const& actions, ActionType type)
{
    for (BotAction const& action : actions)
        if (action.type == type && action.bot_guid == 12)
            return action;
    return std::nullopt;
}

// A player (counter 900) leads a group of five bots in a dungeon on map 36. Bot 12 is the warrior.
inline WorldTickSnapshotView PlayerGroupView(float tankX = 0.0f)
{
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    for (BotSnapshot& bot : view.bots)
    {
        bot.map_id = 36;
        bot.group_leader_guid = PlayerOf(900);
        bot.group_role = 1;
        bot.pos_x = tankX - 15.0f;                                     // the members trail the tank
    }
    view.bots[2].pos_x = tankX;
    // The player stands next to the tank (a unit in its view).
    NearbyUnit player;
    player.guid = PlayerOf(900);
    player.pos_x = tankX;
    player.is_hostile = false;
    player.health = 100;
    player.max_health = 100;
    AddNearby(view, 2, player);
    return view;
}

inline PolicyOptions LeadOptions()
{
    PolicyOptions options = AggressiveOptions();
    options.ClassOf = WarriorLeads;
    options.WanderWhenIdle = false;
    PolicyOptions::DungeonRoute route;
    route.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f }, Point{ 180.0f, 0.0f, 0.0f } };
    route.BossAt[1] = 644;
    options.DungeonsByMap[36] = route;
    return options;
}

inline PolicyOptions::DungeonRoute RouteWithAnEvent()
{
    PolicyOptions::DungeonRoute route;
    route.Route = { Point{ 100.0f, 0.0f, 0.0f }, Point{ 140.0f, 0.0f, 0.0f }, Point{ 180.0f, 0.0f, 0.0f } };
    std::vector<std::string> warnings;
    route.Events = dungeon_events::Parse("event 36 lever Factory lever\nnear 100 0 0 30\nuse 101831 110 5 0\nend\n", warnings);
    return route;
}

inline BotPolicy::ChatContext FromPlayer(std::uint32_t map = 36)
{
    BotPolicy::ChatContext chat;
    chat.Issuer = 900;
    chat.MapId = map;
    return chat;
}

inline ChatCommand TankLead(bool off = false)
{
    std::optional<ChatCommand> command = ParseChatCommand(off ? "tank lead off" : "tank lead", true);
    return command.value();
}











// DungeonTeleport: a leader with no route to a waypoint is put back on the route once; a member that lost its leader is put next to it.
inline std::optional<BotAction> TeleportOf(std::vector<BotAction> const& actions, std::uint64_t bot)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kTeleport && action.bot_guid == bot)
            return action;
    return std::nullopt;
}













 









// Members defend themselves: bot 10 is a rogue here, since the healer holds its range instead (HealerDoesNotMelee).
inline std::uint32_t RogueTenLeadsTheRest(std::uint64_t guid)
{
    return guid == 12 ? 1u : guid == 10 ? 4u : 5u;
}





// ---- dead bots: release, corpse run, reclaim, resurrect ----

inline std::uint32_t AllWarriors(std::uint64_t) { return 1; }

// A five-bot party (12 leads) whose events are collected. classOf defaults to WarriorLeads (priests follow the warrior).
inline PolicyOptions DeadOptions(std::vector<std::string>* events, std::function<std::uint32_t(std::uint64_t)> classOf = WarriorLeads)
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = classOf;
    options.HasPortal = true;
    options.PortalMap = 0;
    options.PortalPoint = { 500.0f, 0.0f, 0.0f };
    options.PortalInsideMap = 36;
    if (events)
        options.OnEvent = [events](std::string const& text) { events->push_back(text); };
    return options;
}

inline void Corpse(BotSnapshot& bot, float x)
{
    bot.life_state = 1;
    bot.health = 0;
    bot.pos_x = x;
    bot.corpse_valid = 1;
    bot.corpse_map = bot.map_id;
    bot.corpse_x = x;
}

inline void Ghost(BotSnapshot& bot, float x, std::uint32_t corpseMap, float corpseX)
{
    bot.life_state = 2;
    bot.health = 1;
    bot.pos_x = x;
    bot.corpse_valid = 1;
    bot.corpse_map = corpseMap;
    bot.corpse_x = corpseX;
}

inline bool LoggedWith(std::vector<std::string> const& events, char const* text)
{
    for (std::string const& event : events)
        if (event.find(text) != std::string::npos)
            return true;
    return false;
}

inline bool Logged(std::vector<std::string> const& events, char const* prefix)
{
    for (std::string const& text : events)
        if (text.rfind(prefix, 0) == 0)
            return true;
    return false;
}







// After the release, a ghost of a dungeon party that is out of combat is teleported to its leader (the tank), not left at the graveyard or sent to its corpse.
inline bool LoggedTeleportToLeader(std::vector<BotAction> const& actions)
{
    std::optional<BotAction> const teleport = TeleportOf(actions, 13);
    return teleport.has_value() && teleport->spell_id == 36 && teleport->dest_x != 50.0f; // 50 is the corpse and the entrance, not a leader
}



// ---- combat drift (0.45.0): the pull and chase leash, the leader's rejoin, a release that would cut a resurrect ----

// A route-active party of five on map 36, everyone at the origin; the route runs from (0, 0) east to (100, 0). Bot 12 (a warrior) leads; 10 is a rogue (melee).
inline PolicyOptions DriftOptions(std::uint32_t pullLeash, std::uint32_t chaseLeash, std::vector<std::string>* events = nullptr)
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 10 ? 4u : 5u; };
    options.Route = { Point{ 0.0f, 0.0f, 0.0f }, Point{ 100.0f, 0.0f, 0.0f } };
    options.DungeonPullLeashYards = pullLeash;
    options.DungeonChaseLeashYards = chaseLeash;
    if (events)
        options.OnEvent = [events](std::string const& text) { events->push_back(text); };
    return options;
}

inline WorldTickSnapshotView DriftView()
{
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 }, [](std::uint64_t guid) { return guid == 12 ? 1u : guid == 10 ? 4u : 5u; });
    view.max_nearby_units = 8;
    view.nearby.assign(view.bots.size() * view.max_nearby_units, NearbyUnit{});
    for (BotSnapshot& bot : view.bots)
        bot.map_id = 36;
    return view;
}

inline NearbyUnit HostileAt(std::uint64_t counter, float x, float y)
{
    NearbyUnit unit = Hostile(counter, x, 100);
    unit.pos_y = y;
    return unit;
}
























inline WorldTickSnapshotView CorpseNextToPriest(float x = 10.0f)
{
    WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 }, WarriorLeads);   // classOf: bot 12 warrior, the rest priests (mana users)
    Corpse(v.bots[3], x);
    for (std::size_t i : { 0, 1, 2, 4 })
    {
        v.bots[i].max_power = 1000;
        v.bots[i].power = 1000;
    }
    return v;
}








// ---- soak diagnosis: stalled fights, grey creatures, mana ----







// An idle mana user low on mana sits to drink, stands when it has enough, and does not wait for mana that never comes.
inline WorldTickSnapshotView ManaView(std::uint32_t powerPct)
{
    WorldTickSnapshotView view = PartyView({ 10 });
    view.bots[0].power_type = 0;
    view.bots[0].max_power = 100;
    view.bots[0].power = powerPct;
    return view;
}




// A cast result as the next snapshot reports it: spell, status, with a new sequence number.
inline void Result(WorldTickSnapshotView& view, std::uint32_t spell, BotActionStatus status, std::uint32_t sequence)
{
    view.bots[0].last_action_sequence = sequence;
    view.bots[0].last_action_type = uint8_t(ActionType::kCastSpell);
    view.bots[0].last_action_spell = spell;
    view.bots[0].last_action_status = status;
}

inline WorldTickSnapshotView HunterView(std::uint64_t tick, std::uint32_t level, std::vector<NearbyUnit> nearby, std::uint64_t threat = 0)
{
    // A far creature too strong to pull keeps the bot from being idle (after 10 s of nothing in view it explores on foot, and is then not standing).
    NearbyUnit strong = Hostile(99, 35.0f, 100);
    strong.level = 60;
    nearby.push_back(strong);
    WorldTickSnapshotView view = View(tick, threat, std::move(nearby));
    view.bots[0].level = level;
    return view;
}








// ---- new behaviour: kiting, fleeing, mounting ----





























// ---- weakest target, interrupt sharing, heal sharing, party buffs ----














// ---- layout 13: auras, pet, lost-target reason, vendor guid ----








// ---- Supplies (layout 15, supply_policy.h) ----
// A bot short of ammunition, food or drink walks to a vendor that sells it, buys there, backs off when a trip bought nothing, and eats or drinks
// while it rests.
inline BotPolicy SupplyPolicy(std::uint32_t classId = 0)
{
    PolicyOptions options = AggressiveOptions();
    options.ClassId = classId;
    options.WanderWhenIdle = false; // an idle bot would take an exploring walk; these tests look at trips, rests and eating
    options.Supplies.Enabled = true;
    return BotPolicy(options);
}

// A hunter at the origin with ammunition running low, 500 copper and room in the bags. The ammunition vendor stands at x = 100, an ordinary vendor on
// the other side at x = -50.
inline WorldTickSnapshotView ShopperView(std::uint32_t ammo, std::uint32_t food = 20)
{
    WorldTickSnapshotView view = PartyView({ 10 });
    BotSnapshot& bot = view.bots[0];
    bot.level = 10;
    bot.ammo_entry = 2512;
    bot.ammo_count = ammo;
    bot.food_count = food;
    bot.copper = 500;
    bot.bag_free_slots = 5;
    bot.ammo_vendor_x = 100.0f;
    bot.ammo_vendor_valid = 1;
    bot.ammo_vendor_guid = MakeUnitRef(8, 4553, 31);
    bot.vendor_x = -50.0f;
    bot.vendor_valid = 1;
    return view;
}

// A copy of the first action of this type for bot 10 (a pointer into the vector that Decide returned would dangle once the call ends).
inline std::optional<BotAction> FindAction(std::vector<BotAction> const& actions, ActionType type)
{
    for (BotAction const& action : actions)
        if (action.type == type && action.bot_guid == 10)
            return action;
    return std::nullopt;
}












// ---- Potions and bandages (layout 16) ----
inline BotPolicy GoodsPolicy(std::uint32_t classId = 1)
{
    PolicyOptions options = AggressiveOptions();
    options.ClassId = classId;
    options.WanderWhenIdle = false;
    options.Supplies.Enabled = true;
    options.Supplies.Potions = true;
    options.Supplies.Bandages = true;
    return BotPolicy(options);
}

// A warrior with food and money and nothing else, and a goods vendor 2 yards away.
inline WorldTickSnapshotView GoodsView()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    BotSnapshot& bot = view.bots[0];
    bot.level = 20;
    bot.food_count = 10;
    bot.copper = 500;
    bot.bag_free_slots = 5;
    bot.goods_vendor_valid = 1;
    bot.goods_vendor_x = 2.0f;
    bot.goods_vendor_guid = MakeUnitRef(8, 5100, 41);
    return view;
}










// ---- Flight points (layout 17, taxi_policy.h) ----
inline BotPolicy TaxiPolicy()
{
    PolicyOptions options = AggressiveOptions();
    options.HubsByMap[1] = { Point{ 3000.0f, 0.0f, 0.0f } };   // a hub far away: the idle bot's walk is long
    options.Taxi.Enabled = true;
    return BotPolicy(options);
}

inline WorldTickSnapshotView TaxiView(float x)
{
    WorldTickSnapshotView view = OnMap(1);
    view.bots[0].pos_x = x;
    view.bots[0].taxi_master_valid = 1;
    view.bots[0].taxi_master_x = 100.0f;
    view.bots[0].taxi_master_guid = MakeUnitRef(8, 3838, 5);
    return view;
}

inline std::optional<BotAction> FirstOf(std::vector<BotAction> const& actions, ActionType type)
{
    for (BotAction const& action : actions)
        if (action.type == type && action.bot_guid == 10)
            return action;
    return std::nullopt;
}



 

// ---- Realism without combat (docs/bot-realism-backlog.md items 2, 4 and 5) ----

// Item 2: a bot that waits for its flight to start fidgets, only while it is idle.
inline std::size_t FidgetsInWait(BotPolicy& policy, WorldTickSnapshotView const& view, std::uint64_t from, std::uint64_t to)
{
    std::size_t n = 0;
    for (std::uint64_t t = from; t <= to; t += 500)
        for (BotAction const& action : policy.Decide(view, t))
            if (action.bot_guid == 10 && (action.type == ActionType::kStop || action.type == ActionType::kMoveTo))
                ++n;
    return n;
}

inline BotPolicy FidgetPolicy(bool enabled)
{
    PolicyOptions options = AggressiveOptions();
    options.HubsByMap[1] = { Point{ 3000.0f, 0.0f, 0.0f } };
    options.Taxi.Enabled = true;
    options.Fidget.Enabled = enabled;
    options.Fidget.ChancePct = 100;
    options.Fidget.MinGapSec = 1;
    options.Fidget.MaxGapSec = 1;
    options.Fidget.StandWeight = 1;
    options.Fidget.StepWeight = 1;
    options.Fidget.TurnWeight = 1;
    return BotPolicy(options);
}



// Item 4: the passing-player buff.
inline PolicyOptions BuffOptions()
{
    std::istringstream file("class 11\nrow 5176 always target range 0 30\nbuff 1126 duration 1800 party\n");
    std::vector<std::string> drop;
    PolicyOptions options = AggressiveOptions();
    options.Rotations = std::make_shared<rotation::RotationData>(rotation::ParseRotationData(file, drop));
    std::istringstream costs("1126\t1\t0\t200\t0\t0\t0\tMark\n5176\t1\t0\t2000\t0\t0\t0\tWrath\n");
    std::vector<std::string> warnings;
    options.SpellCosts = std::make_shared<spell_cost::SpellCostData>(spell_cost::ParseSpellCosts(costs, warnings));
    options.ClassOf = [](std::uint64_t) { return 11; };
    options.PassingBuff.Enabled = true;
    options.PassingBuff.ChancePct = 100;
    options.PassingBuff.CooldownSec = 600;
    options.PassingBuff.ZonePerMin = 10;
    return options;
}

inline WorldTickSnapshotView PasserView(std::uint32_t power)
{
    WorldTickSnapshotView view = PartyView({ 10 }, [](std::uint64_t) { return 11u; });
    view.bots[0].level = 10;
    view.bots[0].power = power;
    view.bots[0].max_power = 5000;
    view.bots[0].nearby_count = 1;
    view.nearby[0] = Unit(2, 900, 8.0f, false);                  // a player 8 yards away, not one of ours
    return view;
}

// The buff casts that go to the player (not the bot's own upkeep).
inline std::size_t BuffsOnThePlayer(std::vector<BotAction> const& actions)
{
    std::size_t n = 0;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kCastSpell && action.spell_id == 1126 && action.target_guid == MakeUnitRef(2, 1, 900))
            ++n;
    return n;
}

inline std::size_t PasserBuffsOver(BotPolicy& policy, WorldTickSnapshotView const& view, std::uint64_t from, std::uint64_t to)
{
    std::size_t n = 0;
    for (std::uint64_t t = from; t <= to; t += 3000)
        n += BuffsOnThePlayer(policy.Decide(view, t));
    return n;
}



// Item 5: town AFK.
inline PolicyOptions AfkOptions(std::uint32_t zoneMax)
{
    PolicyOptions options = AggressiveOptions();
    options.HubsByMap[1] = { Point{ 500.0f, 0.0f, 0.0f }, Point{ 900.0f, 0.0f, 0.0f } };
    options.TownAfk.Enabled = true;
    options.TownAfk.ChancePct = 100;
    options.TownAfk.MinMin = 5;
    options.TownAfk.MaxMin = 5;
    options.TownAfk.ZoneMax = zoneMax;
    return options;
}




// --- The Holy priest of the level 30 guide: Renew on the tank, Binding Heal, Prayer of Healing, no shield on a rage user; a roster spec sets the role ---

inline WorldTickSnapshotView LeveledDungeonView(std::uint32_t level, std::function<std::uint32_t(std::uint64_t)> classOf = nullptr)
{
    WorldTickSnapshotView view = DungeonView({ 10, 11, 12, 13, 14 }, classOf);
    for (BotSnapshot& bot : view.bots)
        bot.level = level;
    return view;
}







// --- 0.44.18 tests: the hold and regroup logs, the cost log, the pull-hold cap, and a module with no scenario ---

// A held party of five with a hurt mana user (bot 10) that has food and drink; returns the lines logged over `untilMs` of 1 s ticks.
// With recovering, bot 12's mana climbs 60 points a tick and starts over every 10 ticks (a rise of 8 % of the party's summed maximum within 10 s counts as progress), so the hold keeps seeing progress and does not give up at its 45 s cap.
inline std::vector<std::string> HeldPartyLog(bool held, std::uint64_t untilMs, bool recovering = false)
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    options.Supplies.Enabled = true;
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(held);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 }, WarriorLeads);
    for (BotSnapshot& bot : view.bots)
    {
        bot.group_role = bot.guid == 12 ? 2 : 1;
        bot.group_leader_guid = (std::uint64_t(2) << 56) | 12;
        bot.max_power = 0;
        if (bot.guid == 12)
        {
            bot.power_type = 0;
            bot.max_power = 1000;
            bot.power = 50;
            bot.drink_count = 5;
            bot.food_count = 3;
        }
    }
    for (std::uint64_t at = 40000; at <= untilMs; at += 1000)
    {
        if (recovering)
            for (BotSnapshot& bot : view.bots)
                if (bot.guid == 12)
                    bot.power = 50 + std::uint32_t(((at - 40000) / 1000) % 10) * 60;
        policy.Decide(view, at);
    }
    return events;
}

inline std::size_t CountLines(std::vector<std::string> const& events, std::string const& prefix)
{
    std::size_t n = 0;
    for (std::string const& line : events)
        n += line.rfind(prefix, 0) == 0;
    return n;
}

} // namespace

void RunPolicyTestsPart1();
void RunPolicyTestsPart2();
void RunPolicyTestsPart3();
void RunPolicyTestsPart4();
void RunPolicyTestsPart5();
void RunPolicyTestsPart6();
void RunPolicyTestsPart7();
void RunPolicyTestsPart8();
