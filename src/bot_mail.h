// SPDX-License-Identifier: GPL-2.0-or-later
// Bot-to-bot mail (config key BotMail, off by default): bots mail gold and crafting materials to the bots of their own guild, and hand on a piece of gear
// they will not wear to a guildmate who would. Pure logic, unit-tested in tests/bot_mail_test.cpp. The policy (DriveMail, bot_policy_mail.cpp) turns the
// plan into social::Order values; social_actions.cpp sends them through ModuleClient::BotGiftMail (the core's gift action: no mailbox visit, the core
// moves the item or money and charges the postage). The design and the core's limits are in doc/bot-trade-mail.md.
//
// Who may receive: only a member of the sender's own guild (guild_guid in the snapshot) of the same faction. "Alts" (characters of one account) need an
// account id the snapshot does not carry yet, so they are not told apart. A live trade between two bots needs a core action (a trade started by a bot)
// that does not exist: mail is the only channel, and it has the mail delivery delay (the core's Mail.DeliveryDelay, 3600 s by default).
//
// Gear is judged by gear_score.h's score (item level x 10 plus weighted stats), never by item id. The facts it needs (inventory type, item level, armour
// subclass, required level, and what each recipient wears) are not in the module's inputs yet, so PickGearRecipient is ready and tested but the policy
// does not call it (doc/bot-trade-mail.md, "What the core has to add").
#pragma once

#include "gear_score.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace botmail {

struct Settings
{
    bool Enabled = false;                          // BotMail
    bool Gold = true;                              // BotMailGold: poor guildmates get gold from rich ones
    bool Materials = true;                         // BotMailMaterials: gathered goods go to a guildmate whose profession uses them
    std::uint32_t DailyCap = 2;                    // BotMailDailyCap: mails per sender per day (0 stops all)
    std::uint32_t CooldownSec = 3600;              // BotMailCooldownSec: between two mails of one sender
    std::uint32_t RecipientDailyCap = 3;           // BotMailRecipientDailyCap: mails one bot may receive per day
    std::uint32_t PlanEverySec = 120;              // BotMailPlanEverySec: how often the pool is looked at
    std::uint32_t MaxPerPlan = 2;                  // BotMailMaxPerPlan: orders queued by one plan
    std::uint64_t GoldKeepCopper = 50000;          // BotMailGoldKeepCopper: a sender never goes below this (and never sends what it is saving)
    std::uint64_t GoldMinCopper = 10000;           // BotMailGoldMinCopper: a smaller gift is not worth the postage and the delay
    std::uint64_t GoldMaxCopper = 10000;           // BotMailGoldMaxCopper: per mail; the core refuses more than its Bot.Social.GiftMaxCopper (10000 by default)
    std::uint64_t NeedyBelowCopper = 20000;        // BotMailNeedyBelowCopper: a guildmate with less than this may be given gold
    std::uint32_t MaterialMinCount = 5;            // BotMailMaterialMinCount: a stack smaller than this stays where it is
    std::uint32_t MaterialMaxCount = 200;          // BotMailMaterialMaxCount: and a larger one (the core sends whole stacks and caps their vendor value)
};

enum class Kind : std::uint8_t { Gold, Materials, Gear };

// What the planner knows about one online bot.
struct Member
{
    std::uint64_t Guid = 0;
    std::uint64_t Guild = 0;           // 0 = none
    std::uint8_t Side = 0;             // 1 Alliance, 0 Horde (the core refuses a mail across factions)
    std::uint32_t ClassId = 0;
    std::uint32_t Level = 1;
    std::uint64_t Copper = 0;
    std::array<std::uint16_t, 2> Skill{ 0, 0 };   // the two primary professions (skill line ids), 0 = none
    bool Free = false;                 // alive, out of combat, not in flight, not in a party
};

struct Offer
{
    Kind What = Kind::Gold;
    std::uint64_t Sender = 0;
    std::uint64_t Recipient = 0;
    std::uint64_t Copper = 0;                      // Gold: how much
    std::array<std::uint16_t, 2> RecipientSkill{ 0, 0 };   // Materials: what the recipient's professions are
    std::array<std::uint16_t, 2> SenderSkill{ 0, 0 };      // Materials: what the sender's are (it keeps what it uses itself)
};

// Per-bot counters (a day is kDayMs from the first mail of the day).
constexpr std::uint64_t kDayMs = 86400000ull;
struct Count
{
    std::uint64_t DayStartMs = 0;
    std::uint32_t Today = 0;
};
struct State
{
    std::uint64_t PlanAtMs = 0;
    std::map<std::uint64_t, std::uint64_t> LastSentMs;   // sender -> last order
    std::map<std::uint64_t, Count> Sent;                 // sender -> mails today
    std::map<std::uint64_t, Count> Received;             // recipient -> mails today
    std::uint64_t Cursor = 0;                            // rotates the walk over the senders, so the same bots do not always go first
};

// The skill line ids of the professions (SkillLine.dbc) the table below uses.
constexpr std::uint16_t kAlchemy = 171, kBlacksmithing = 164, kEnchanting = 333, kEngineering = 202, kHerbalism = 182, kLeatherworking = 165,
                        kMining = 186, kSkinning = 393, kTailoring = 197, kCooking = 185, kFirstAid = 129;

// The professions that use a crafting material, by item entry (the items classic gatherers and skinners bring home). Empty when the entry is not
// a known material. Entries are item ids of materials, not of gear.
std::vector<std::uint16_t> ConsumersOf(std::uint32_t entry);

// One stack of a bot's bags (BotTrade kListBags).
struct Stack
{
    std::uint64_t ItemGuid = 0;
    std::uint32_t Entry = 0;
    std::uint32_t Count = 0;
    std::string Name;
};

// The stack to mail to a bot with `recipientSkill`: a known material one of its professions uses, that no profession of the sender uses, whole, between
// MaterialMinCount and MaterialMaxCount; the biggest such stack, the lowest entry on a tie. Index into `stacks`, or -1.
int PickMaterial(Settings const& settings, std::vector<Stack> const& stacks, std::array<std::uint16_t, 2> const& recipientSkill,
                 std::array<std::uint16_t, 2> const& senderSkill);

// The mails to send now. Honours the plan period, every cap and the cooldown, and records the orders in `state` (call NoteFailed to give a slot back).
std::vector<Offer> Plan(Settings const& settings, State& state, std::vector<Member> const& members, std::uint64_t nowMs);

// An order the core refused or that found nothing to send: the day's slot comes back (the cooldown stays, so the same pair is not retried at once).
void NoteFailed(State& state, std::uint64_t sender, std::uint64_t recipient);

// Subjects, 1 to 64 bytes.
std::string GoldSubject(std::uint64_t copper);
std::string MaterialSubject(std::string const& itemName);
std::string GearSubject(std::string const& itemName);

// ---- Gear: the exact rule, ready for the core's facts (not called by the policy yet) ----

// A candidate for a piece of gear: what the guildmate wears in the slot (the one or two items of that type, as JudgeBagGear takes them) and what it is.
struct GearCandidate
{
    std::uint64_t Guid = 0;
    std::uint32_t ClassId = 0;
    std::uint32_t Level = 0;
    std::uint32_t Role = 0;                        // 0 = what the class gives (gear_score::ArchetypeOf)
    std::vector<gear_score::Item> Worn;            // the items worn in the slots this type fills (empty slot: leave out; WornSlots says how many exist)
    std::size_t WornSlots = 1;                     // 1, or 2 for rings, trinkets and one-hand weapons
};

// The best armour subclass a class wears at a level (1 cloth, 2 leather, 3 mail, 4 plate); 0 for a class id the table does not know.
std::uint32_t BestArmorSubclass(std::uint32_t classId, std::uint32_t level);

// A guildmate to give `item` to: it can wear it (armour class and subclass, required level), the item beats what it wears in the slot by at least
// gear_score::kSellMarginScore (an empty slot counts as an upgrade), and the sender itself has no use for it (`senderWouldWear` false). The biggest
// improvement wins, the lowest guid on a tie. Index into `candidates`, or -1. Soulbound or quest items are never offered (`item.QuestItem`, `bound`).
constexpr std::int32_t kEmptySlotScore = 0;
int PickGearRecipient(gear_score::Item const& item, bool bound, bool senderWouldWear, std::vector<GearCandidate> const& candidates);

}  // namespace botmail
