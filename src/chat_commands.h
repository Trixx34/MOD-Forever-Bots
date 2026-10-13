// Chat commands for the bots (party chat, raid chat, or a whisper to one bot). The party leader types
//   [selector] <verb> [args]
// and the bots of the leader's group that the selector names obey. Pure parsing, unit-tested in tests/chat_test.cpp; BotPolicy applies them.
//
// Selectors: all | tank | healer | dps | <class> (also the plural), joined by commas for a union (healer,rogue). No selector means all.
//   tank = warrior; healer = priest, paladin, shaman, druid; dps = hunter, rogue, mage, warlock.
// Basic verbs (ChatCommands = 1):  follow [off] | stay [off] | goto here | release | status
// Orders (ChatOrders = 1):          stop | pull | heal | passive | aggressive | summon | revive | mount | dismount
// Control panel (ChatControl = 1):  role tank|healer|dps|auto | stance aggressive|defensive|passive | focus [off] | distance <yards>|default
//   follow: walk after the speaker. stay: hold the position (fights only what attacks it). stop: drop follow, goto and stay.
//   goto here: walk to where the speaker stands (a spread of positions, so the bots do not share one point). release: a dead bot releases its
//   spirit now. status: one line per bot, whispered back. pull: the tank attacks the speaker's selected target. heal: a healer casts its
//   heal on the speaker's selected friend. passive: fights only what attacks it. aggressive: back to normal.
//   summon: the bots walk (teleport) next to the speaker. revive: a dead bot is resurrected at 35% health and put next to the speaker. mount /
//   dismount: the fastest ground mount it knows. role: overrides what the class gives (tank pulls on `pull` and leads a dungeon party; dps does
//   not heal on its own). stance: aggressive fights what the group fights or meets, defensive only what attacks it or the group, passive only
//   what attacks the bot. focus: the speaker's selected target is attacked first in a fight. distance: how far from the speaker a following
//   bot stops (3 to 40 yards).
// Anything else is ordinary chat: ParseChatCommand returns nothing.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class ChatVerb : std::uint8_t
{
    None,
    Follow,
    FollowOff,
    Stay,
    StayOff,
    Stop,
    GotoHere,
    Release,
    Status,
    Pull,
    Heal,
    Passive,
    Aggressive,
    Summon,
    Revive,
    Mount,
    Dismount,
    Role,
    Stance,
    Focus,
    FocusOff,
    Distance,
};

// Orders need ChatOrders = 1, the control panel ChatControl = 1 (the basic verbs only need ChatCommands = 1).
bool IsOrderVerb(ChatVerb verb);
bool IsControlVerb(ChatVerb verb);
char const* ChatVerbName(ChatVerb verb);

struct ChatSelector
{
    bool All = true;                 // no selector given, or "all"
    std::uint32_t ClassMask = 0;     // bit n = class id n, when a selector was given
    bool Matches(std::uint32_t classId) const { return All || (classId < 32 && (ClassMask & (1u << classId)) != 0); }
};

struct ChatCommand
{
    ChatSelector Selector;
    ChatVerb Verb = ChatVerb::None;
    std::string Arg;       // role / stance: the word; distance: "default" or empty
    float Value = 0.0f;    // distance: the yards
};

// Parses a chat line. `orders` says whether the order verbs are accepted. Returns nothing for ordinary chat.
std::optional<ChatCommand> ParseChatCommand(std::string_view text, bool orders, bool control = false);

// Parses one selector token ("all", "healer", "rogues", "healer,rogue"). Returns false when it is not a selector.
bool ParseChatSelector(std::string_view token, ChatSelector& out);

// Offset from the speaker of the index-th bot of a "goto here": a spiral, so the bots do not stand on one point.
void ChatSpreadOffset(std::uint32_t index, float& dx, float& dy);
