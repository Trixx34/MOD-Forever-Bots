// SPDX-License-Identifier: GPL-2.0-or-later
// Dungeon and group etiquette (config GroupEtiquette, off by default). Three small rules a human group keeps, all pure logic (tests/group_etiquette_test.cpp):
//   - WAIT: a route party does not start the next pull while its healer or tank is not ready (healer mana, tank health at GroupEtiquetteReadyPct), while a member is a
//     straggler farther than GroupEtiquetteStragglerYards (for at most GroupEtiquetteStragglerSec), and never for longer than GroupEtiquetteWaitSec in all. Dead members
//     and combat are not its business (the route's own dead-member wait and the combat code keep them). Drinking mana is the existing PartyMana / PullReadyManaPct wait.
//   - CHAT: one greeting when a bot finds itself in a dungeon party, one "ready?" from the party's leader when a wait ends, one "thanks" from a bot that was
//     dead and is up again. The lines ride the existing party chat order (advisor::Kind::PartySay); the limits here are per bot and per party.
//   - ROLL: need, greed or pass on a group loot roll from the item's upgrade score and class need; never need an item a visible groupmate needs more.
//     NOT WIRED: the core sends no roll event and has no roll action (protocol 29); see doc/group-etiquette.md for what the core would add.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace group_etiquette {

struct Settings
{
    bool Enabled = false;                    // GroupEtiquette: the wait gate and the chat lines
    bool Loot = false;                       // GroupEtiquetteLoot: the roll rule (takes effect once the core sends roll events)
    std::uint32_t WaitSec = 45;              // GroupEtiquetteWaitSec: the cap of one wait, over all reasons
    std::uint32_t ReadyPct = 80;             // GroupEtiquetteReadyPct: the healer's mana and the tank's health the party waits for
    std::uint32_t StragglerYards = 45;       // GroupEtiquetteStragglerYards: a member farther than this from the leader is a straggler
    std::uint32_t StragglerSec = 20;         // GroupEtiquetteStragglerSec: how long the party waits for one
};

// ---- wait ----
struct Member
{
    std::uint64_t Bot = 0;
    bool Tank = false;
    bool Healer = false;
    bool Alive = true;
    bool ManaUser = false;
    std::uint32_t HealthPct = 100;
    std::uint32_t ManaPct = 100;
    float LeaderYards = 0.0f;                // distance to the party leader
};

enum class Hold : std::uint8_t { None, Healer, Tank, Straggler };
char const* HoldName(Hold hold);

// The reason to wait now, by the party's members as they are this tick (a hold is not capped here). A dead member never holds.
Hold Evaluate(Settings const& settings, std::vector<Member> const& members);

// One per party (keyed by its leader).
class Waiter
{
public:
    // Whether the party waits now. `inCombat`: never waits. The straggler reason ends after StragglerSec and every reason after WaitSec; a released episode
    // stays released until the party has been ready once. `ended` is set when a wait ended because everybody is ready (the moment for "ready?").
    bool Holding(Settings const& settings, std::vector<Member> const& members, bool inCombat, std::uint64_t nowMs, bool* ended = nullptr);

private:
    std::uint64_t _since = 0;                // when the episode began, 0 = none
    std::uint64_t _stragglerSince = 0;
    bool _active = false, _straggling = false;
    bool _released = false;
};

// ---- chat ----
enum class Line : std::uint8_t { Greet, Ready, Thanks };

struct ChatFacts
{
    bool InDungeonParty = false;
    bool Alive = true;
    bool Leader = false;
    std::uint32_t GroupSize = 0;
    bool GroupMateNear = false;              // a living member within 40 yards (for the thanks)
};

// Limits: a bot speaks at most once a minute and a party at most once per 12 seconds and 6 lines in 5 minutes. The greeting waits 2 to 9 seconds after the
// bot first sees the party (so the bots do not greet in one tick) and is said by at most two members of a party; "ready?" by the leader, once per 3 minutes;
// the thanks by a bot that was dead a moment ago, once per 2 minutes.
class Chatter
{
public:
    // Feed each bot of a dungeon party once per tick. True (with `line`) when it should speak now. Records the use of the limits only when it returns true.
    bool Note(Settings const& settings, ChatFacts const& facts, std::uint64_t seed, std::uint64_t bot, std::uint64_t party, std::uint64_t nowMs, Line& line);
    // The party's wait ended (see Waiter::Holding): the leader may say "ready?".
    void MarkReady(std::uint64_t party) { _readyDue[party] = true; }

private:
    struct PerBot
    {
        std::uint64_t FirstSeenMs = 0, LastSayMs = 0, ReadyOrThanksMs = 0, GreetedParty = ~0ull, SeenParty = ~0ull, DeadSinceMs = 0;
        bool WasDead = false;
    };
    struct PerParty
    {
        std::uint32_t Greets = 0;
        std::uint64_t LastSayMs = 0, ReadyMs = 0;
        std::vector<std::uint64_t> Recent;   // the times of its lines in the last 5 minutes
    };
    bool PartyMay(PerParty& party, std::uint64_t nowMs) const;
    std::map<std::uint64_t, PerBot> _bots;
    std::map<std::uint64_t, PerParty> _parties;
    std::map<std::uint64_t, bool> _readyDue;
};

// A pool line "kind|text" (kind greet, ready or thanks): the texts of one kind. Lines without a known kind are skipped.
std::vector<std::string> LinesOf(std::vector<std::string> const& pool, Line kind);

// ---- roll ----
enum class Roll : std::uint8_t { Pass, Greed, Need };
char const* RollName(Roll roll);

struct Rival
{
    std::uint64_t Bot = 0;
    bool Usable = false;                     // the groupmate's class can use the item
    std::int32_t Upgrade = 0;                // its upgrade score for that member (gear_score scale: item level x 10 plus weighted stats, candidate minus worn)
};

struct RollFacts
{
    std::uint64_t Bot = 0;
    std::uint8_t Quality = 0;                // 0 grey ... 4 epic
    bool Usable = false;                     // armor type, weapon skill and class restrictions allow it
    bool WantsStats = true;                  // its main stats suit the bot's spec (a warrior's spirit mail does not)
    bool BindOnPickup = false;
    std::int32_t Upgrade = 0;                // candidate score minus what is worn in that slot (an empty slot: the whole score)
    std::vector<Rival> Rivals;               // the groupmates whose needs are visible; unknown members are simply absent
};

constexpr std::int32_t kNeedMarginScore = 10;   // one item level: smaller gains are greed, not need

// Need only when it is a real upgrade for the bot's spec and no visible groupmate needs it more (a tie goes to the lower bot id, so two bots do not both
// need). Greed on a usable item that is no upgrade and on a bind-on-equip item nobody can use; pass on grey items and on a bind-on-pickup item the bot cannot use.
Roll DecideRoll(Settings const& settings, RollFacts const& facts);

}  // namespace group_etiquette
