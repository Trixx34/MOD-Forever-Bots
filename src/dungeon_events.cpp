// SPDX-License-Identifier: GPL-2.0-or-later
#include "dungeon_events.h"

#include "text_number.h"

#include <cmath>
#include <sstream>

namespace dungeon_events {

namespace {

// Was a local ToUint/ToFloat pair; now the shared single-token parser (text_number.h) used by every
// other plain-text data file. The old ToUint here did not check that the whole token was consumed, so
// e.g. "5dummy" silently parsed as 5 - that bug is gone now that every file shares one implementation.
bool ToUint(std::string const& text, std::uint32_t& out)
{
    return text_number::ParseWhole(text, out);
}

bool ToFloat(std::string const& text, float& out)
{
    return text_number::ParseDecimal(text, out);
}

float Dist2d(float ax, float ay, float bx, float by)
{
    return std::hypot(ax - bx, ay - by);
}

} // namespace

std::vector<Event> Parse(std::string_view text, std::vector<std::string>& warnings)
{
    std::vector<Event> events;
    std::istringstream in{ std::string(text) };
    std::string line;
    std::size_t number = 0;
    Event current;
    bool inside = false;
    bool hasTrigger = false;
    bool bad = false;
    auto const warn = [&](std::string const& what)
    {
        warnings.push_back("dungeon events line " + std::to_string(number) + ": " + what);
    };
    while (std::getline(in, line))
    {
        ++number;
        if (auto const hash = line.find('#'); hash != std::string::npos)
            line.resize(hash);
        std::istringstream words(line);
        std::vector<std::string> w;
        for (std::string word; words >> word;)
            w.push_back(word);
        if (w.empty())
            continue;

        if (w[0] == "event")
        {
            if (inside)
                warn("'event' inside an event; the unfinished one is dropped");
            current = Event();
            inside = true;
            hasTrigger = false;
            bad = false;
            if (w.size() < 3 || !ToUint(w[1], current.Map) || current.Map == 0)
            {
                warn("event needs <map> <id> <name>");
                bad = true;
                continue;
            }
            current.Id = w[2];
            for (std::size_t i = 3; i < w.size(); ++i)
                current.Name += (i > 3 ? " " : "") + w[i];
            continue;
        }
        if (!inside)
        {
            warn("'" + w[0] + "' outside an event; ignored");
            continue;
        }
        if (w[0] == "end")
        {
            if (!bad && !hasTrigger)
                warn("event " + current.Id + " has no trigger; dropped");
            else if (!bad && current.Steps.empty())
                warn("event " + current.Id + " has no steps; dropped");
            else if (!bad)
                events.push_back(current);
            inside = false;
            continue;
        }
        if (w[0] == "at_start")
        {
            current.When = Event::Trigger::AtStart;
            hasTrigger = true;
        }
        else if (w[0] == "after_boss")
        {
            if (w.size() == 2 && ToUint(w[1], current.Boss) && current.Boss != 0)
            {
                current.When = Event::Trigger::AfterBoss;
                hasTrigger = true;
            }
            else
            {
                warn("after_boss needs a creature entry");
                bad = true;
            }
        }
        else if (w[0] == "after_bosses")
        {
            current.Bosses.clear();
            bool ok = w.size() >= 2;
            for (std::size_t i = 1; ok && i < w.size(); ++i)
            {
                std::uint32_t entry = 0;
                ok = ToUint(w[i], entry) && entry != 0;
                if (ok)
                    current.Bosses.push_back(entry);
            }
            if (ok)
            {
                current.When = Event::Trigger::AfterAllBosses;
                hasTrigger = true;
            }
            else
            {
                warn("after_bosses needs one or more creature entries");
                bad = true;
            }
        }
        else if (w[0] == "near")
        {
            if (w.size() == 5 && ToFloat(w[1], current.X) && ToFloat(w[2], current.Y) && ToFloat(w[3], current.Z) && ToFloat(w[4], current.Radius) && current.Radius > 0)
            {
                current.When = Event::Trigger::Near;
                hasTrigger = true;
            }
            else
            {
                warn("near needs <x> <y> <z> <radius>");
                bad = true;
            }
        }
        else if (w[0] == "use")
        {
            Step step;
            step.What = Step::Kind::Use;
            bool ok = w.size() >= 5 && ToUint(w[1], step.Object) && step.Object != 0 && ToFloat(w[2], step.X) && ToFloat(w[3], step.Y) && ToFloat(w[4], step.Z);
            if (ok && w.size() == 7)
                ok = w[5] == "item" && ToUint(w[6], step.Item) && step.Item != 0;
            else if (ok)
                ok = w.size() == 5;
            if (ok)
                current.Steps.push_back(step);
            else
            {
                warn("use needs <object> <x> <y> <z> [item <entry>]");
                bad = true;
            }
        }
        else if (w[0] == "talk")
        {
            Step step;
            step.What = Step::Kind::Talk;
            if (w.size() == 6 && ToUint(w[1], step.Creature) && step.Creature != 0 && ToUint(w[2], step.Option) && ToFloat(w[3], step.X) && ToFloat(w[4], step.Y) &&
                ToFloat(w[5], step.Z))
                current.Steps.push_back(step);
            else
            {
                warn("talk needs <creature> <option> <x> <y> <z>");
                bad = true;
            }
        }
        else if (w[0] == "follow")
        {
            Step step;
            step.What = Step::Kind::Follow;
            if (w.size() == 3 && ToUint(w[1], step.Creature) && step.Creature != 0 && ToUint(w[2], step.WaitSec) && step.WaitSec >= 1 && step.WaitSec <= 600)
                current.Steps.push_back(step);
            else
            {
                warn("follow needs <creature> and 1 to 600 seconds");
                bad = true;
            }
        }
        else if (w[0] == "teleport")
        {
            Step step;
            step.What = Step::Kind::Teleport;
            if (w.size() == 4 && ToFloat(w[1], step.X) && ToFloat(w[2], step.Y) && ToFloat(w[3], step.Z))
                current.Steps.push_back(step);
            else
            {
                warn("teleport needs <x> <y> <z>");
                bad = true;
            }
        }
        else if (w[0] == "wait")
        {
            Step step;
            step.What = Step::Kind::Wait;
            if (w.size() == 2 && ToUint(w[1], step.WaitSec) && step.WaitSec <= 600)
                current.Steps.push_back(step);
            else
            {
                warn("wait needs 0 to 600 seconds");
                bad = true;
            }
        }
        else if (w[0] == "timeout")
        {
            if (!(w.size() == 2 && ToUint(w[1], current.TimeoutSec) && current.TimeoutSec >= 5 && current.TimeoutSec <= 3600))
            {
                warn("timeout needs 5 to 3600 seconds");
                bad = true;
            }
        }
        else
            warn("unknown line '" + w[0] + "'; ignored");
    }
    if (inside)
        warn("the last event has no 'end'; dropped");
    return events;
}

std::vector<Event> ForMap(std::vector<Event> const& all, std::uint32_t map)
{
    std::vector<Event> out;
    for (Event const& event : all)
        if (event.Map == map)
            out.push_back(event);
    return out;
}

void Runner::Finish(std::string const& how, std::uint64_t nowMs, bool success)
{
    if (_active < 0)
        return;
    std::size_t const idx = std::size_t(_active);
    if (success)
    {
        _log.push_back(how + ": event=" + _events[idx].Id + " name=" + _events[idx].Name);
        _done[idx] = true;
    }
    else
    {
        ++_attempts[idx];
        if (_attempts[idx] >= kMaxEventAttemptCycles)
        {
            // Given up for good: do NOT let the leader treat this as done and walk past a door that is
            // still closed. This is a terminal, loud state meant to be grepped/alerted on; it needs a
            // human to look at the dungeon file (wrong object entry, wrong radius, a GO state issue, ...).
            _log.push_back("event_blocked: event=" + _events[idx].Id + " name=" + _events[idx].Name
                + " attempts=" + std::to_string(_attempts[idx]));
            _done[idx] = true;
        }
        else
        {
            _log.push_back(how + "_retry: event=" + _events[idx].Id + " name=" + _events[idx].Name
                + " attempt=" + std::to_string(_attempts[idx]) + "/" + std::to_string(kMaxEventAttemptCycles));
            _retryAfter[idx] = nowMs + kEventRetryBackoffMs;
            // _done[idx] stays false: Next() will consider this event again once its trigger holds and
            // the backoff has passed, instead of silently letting the route continue past a shut door.
        }
    }
    _active = -1;
    _step = 0;
    _sent = false;
    _failures = 0;
    _close = false;
}

void Runner::OnUseResult(UseResult result, std::uint64_t nowMs)
{
    if (_active < 0 || !_sent)
        return;
    _sent = false;
    switch (result)
    {
        case UseResult::Applied:
            ++_step;
            _stepSinceMs = nowMs;
            _failures = 0;
            _close = false;
            break;
        case UseResult::OutOfRange:
            _close = true;      // come closer and try again; this is not a failure of the object
            break;
        case UseResult::Failed:
            if (++_failures >= kMaxFailures)
                Finish("event_failed", nowMs, /*success=*/false);
            break;
    }
}

std::vector<std::string> Runner::TakeLog()
{
    std::vector<std::string> out;
    out.swap(_log);
    return out;
}

Command Runner::Next(Facts const& facts, std::uint64_t nowMs)
{
    if (_events.empty())
        return {};

    // Start the first event, in file order, whose trigger holds and that has not run yet.
    if (_active < 0)
    {
        if (facts.InCombat || !facts.Alive)
            return {};
        for (std::size_t i = 0; i < _events.size(); ++i)
        {
            if (_done[i])
                continue;
            if (nowMs < _retryAfter[i])
                continue;   // failed or timed out recently; back off before trying this door/lever again
            Event const& event = _events[i];
            bool fires = false;
            switch (event.When)
            {
                case Event::Trigger::AtStart: fires = true; break;
                case Event::Trigger::AfterBoss: fires = facts.KilledBosses && facts.KilledBosses->count(event.Boss) != 0; break;
                case Event::Trigger::Near: fires = Dist2d(facts.X, facts.Y, event.X, event.Y) <= event.Radius; break;
                case Event::Trigger::AfterAllBosses:
                    fires = facts.KilledBosses != nullptr && !event.Bosses.empty();
                    for (std::uint32_t const boss : event.Bosses)
                        if (!facts.KilledBosses || facts.KilledBosses->count(boss) == 0)
                            fires = false;
                    break;
            }
            if (!fires)
                continue;
            _active = int(i);
            _step = 0;
            _startedMs = nowMs;
            _stepSinceMs = nowMs;
            _sent = false;
            _failures = 0;
            _close = false;
            _log.push_back("event_start: event=" + event.Id + " name=" + event.Name);
            break;
        }
        if (_active < 0)
            return {};
    }

    Event const& event = _events[_active];
    if (nowMs - _startedMs >= std::uint64_t(event.TimeoutSec) * 1000)
    {
        Finish("event_timeout", nowMs, /*success=*/false);
        return {};
    }
    if (_step >= event.Steps.size())
    {
        Finish("event_done", nowMs, /*success=*/true);
        return {};
    }
    if (!facts.Alive)
        return { Command::What::Hold };
    if (facts.InCombat)
        return { Command::What::Hold };   // the party fights first; the step is picked up afterwards

    Step const& step = event.Steps[_step];
    if (step.What == Step::Kind::Wait)
    {
        if (nowMs - _stepSinceMs >= std::uint64_t(step.WaitSec) * 1000)
        {
            ++_step;
            _stepSinceMs = nowMs;
            return Next(facts, nowMs);    // on to the next step in the same tick
        }
        return { Command::What::Hold };
    }

    if (step.What == Step::Kind::Follow)
    {
        if (nowMs - _stepSinceMs >= std::uint64_t(step.WaitSec) * 1000)
        {
            ++_step;
            _stepSinceMs = nowMs;
            return Next(facts, nowMs);
        }
        Command follow;
        follow.Do = Command::What::Follow;
        follow.Creature = step.Creature;
        return follow;
    }
    if (step.What == Step::Kind::Teleport)
    {
        Command teleport;
        teleport.Do = Command::What::Teleport;
        teleport.X = step.X;
        teleport.Y = step.Y;
        teleport.Z = step.Z;
        ++_step;                  // a teleport has no answer to wait for
        _stepSinceMs = nowMs;
        return teleport;
    }

    // Use or Talk: be at the place first, then send it; the answer is waited for.
    float const reach = _close ? kCloseYards : kApproachYards;
    if (Dist2d(facts.X, facts.Y, step.X, step.Y) > reach)
    {
        Command move;
        move.Do = Command::What::MoveTo;
        move.X = step.X;
        move.Y = step.Y;
        move.Z = step.Z;
        return move;
    }
    if (_sent)
    {
        if (nowMs - _sentMs < kAnswerWaitMs)
            return { Command::What::Hold };
        _sent = false;      // no answer came: try again (counted as a failure)
        if (++_failures >= kMaxFailures)
        {
            Finish("event_failed", nowMs, /*success=*/false);
            return {};
        }
    }
    Command use;
    use.Do = step.What == Step::Kind::Talk ? Command::What::Talk : Command::What::Use;
    use.X = step.X;
    use.Y = step.Y;
    use.Z = step.Z;
    use.Object = step.Object;
    use.Item = step.Item;
    use.Creature = step.Creature;
    use.Option = step.Option;
    _sent = true;
    _sentMs = nowMs;
    return use;
}

} // namespace dungeon_events
