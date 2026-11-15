// SPDX-License-Identifier: GPL-2.0-or-later
#include "group_etiquette.h"

#include "bot_random.h"

#include <algorithm>

namespace group_etiquette {

namespace {
constexpr std::uint64_t kBotGapMs = 60000;
constexpr std::uint64_t kPartyGapMs = 12000;
constexpr std::uint64_t kPartyWindowMs = 5 * 60000;
constexpr std::size_t kPartyWindowLines = 6;
constexpr std::uint64_t kReadyGapMs = 3 * 60000;
constexpr std::uint64_t kThanksGapMs = 2 * 60000;
constexpr std::uint64_t kThanksWindowMs = 90000;   // thanks only this long after the bot stood up
constexpr std::uint32_t kMaxGreeters = 2;
constexpr std::uint32_t kSaltGreet = 0xE1;
}  // namespace

char const* HoldName(Hold hold)
{
    switch (hold)
    {
        case Hold::Healer: return "healer";
        case Hold::Tank: return "tank";
        case Hold::Straggler: return "straggler";
        default: return "none";
    }
}

Hold Evaluate(Settings const& settings, std::vector<Member> const& members)
{
    Hold stragglers = Hold::None;
    Hold result = Hold::None;
    for (Member const& m : members)
    {
        if (!m.Alive)
            continue;
        if (m.Healer && m.ManaUser && m.ManaPct < settings.ReadyPct)
            result = Hold::Healer;
        else if (m.Tank && m.HealthPct < settings.ReadyPct && result == Hold::None)
            result = Hold::Tank;
        if (m.LeaderYards > float(settings.StragglerYards))
            stragglers = Hold::Straggler;
    }
    return result != Hold::None ? result : stragglers;
}

bool Waiter::Holding(Settings const& settings, std::vector<Member> const& members, bool inCombat, std::uint64_t nowMs, bool* ended)
{
    if (ended)
        *ended = false;
    if (!settings.Enabled || inCombat)
    {
        _active = _straggling = _released = false;
        return false;
    }
    Hold const hold = Evaluate(settings, members);
    if (hold == Hold::None)
    {
        if (ended && _active && !_released)
            *ended = true;
        _active = _straggling = _released = false;
        return false;
    }
    if (_released)
        return false;
    if (!_active)
    {
        _active = true;
        _since = nowMs;
    }
    if (hold == Hold::Straggler)
    {
        if (!_straggling)
        {
            _straggling = true;
            _stragglerSince = nowMs;
        }
        if (nowMs - _stragglerSince >= std::uint64_t(settings.StragglerSec) * 1000)
        {
            _released = true;
            return false;
        }
    }
    else
        _straggling = false;
    if (nowMs - _since >= std::uint64_t(settings.WaitSec) * 1000)
    {
        _released = true;
        return false;
    }
    return true;
}

bool Chatter::PartyMay(PerParty& party, std::uint64_t nowMs) const
{
    if (party.LastSayMs != 0 && nowMs - party.LastSayMs < kPartyGapMs)
        return false;
    party.Recent.erase(std::remove_if(party.Recent.begin(), party.Recent.end(), [&](std::uint64_t t) { return nowMs - t >= kPartyWindowMs; }), party.Recent.end());
    return party.Recent.size() < kPartyWindowLines;
}

bool Chatter::Note(Settings const& settings, ChatFacts const& facts, std::uint64_t seed, std::uint64_t bot, std::uint64_t party, std::uint64_t nowMs, Line& line)
{
    if (!settings.Enabled || !facts.InDungeonParty || facts.GroupSize < 2)
        return false;
    PerBot& me = _bots[bot];
    PerParty& group = _parties[party];
    // Track the dead moment for the thanks.
    if (!facts.Alive)
    {
        me.WasDead = true;
        me.DeadSinceMs = nowMs;
        return false;
    }
    bool const justUp = me.WasDead;
    if (justUp)
        me.WasDead = false;
    if (me.SeenParty != party)
    {
        me.SeenParty = party;
        me.FirstSeenMs = nowMs;
    }
    if (me.LastSayMs != 0 && nowMs - me.LastSayMs < kBotGapMs)
        return false;
    if (!PartyMay(group, nowMs))
        return false;
    auto const say = [&](Line what)
    {
        me.LastSayMs = nowMs;
        group.LastSayMs = nowMs;
        group.Recent.push_back(nowMs);
        line = what;
        return true;
    };
    if (justUp && facts.GroupMateNear && nowMs - me.DeadSinceMs <= kThanksWindowMs && (me.ReadyOrThanksMs == 0 || nowMs - me.ReadyOrThanksMs >= kThanksGapMs))
    {
        me.ReadyOrThanksMs = nowMs;
        return say(Line::Thanks);
    }
    if (facts.Leader)
    {
        auto due = _readyDue.find(party);
        if (due != _readyDue.end() && due->second)
        {
            due->second = false;
            if (group.ReadyMs == 0 || nowMs - group.ReadyMs >= kReadyGapMs)
            {
                group.ReadyMs = nowMs;
                return say(Line::Ready);
            }
        }
    }
    if (me.GreetedParty != party && group.Greets < kMaxGreeters)
    {
        std::uint64_t const delay = 2000 + bot_random::Below(bot_random::Draw(seed, bot, kSaltGreet, party), 7001);
        if (nowMs - me.FirstSeenMs >= delay)
        {
            me.GreetedParty = party;
            ++group.Greets;
            return say(Line::Greet);
        }
    }
    return false;
}

std::vector<std::string> LinesOf(std::vector<std::string> const& pool, Line kind)
{
    char const* const key = kind == Line::Greet ? "greet" : kind == Line::Ready ? "ready" : "thanks";
    std::string const prefix = std::string(key) + "|";
    std::vector<std::string> out;
    for (std::string const& row : pool)
        if (row.compare(0, prefix.size(), prefix) == 0 && row.size() > prefix.size())
            out.push_back(row.substr(prefix.size()));
    return out;
}

char const* RollName(Roll roll)
{
    return roll == Roll::Need ? "need" : roll == Roll::Greed ? "greed" : "pass";
}

Roll DecideRoll(Settings const& settings, RollFacts const& facts)
{
    if (!settings.Loot || facts.Quality == 0)
        return Roll::Pass;
    if (!facts.Usable)
        return facts.BindOnPickup ? Roll::Pass : Roll::Greed;
    if (facts.Upgrade >= kNeedMarginScore && facts.WantsStats)
    {
        for (Rival const& rival : facts.Rivals)
        {
            if (!rival.Usable || rival.Bot == facts.Bot)
                continue;
            if (rival.Upgrade > facts.Upgrade || (rival.Upgrade == facts.Upgrade && rival.Bot < facts.Bot))
                return Roll::Greed;   // somebody needs it more
        }
        return Roll::Need;
    }
    return Roll::Greed;
}

}  // namespace group_etiquette
