// SPDX-License-Identifier: GPL-2.0-or-later
// Bot trade (plan: .opencode/docs/plan-bot-trade.md). The decisions of a trade with a bot, with no SDK dependency so they are unit-tested
// on their own (tests/trade_test.cpp). main.cpp feeds it the trade events, the bag list and the whispers, and carries out the steps it
// returns. Everything here runs on the client thread (the thread of the SDK callbacks), so there is no locking.
//
// The flow: a player proposes a trade -> the bot opens the window and asks the core for its tradeable items -> it whispers them as numbered
// pages -> the player whispers back what it wants (numbers or names) and the bot puts those stacks in the window -> the bot presses Accept
// when the player has. Items the player offers are accepted the same way. A trade that sits idle is cancelled.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace trade {

// One item the bot could trade (the core's list-bags reply).
struct BagItem
{
    std::uint64_t ItemGuid = 0;
    std::uint32_t Entry = 0;
    std::uint32_t Count = 0;
    std::string Name;
};

// BotTradeEvent.status in the SDK.
enum class Status : std::uint32_t
{
    Requested = 0,
    Opened = 1,
    Changed = 2,
    PartnerAccepted = 3,
    Cancelled = 4,
    Completed = 5,
};

// One thing for main.cpp to do.
struct Step
{
    enum class Kind { Begin, ListBags, Whisper, SetItem, Accept, Cancel, Regear };
    Kind What = Kind::Whisper;
    std::uint64_t Bot = 0;
    std::uint64_t Partner = 0;  // Whisper: the player to whisper to
    std::uint32_t Slot = 0;     // SetItem: the trade slot (0 to 5)
    std::uint64_t ItemGuid = 0; // SetItem
    std::string Text;           // Whisper: the line. SetItem: the item's name, for the line when the core refuses it
};

// The trade window holds six items.
constexpr std::uint32_t kTradeSlots = 6;

// A chat line is at most 255 bytes; the pages stay under this.
constexpr std::size_t kWhisperBytes = 240;

// The bag list as numbered pages ("[1/2] 1) Linen Cloth x20; 2) ..."). The numbers run on across pages and are what a reply refers to.
std::vector<std::string> ListWhispers(std::vector<BagItem> const& items, std::size_t maxBytes = kWhisperBytes);

// What a player's reply asks for.
struct Choice
{
    std::size_t Index = 0;     // into the item list
    std::uint32_t Count = 0;   // the count the player named (0 = none named); only whole stacks can be traded
};

struct Reply
{
    std::vector<Choice> Chosen;
    std::vector<std::string> NotFound;   // words that matched no item
    std::vector<std::string> Ambiguous;  // words that matched several differently named items, with the names
    bool All = false;                    // "all"
    bool Cancel = false;                 // "cancel"
};

// Reads a reply: pieces split at commas, semicolons and "and"; each piece is a number, "2 x3", a name, "3 linen cloth", "linen cloth x3",
// "all" or "cancel". A name matches exactly, then by its start, then anywhere in the name (case does not matter).
Reply ParseReply(std::string const& text, std::vector<BagItem> const& items);

class TradeBook
{
public:
    explicit TradeBook(std::uint64_t idleMs = 120000) : _idleMs(idleMs) {}

    // A trade event for a bot. Requested opens the bot's side and asks for the bags; PartnerAccepted presses Accept; Cancelled and
    // Completed end the trade (Completed thanks the player and asks for a gear pass: the bot may have been given something to wear).
    std::vector<Step> OnEvent(std::uint64_t bot, std::uint64_t partner, Status status, std::uint64_t nowMs);

    // The core's list-bags reply: whispers the items as pages.
    std::vector<Step> OnBagList(std::uint64_t bot, std::vector<BagItem> items, std::uint64_t nowMs);

    // A whisper to a bot. Empty unless the bot is in a trade with the speaker and has listed its items (anything else is ordinary chat).
    std::vector<Step> OnWhisper(std::uint64_t speaker, std::uint64_t bot, std::string const& text, std::uint64_t nowMs);

    // The core refused to put an item in the window: the line for the player.
    std::vector<Step> OnItemRefused(std::uint64_t bot, std::string const& itemName);

    // Cancels the trades that have been idle too long.
    std::vector<Step> OnIdle(std::uint64_t nowMs);

    // Whether the bot is in a trade, and with whom (0 when it is not).
    bool Active(std::uint64_t bot) const { return _sessions.count(bot) != 0; }
    std::uint64_t PartnerOf(std::uint64_t bot) const;

private:
    struct Session
    {
        std::uint64_t Partner = 0;
        std::vector<BagItem> Items;
        bool Listed = false;
        std::uint32_t NextSlot = 0;
        std::set<std::size_t> Placed;   // indexes of Items already in the window
        std::uint64_t LastMs = 0;
    };

    std::uint64_t _idleMs;
    std::map<std::uint64_t, Session> _sessions;
};

} // namespace trade
