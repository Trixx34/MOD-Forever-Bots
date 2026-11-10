// SPDX-License-Identifier: GPL-2.0-or-later
// What the advisor features decide (emote, dungeon finder join, party / guild / Trade line). The policy queues these on the pump thread; the Run() thread
// sends them through advisor_send.h, which is the only file that names the protocol 21 SDK calls.
#pragma once

#include <cstdint>
#include <string>

namespace advisor {

enum class Kind : std::uint8_t { Emote, LfgJoin, PartySay, GuildSay, TradeSay, Say };

struct Order
{
    Kind What = Kind::PartySay;
    std::uint64_t Bot = 0;
    std::uint64_t Target = 0;      // Emote: the unit the bot faces (raw guid counter), 0 for none
    std::uint32_t Arg = 0;         // Emote: 0 salute, 1 applaud, 2 cheer, 3 wave, 4 bow (EmoteAction::EmoteKind). LfgJoin: the role, 1 tank, 2 healer, 4 dps
    std::uint32_t Map = 0;         // LfgJoin: the dungeon map
    std::string Text;              // the *Say kinds (Say: /say, Target is the player addressed)
};

}  // namespace advisor
