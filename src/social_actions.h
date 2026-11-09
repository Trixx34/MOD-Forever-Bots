// SPDX-License-Identifier: GPL-2.0-or-later
// Duel, battleground, guild and gift-mail orders: what the policy decides (Order), how an order is sent through the SDK's ModuleClient (Send), and what
// the core answered (Outcome). The policy runs on the pump thread and only queues orders; the Run() thread sends them (main_commands.cpp), because client
// calls belong to it. Settings Duels, Battlegrounds, GuildJoin and MailGifts are off by default; the actions need a core that knows them (SDK
// ActionRequest fields 23 to 26), and an older core refuses them, which the policy logs as duel_gap, battleground_gap, guild_gap or gift_gap.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace forever_module {
class ModuleClient;
}

namespace social {

enum class Kind : std::uint8_t { Duel, Battleground, Guild, Gift };

// Op values match the SDK enums: DuelOp (0 challenge, 1 accept, 2 decline), BattlegroundOp (0 queue, 1 leave queue, 2 leave), GuildOp (0 accept,
// 1 decline, 2 leave). A gift has no op.
struct Order
{
    Kind What = Kind::Duel;
    std::uint64_t Bot = 0;
    std::uint32_t Op = 0;
    std::uint64_t Player = 0;       // Duel: the other duelist. Gift: the recipient (raw character guid counters)
    std::uint32_t Id = 0;           // Battleground: the BattlemasterList id
    std::uint64_t Seed = 0;         // Gift: spreads the choice of the stack
    std::string Text;               // Guild create (op 3): the new guild's name
};

struct Outcome
{
    Kind What = Kind::Duel;
    std::uint64_t Bot = 0;
    std::uint32_t Op = 0;
    bool Success = false;
    std::string Reason;
};

using Done = std::function<void(Outcome const&)>;

// Sends the order through the client. `done` is called from the client's thread when the core answers (or, for a gift, when no stack was fit to give).
// Returns false when nothing could be sent at all (an order with no bot).
bool Send(forever_module::ModuleClient& client, Order const& order, Done done);

// The name of the event line for a refused or unsendable order ("duel_gap", "battleground_gap", "guild_gap", "gift_gap").
char const* GapEventName(Kind kind);
// Whether a core's reason means it does not know the action at all (an older core), as against refusing this one request.
bool IsUnknownActionReason(std::string const& reason);

// BattlemasterList ids the policy queues for (3.3.5 numbering): 1 Alterac Valley, 2 Warsong Gulch, 3 Arathi Basin, 7 Eye of the Storm.
// The choice spreads bots over the battlegrounds their level allows.
std::uint32_t PickBattleground(std::uint32_t level, std::uint64_t seed);
// Whether a map id is a battleground map (30 Alterac Valley, 489 Warsong Gulch, 529 Arathi Basin, 566 Eye of the Storm).
bool IsBattlegroundMap(std::uint32_t mapId);

}  // namespace social
