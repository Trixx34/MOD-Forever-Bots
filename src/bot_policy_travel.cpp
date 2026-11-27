// SPDX-License-Identifier: GPL-2.0-or-later
// BotPolicy: the detour over known ways (TravelGraph, travel_graph.h, doc/travel-graph.md) and the memory of the places a bot keeps dying at (DeathPlaceMemory,
// doc/level-safe.md). Both default off.
#include "bot_policy_internal.h"

// A long walk whose chain of legs got nowhere (PartialChainTick): true when a known way leads on, so the walk is not refused; the next leg replaces the destination
// when the walk is sent again (TravelApply).
bool BotPolicy::TravelDetour(BotState& state, BotSnapshot const& bot, std::uint64_t nowMs)
{
    if (!Opt().TravelGraph || bot.life_state != 0)
        return false;
    BotTravel& travel = state.Travel;
    Point const dest = state.PartialDest;
    // The leg itself had no route: the known way is wrong here, the leg is refused as any walk.
    if (travel.Active && std::hypot(dest.X - travel.Via.X, dest.Y - travel.Via.Y) < kNoRouteSameYards)
    {
        travel.Active = false;
        LogEvent("travel_detour: bot=" + std::to_string(bot.guid) + " result=leg_failed legs=" + std::to_string(travel.Legs));
        return false;
    }
    bool const same = travel.Map == bot.map_id && std::hypot(travel.Final.X - dest.X, travel.Final.Y - dest.Y) < kNoRouteSameYards &&
                      nowMs < travel.UntilMs;
    if (!same)
        travel = BotTravel{};
    auto const leg = _travel.Next(bot.map_id, { bot.pos_x, bot.pos_y, bot.pos_z }, { dest.X, dest.Y, dest.Z });
    std::string const who = "travel_detour: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " dest=" + std::to_string(int(dest.X)) + "," +
                            std::to_string(int(dest.Y)) + " dist=" + std::to_string(int(std::hypot(bot.pos_x - dest.X, bot.pos_y - dest.Y)));
    // No way known, no nearer along it than the last leg was, or too many legs: the walk is refused.
    if (!leg || (same && (leg->Remaining > travel.Remaining - kTravelGainYards || travel.Legs >= kTravelLegsMax)))
    {
        travel.Active = false;
        LogEvent(who + " result=" + (!leg ? "no_way" : "no_progress") + " nodes=" + std::to_string(_travel.Nodes()));
        return false;
    }
    travel.Active = true;
    travel.Map = bot.map_id;
    travel.Final = dest;
    travel.Via = Point{ leg->Point.X, leg->Point.Y, leg->Point.Z };
    travel.Remaining = leg->Remaining;
    travel.UntilMs = nowMs + kTravelMs;
    ++travel.Legs;
    LogEvent(who + " result=leg legs=" + std::to_string(travel.Legs) + " via=" + std::to_string(int(travel.Via.X)) + "," + std::to_string(int(travel.Via.Y)) +
             " remaining=" + std::to_string(int(leg->Remaining)) + " nodes=" + std::to_string(_travel.Nodes()));
    state.Last.reset(); // the walk is sent again
    return true;
}

// A walk about to be sent to the destination of a detour goes to the leg instead. At the leg's end the destination itself is tried again.
void BotPolicy::TravelApply(BotState& state, BotSnapshot const& bot, Decision& wanted, std::uint64_t nowMs) const
{
    BotTravel& travel = state.Travel;
    if (!Opt().TravelGraph || !travel.Active || wanted.Type != ActionType::kMoveTo || wanted.Target != 0 || bot.life_state != 0 || travel.Map != bot.map_id ||
        std::hypot(wanted.Dest.X - travel.Final.X, wanted.Dest.Y - travel.Final.Y) >= kNoRouteSameYards)
        return;
    if (nowMs >= travel.UntilMs || std::hypot(bot.pos_x - travel.Via.X, bot.pos_y - travel.Via.Y) <= kPartialArrivedYards)
    {
        travel.Active = false;
        return;
    }
    wanted.Dest = travel.Via;
}

void BotPolicy::NoteDeathPlace(BotState& state, std::uint32_t map, float x, float y, std::uint64_t nowMs) const
{
    if (!Opt().DeathPlaceMemory)
        return;
    std::erase_if(state.DeathPlaces, [&](BotDeathSpot const& place)
    { return nowMs - place.AtMs >= kDeathPlaceMs || (place.Map == map && std::hypot(place.X - x, place.Y - y) < kDeathPlaceSameYards); });
    if (state.DeathPlaces.size() >= kDeathPlaceMax)
        state.DeathPlaces.erase(state.DeathPlaces.begin());
    state.DeathPlaces.push_back(BotDeathSpot{ map, x, y, nowMs });
}

// Grind spots near a remembered place are left out, unless that leaves none.
std::vector<std::size_t> BotPolicy::DeathPlaceFilter(BotState& state, BotSnapshot const& bot, std::vector<std::size_t> spots, std::uint64_t nowMs)
{
    if (!Opt().DeathPlaceMemory || state.DeathPlaces.empty())
        return spots;
    std::vector<std::size_t> kept;
    for (std::size_t const index : spots)
    {
        GrindSpot const& spot = Opt().Grind->Spots[index];
        if (std::none_of(state.DeathPlaces.begin(), state.DeathPlaces.end(), [&](BotDeathSpot const& place)
            { return nowMs - place.AtMs < kDeathPlaceMs && place.Map == spot.Map && std::hypot(place.X - spot.X, place.Y - spot.Y) <= kDeathPlaceYards; }))
            kept.push_back(index);
    }
    if (kept.size() != spots.size() && (state.DeathPlaceLoggedMs == 0 || nowMs - state.DeathPlaceLoggedMs >= 60000))
    {
        state.DeathPlaceLoggedMs = nowMs;
        LogEvent("death_place_skip: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " skipped=" +
                 std::to_string(spots.size() - kept.size()) + " left=" + std::to_string(kept.size()) + " places=" + std::to_string(state.DeathPlaces.size()));
    }
    return kept.empty() ? spots : kept;
}
