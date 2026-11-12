// SPDX-License-Identifier: GPL-2.0-or-later
// Ride with the leader (config TransportRide, off by default; doc/transports.md): a bot in a group whose leader stands on a ship, zeppelin or elevator is put on
// the same transport (kTransport spell_id 3), and a bot that rides while its leader does not is taken off (spell_id 2). Pure decisions over plain facts, unit-tested in
// tests/transport_ride_test.cpp. The core refuses the action unless its Bot.Actions.Transport is on; a refusal backs the bot off for BackoffMs.
//
// A passenger is moved by the transport: a world-coordinate kMoveTo would put it off the deck, so the policy sends it no kMoveTo while it rides (kStop and kFollow
// only) and for one turn after a boarding order, when the snapshot still shows the old place.
#pragma once

#include <cstdint>

namespace transport_ride {

struct Settings
{
    bool Enabled = false;                    // TransportRide
    std::uint32_t EveryMs = 5000;            // TransportRideEveryMs: an order is not repeated before this
    std::uint32_t BackoffMs = 30000;         // TransportRideBackoffMs: the wait after the core refused one
};

enum class Order : std::uint32_t { kNone = 0, kJoin = 1, kLeave = 2 };

// The values of kTransport's spell_id.
constexpr std::uint32_t kBoardSpell = 1;
constexpr std::uint32_t kLeaveSpell = 2;
constexpr std::uint32_t kJoinSpell = 3;

// BotSnapshot::movement_flags bits (layout 24).
constexpr std::uint32_t kFlagSwimming = 1u << 0;
constexpr std::uint32_t kFlagOnTransport = 1u << 3;

struct Facts
{
    bool Alive = true;
    bool InCombat = false;
    bool InFlight = false;                   // on a taxi flight: never mixed with a transport
    bool Swimming = false;
    bool InGroup = false;
    bool IsLeader = false;                   // a leading bot never rides by itself (crossing the sea is a travel feature, not a follow feature)
    bool LeaderKnown = false;                // the leader's member entry was found in the party view and the leader is online
    bool BotOnTransport = false;             // movement_flags says on a transport, or transport_guid is set
    std::uint32_t BotTransportEntry = 0;     // 0 when none or not known
    std::uint32_t LeaderTransportEntry = 0;  // 0 when the leader stands on no transport
};

// What the bot should do now. The caller checks the pace (Due) and sends the order.
inline Order Decide(Settings const& settings, Facts const& facts)
{
    if (!settings.Enabled || !facts.Alive || facts.InFlight || !facts.InGroup || facts.IsLeader || !facts.LeaderKnown)
        return Order::kNone;
    if (facts.LeaderTransportEntry != 0)
    {
        if (facts.BotOnTransport && (facts.BotTransportEntry == 0 || facts.BotTransportEntry == facts.LeaderTransportEntry))
            return Order::kNone;             // aboard the leader's transport (or on one whose entry is not known: not touched)
        if (facts.InCombat || facts.Swimming)
            return Order::kNone;
        return Order::kJoin;                 // not aboard, or aboard another transport: the core moves the bot over
    }
    if (facts.BotOnTransport && !facts.InCombat)
        return Order::kLeave;                // the leader got off (or never was aboard): the bot does not stay aboard
    return Order::kNone;
}

// Whether the next order may be sent: not before `nextMs`.
inline bool Due(std::uint64_t nowMs, std::uint64_t nextMs)
{
    return nowMs >= nextMs;
}

// The time the next order may be sent after one was sent at nowMs (refused: the back-off).
inline std::uint64_t NextAfter(Settings const& settings, std::uint64_t nowMs, bool refused)
{
    return nowMs + (refused ? settings.BackoffMs : settings.EveryMs);
}

// Whether the bot rides: the movement flag or a transport entry.
inline bool OnTransport(std::uint32_t movementFlags, std::uint64_t transportEntry)
{
    return (movementFlags & kFlagOnTransport) != 0 || transportEntry != 0;
}

}  // namespace transport_ride
