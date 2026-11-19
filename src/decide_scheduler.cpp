// SPDX-License-Identifier: GPL-2.0-or-later
#include "decide_scheduler.h"

#include <algorithm>
#include <sstream>

namespace decide_scheduler
{

char const* TierName(Tier tier)
{
    switch (tier)
    {
        case Tier::kEvery: return "every";
        case Tier::kWalk: return "walk";
        case Tier::kIdle: return "idle";
    }
    return "?";
}

Tier TierOf(BotFacts const& facts)
{
    if (facts.InCombat || facts.Party || facts.Dungeon || facts.GhostReviving || facts.Busy || facts.HostileNear)
        return Tier::kEvery;
    if (facts.LifeState != 0)
        return Tier::kIdle;   // dead waiting to release, or a ghost on its corpse run
    if (facts.Resting)
        return Tier::kIdle;
    if (facts.Moving || facts.Questing)
        return Tier::kWalk;
    return Tier::kIdle;       // idle standing, nothing in the log
}

std::uint32_t StaggerOf(std::uint64_t guid, std::uint32_t every)
{
    if (every <= 1)
        return 0;
    std::uint64_t z = guid + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z = z ^ (z >> 31);
    return std::uint32_t(z % every);
}

std::string FormatStats(Settings const& settings, Stats const& stats)
{
    if (!settings.Enabled)
        return {};
    std::ostringstream out;
    out << " sched=on walk=" << settings.TierWalk << " idle=" << settings.TierIdle << " budget_us=" << settings.BudgetUs << " tiers=";
    for (std::size_t t = 0; t < kTierCount; ++t)
        out << (t ? "," : "") << TierName(Tier(t)) << ":" << stats.Seen[t] << "/" << stats.Ran[t];
    out << " skipped=" << stats.Skipped << " deferred=" << stats.Deferred << " forced=" << stats.Forced << " starved=" << stats.Starved
        << " overruns=" << stats.Overruns << " max_turn_us=" << stats.MaxTurnUs << " max_tick_us=" << stats.MaxTickUs;
    return out.str();
}

std::uint32_t Scheduler::EveryOf(Tier tier) const
{
    switch (tier)
    {
        case Tier::kEvery: return 1;
        case Tier::kWalk: return std::max<std::uint32_t>(1, _settings.TierWalk);
        case Tier::kIdle: return std::max<std::uint32_t>(1, _settings.TierIdle);
    }
    return 1;
}

int Scheduler::Need(BotFacts const& facts, Memory const* memory, Tier tier, bool& reaction) const
{
    reaction = false;
    if (memory == nullptr)
    {
        reaction = true;   // first sight
        return 2;
    }
    if (tier == Tier::kEvery)
        return 2;
    if (facts.PendingReaction || facts.ActionSequence != memory->Sequence || facts.LifeState != memory->LifeState || (memory->Moving && !facts.Moving))
    {
        reaction = true;
        return 2;
    }
    if (memory->Waited >= std::max<std::uint32_t>(1, _settings.MaxSkipTicks))
        return 3;
    if (memory->Carry)
        return 1;
    std::uint32_t const every = EveryOf(tier);
    if (every <= 1 || (_tick + StaggerOf(facts.Guid, every)) % every == 0)
        return 1;
    return 0;
}

void Scheduler::Tick(std::span<BotFacts const> facts, std::function<void(std::size_t)> const& run, std::function<void(std::size_t)> const& skip,
                     std::function<std::uint64_t()> const& clockUs)
{
    if (!_settings.Enabled)
    {
        for (std::size_t i = 0; i < facts.size(); ++i)
            run(i);
        return;
    }
    ++_tick;
    ++_stats.Ticks;
    std::uint64_t const startUs = clockUs();

    std::size_t const n = facts.size();
    std::vector<Tier> tiers(n);
    std::vector<std::size_t> must;
    std::vector<std::size_t> optional;
    std::vector<std::size_t> idle;
    must.reserve(n);
    optional.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        tiers[i] = TierOf(facts[i]);
        ++_stats.Seen[std::size_t(tiers[i])];
        auto const found = _memory.find(facts[i].Guid);
        bool reaction = false;
        int const need = Need(facts[i], found == _memory.end() ? nullptr : &found->second, tiers[i], reaction);
        if (need >= 2)
        {
            must.push_back(i);
            if (need == 3)
                ++_stats.Starved;
            else if (reaction && found != _memory.end())
                ++_stats.Forced;
        }
        else if (need == 1)
            optional.push_back(i);
        else
            idle.push_back(i);
    }

    auto const turn = [&](std::size_t i) {
        std::uint64_t const before = clockUs();
        run(i);
        std::uint64_t const after = clockUs();
        _stats.MaxTurnUs = std::max(_stats.MaxTurnUs, after >= before ? after - before : 0);
        ++_stats.Ran[std::size_t(tiers[i])];
        Memory& memory = _memory[facts[i].Guid];
        memory.Waited = 0;
        memory.Sequence = facts[i].ActionSequence;
        memory.LifeState = facts[i].LifeState;
        memory.Moving = facts[i].Moving;
        memory.Carry = false;
        memory.SeenTick = _tick;
    };
    auto const wait = [&](std::size_t i, bool carry) {
        skip(i);
        Memory& memory = _memory[facts[i].Guid];   // known: a bot seen for the first time is a must-run
        ++memory.Waited;
        memory.Moving = facts[i].Moving;   // seen every tick: a walk that starts and ends between two turns still ends with a turn
        memory.Carry = carry;
        memory.SeenTick = _tick;
    };

    for (std::size_t i : must)
        turn(i);

    // The due bots: the longest waiting first (carried over ones have waited longer), then round-robin from the cursor.
    std::size_t const cursor = n ? _cursor % n : 0;
    std::stable_sort(optional.begin(), optional.end(), [&](std::size_t a, std::size_t b) {
        Memory const& ma = _memory.at(facts[a].Guid);
        Memory const& mb = _memory.at(facts[b].Guid);
        if (ma.Carry != mb.Carry)
            return ma.Carry;
        if (ma.Waited != mb.Waited)
            return ma.Waited > mb.Waited;
        return (a + n - cursor) % n < (b + n - cursor) % n;
    });
    bool deferredAny = false;
    for (std::size_t i : optional)
    {
        std::uint64_t const now = clockUs();
        if (_settings.BudgetUs != 0 && now - startUs >= _settings.BudgetUs)
        {
            if (!deferredAny)
                _cursor = i;   // the next tick's round-robin starts at the first bot that did not fit
            deferredAny = true;
            ++_stats.Deferred;
            wait(i, true);
            continue;
        }
        turn(i);
    }
    for (std::size_t i : idle)
    {
        ++_stats.Skipped;
        wait(i, false);
    }

    std::uint64_t const tickUs = clockUs() - startUs;
    _stats.TotalTickUs += tickUs;
    _stats.MaxTickUs = std::max(_stats.MaxTickUs, tickUs);
    if (_settings.BudgetUs != 0 && tickUs > _settings.BudgetUs)
        ++_stats.Overruns;

    // Bots that left the view are forgotten (checked now and then, the map is small).
    if (_tick % 32 == 0)
        for (auto it = _memory.begin(); it != _memory.end();)
            it = it->second.SeenTick + 32 < _tick ? _memory.erase(it) : std::next(it);
}

Stats Scheduler::TakeStats()
{
    Stats const out = _stats;
    _stats = Stats{};
    return out;
}

int Scheduler::WaitedOf(std::uint64_t guid) const
{
    auto const found = _memory.find(guid);
    return found == _memory.end() ? -1 : int(found->second.Waited);
}

} // namespace decide_scheduler
