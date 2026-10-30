// Unit tests for the dungeon event table and its runner (src/dungeon_events.h). No core or SDK needed.
#include "dungeon_events.h"

#include <cstdio>

using namespace dungeon_events;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

char const* kFile = R"(# a file
event 36 cannon Iron Clad Door (Defias Cannon)
after_boss 644
use 16398 -107.5 -659.6 7.2 item 5397
wait 3
timeout 90
end

event 36 lever Factory Door lever  # trailing comment
near -188 -460 54 40
use 101831 -188.1 -460.3 54.6
end

event 389 start Welcome
at_start
wait 1
end

event 36 broken
use 1 2 3
end

event 36 notrigger
use 5 1 2 3
end
)";

void TheFileIsParsedAndBadBlocksAreDropped()
{
    std::vector<std::string> warnings;
    std::vector<Event> const all = Parse(kFile, warnings);
    CHECK(all.size() == 3);
    CHECK(warnings.size() == 2);   // 'broken' (use needs 4 numbers) and 'notrigger'
    CHECK(all[0].Map == 36 && all[0].Id == "cannon" && all[0].Name == "Iron Clad Door (Defias Cannon)" && all[0].When == Event::Trigger::AfterBoss && all[0].Boss == 644);
    CHECK(all[0].Steps.size() == 2 && all[0].Steps[0].Object == 16398 && all[0].Steps[0].Item == 5397 && all[0].Steps[0].X == -107.5f);
    CHECK(all[0].Steps[1].What == Step::Kind::Wait && all[0].Steps[1].WaitSec == 3 && all[0].TimeoutSec == 90);
    CHECK(all[1].When == Event::Trigger::Near && all[1].Radius == 40.0f && all[1].Steps[0].Item == 0 && all[1].TimeoutSec == 120);
    CHECK(all[2].When == Event::Trigger::AtStart && all[2].Map == 389);
    CHECK(ForMap(all, 36).size() == 2 && ForMap(all, 389).size() == 1 && ForMap(all, 1).empty());
}

void TheTriggersFire()
{
    std::vector<std::string> warnings;
    Runner runner(ForMap(Parse(kFile, warnings), 36));
    std::set<std::uint32_t> killed;
    Facts far;
    far.X = 0;
    far.Y = 0;
    far.KilledBosses = &killed;
    CHECK(runner.Next(far, 0).Do == Command::What::Nothing && !runner.Active());     // nothing yet: not near the lever, no boss dead

    Facts near = far;
    near.X = -180;
    near.Y = -455;                                                                      // within 40 yards of the lever
    Command const walk = runner.Next(near, 1000);
    CHECK(runner.Active() && walk.Do == Command::What::MoveTo && walk.X == -188.1f);

    Runner second(ForMap(Parse(kFile, warnings), 36));
    killed.insert(644);                                                                 // the boss dead: the cannon event is first in the file
    Command const toCannon = second.Next(far, 0);
    CHECK(toCannon.Do == Command::What::MoveTo && toCannon.X == -107.5f);
}

void AnEventWalksUsesAndWaits()
{
    std::vector<std::string> warnings;
    Runner runner(ForMap(Parse(kFile, warnings), 36));
    std::set<std::uint32_t> killed{ 644 };
    Facts f;
    f.X = -50;
    f.Y = -600;
    f.KilledBosses = &killed;
    std::uint64_t now = 0;

    CHECK(runner.Next(f, now).Do == Command::What::MoveTo);
    f.X = -105;
    f.Y = -658;                                                                          // 3 yards from the cannon
    now += 5000;
    Command const use = runner.Next(f, now);
    CHECK(use.Do == Command::What::Use && use.Object == 16398 && use.Item == 5397);
    now += 500;
    CHECK(runner.Next(f, now).Do == Command::What::Hold);                               // the answer is awaited
    runner.OnUseResult(UseResult::Applied, now);
    now += 100;
    CHECK(runner.Next(f, now).Do == Command::What::Hold);                               // the wait step
    now += 3000;
    CHECK(runner.Next(f, now).Do == Command::What::Nothing && !runner.Active());       // done; the next event (the lever) is not triggered here
    std::vector<std::string> const log = runner.TakeLog();
    CHECK(log.size() == 2 && log[0].rfind("event_start: event=cannon", 0) == 0 && log[1].rfind("event_done: event=cannon", 0) == 0);

    now += 1000;
    CHECK(runner.Next(f, now).Do == Command::What::Nothing);                            // it runs once
}

void OutOfRangeBringsTheLeaderCloserNotToFailure()
{
    std::vector<std::string> warnings;
    Runner runner(ForMap(Parse(kFile, warnings), 36));
    std::set<std::uint32_t> killed{ 644 };
    Facts f;
    f.X = -105;
    f.Y = -658;
    f.KilledBosses = &killed;
    CHECK(runner.Next(f, 0).Do == Command::What::Use);
    runner.OnUseResult(UseResult::OutOfRange, 100);
    Command const closer = runner.Next(f, 200);                                          // 3 yards is not close enough any more
    CHECK(closer.Do == Command::What::MoveTo);
    f.X = -107.0f;
    f.Y = -659.5f;
    CHECK(runner.Next(f, 300).Do == Command::What::Use);
    for (int i = 0; i < 5; ++i)                                                          // out of range again and again is never a failure
    {
        runner.OnUseResult(UseResult::OutOfRange, 400);
        CHECK(runner.Active());
        runner.Next(f, 500);
    }
}

void AFailedEventRetriesWithBackoffThenGivesUpForGood()
{
    std::vector<std::string> warnings;
    Runner runner(ForMap(Parse(kFile, warnings), 36));
    std::set<std::uint32_t> killed{ 644 };
    Facts f;
    f.X = -107;
    f.Y = -659;
    f.KilledBosses = &killed;

    std::uint64_t now = 0;
    for (unsigned cycle = 1; cycle <= kMaxEventAttemptCycles; ++cycle)
    {
        Command const started = runner.Next(f, now);
        CHECK(started.Do == Command::What::Use);   // each cycle restarts the event from its first step
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            runner.OnUseResult(UseResult::Failed, now + 10);
            now += 10;
            if (attempt < 2)
                CHECK(runner.Next(f, now).Do == Command::What::Use);
        }
        CHECK(!runner.Active());                   // the attempt cycle ends: 3 refusals in a row give it up for now
        if (cycle < kMaxEventAttemptCycles)
        {
            CHECK(runner.Next(f, now).Do == Command::What::Nothing);   // too soon: still backing off
            now += kEventRetryBackoffMs;
        }
    }
    // The last cycle gave up for good: waiting out another backoff does not restart it again.
    now += kEventRetryBackoffMs;
    CHECK(runner.Next(f, now).Do == Command::What::Nothing && !runner.Active());

    bool sawRetry = false, sawBlocked = false;
    for (std::string const& line : runner.TakeLog())
    {
        sawRetry = sawRetry || line.rfind("event_failed_retry: event=cannon", 0) == 0;
        sawBlocked = sawBlocked || (line.rfind("event_blocked: event=cannon", 0) == 0 && line.find("attempts=5") != std::string::npos);
    }
    CHECK(sawRetry);
    CHECK(sawBlocked);
}

void ATimedOutEventRetriesThenCanSucceed()
{
    std::vector<std::string> warnings;
    Runner runner(ForMap(Parse(kFile, warnings), 36));
    std::set<std::uint32_t> killed{ 644 };
    Facts f;
    f.X = -50;
    f.Y = -600;
    f.KilledBosses = &killed;

    CHECK(runner.Next(f, 0).Do == Command::What::MoveTo);
    CHECK(runner.Next(f, 89000).Do == Command::What::MoveTo);
    CHECK(runner.Next(f, 90000).Do == Command::What::Nothing && !runner.Active());      // timeout 90 s: the cycle ends but is retryable

    CHECK(runner.Next(f, 90001).Do == Command::What::Nothing && !runner.Active());      // too soon: still backing off

    std::uint64_t const retryNow = 90000 + kEventRetryBackoffMs;
    CHECK(runner.Next(f, retryNow).Do == Command::What::MoveTo && runner.Active());     // backoff elapsed: another chance at the door

    bool sawRetry = false;
    for (std::string const& line : runner.TakeLog())
        sawRetry = sawRetry || line.rfind("event_timeout_retry: event=cannon", 0) == 0;
    CHECK(sawRetry);
}

void TheLeaderFightsFirst()
{
    std::vector<std::string> warnings;
    Runner runner(ForMap(Parse(kFile, warnings), 36));
    std::set<std::uint32_t> killed{ 644 };
    Facts f;
    f.X = -105;
    f.Y = -658;
    f.KilledBosses = &killed;
    f.InCombat = true;
    CHECK(runner.Next(f, 0).Do == Command::What::Nothing && !runner.Active());          // no event starts in a fight
    f.InCombat = false;
    CHECK(runner.Next(f, 100).Do == Command::What::Use);
    f.InCombat = true;
    CHECK(runner.Next(f, 200).Do == Command::What::Hold);                               // one under way waits for the fight to end
    f.InCombat = false;
    f.Alive = false;
    CHECK(runner.Next(f, 300).Do == Command::What::Hold);
}

// The Wailing Caverns escort: after the four Lords of the Fang are dead, talk to the Disciple of Naralex at the start, follow him, then stand through the waves.
void TheNaralexEscortRuns()
{
    char const* text = R"(event 43 naralex Disciple of Naralex (escort)
after_bosses 3671 3669 3670 3673
talk 3678 0 -135 125.4 -78.1
follow 3678 60
wait 20
timeout 600
end

event 43 bridge Bridge to the Lord's cave
at_start
teleport -285 2 -64
end

event 43 badbosses Nothing listed
after_bosses
wait 1
end

event 43 badtalk Missing option
at_start
talk 3678 -135 125.4 -78.1
end
)";
    std::vector<std::string> warnings;
    std::vector<Event> const all = Parse(text, warnings);
    CHECK(all.size() == 2 && warnings.size() == 2);
    CHECK(all[0].When == Event::Trigger::AfterAllBosses && all[0].Bosses.size() == 4 && all[0].Bosses[3] == 3673 && all[0].TimeoutSec == 600);
    CHECK(all[0].Steps.size() == 3 && all[0].Steps[0].What == Step::Kind::Talk && all[0].Steps[0].Creature == 3678 && all[0].Steps[0].Option == 0);
    CHECK(all[0].Steps[1].What == Step::Kind::Follow && all[0].Steps[1].Creature == 3678 && all[0].Steps[1].WaitSec == 60);
    CHECK(all[1].Steps[0].What == Step::Kind::Teleport && all[1].Steps[0].X == -285.0f && all[1].Steps[0].Z == -64.0f);

    // the escort alone (the bridge is for another leader)
    Runner runner({ all[0] });
    std::set<std::uint32_t> killed{ 3671, 3669, 3670 };
    Facts f;
    f.X = -40;
    f.Y = 300;
    f.KilledBosses = &killed;
    std::uint64_t now = 0;
    CHECK(runner.Next(f, now).Do == Command::What::Nothing && !runner.Active());       // three of the four are not enough
    killed.insert(3673);
    Command const walk = runner.Next(f, now += 1000);
    CHECK(runner.Active() && walk.Do == Command::What::MoveTo && walk.X == -135.0f && walk.Y == 125.4f);
    f.X = -133;
    f.Y = 125;                                                                          // within 4 yards of the Disciple
    Command const talk = runner.Next(f, now += 20000);
    CHECK(talk.Do == Command::What::Talk && talk.Creature == 3678 && talk.Option == 0);
    CHECK(runner.Next(f, now += 500).Do == Command::What::Hold);                        // the answer is awaited
    runner.OnUseResult(UseResult::Applied, now);
    Command const follow = runner.Next(f, now += 100);
    CHECK(follow.Do == Command::What::Follow && follow.Creature == 3678);
    f.InCombat = true;
    CHECK(runner.Next(f, now += 5000).Do == Command::What::Hold);                      // a fight on the way is fought first
    f.InCombat = false;
    CHECK(runner.Next(f, now += 5000).Do == Command::What::Follow);
    CHECK(runner.Next(f, now += 60000).Do == Command::What::Hold);                      // the follow time is over: the wait step
    CHECK(runner.Next(f, now += 20000).Do == Command::What::Nothing && !runner.Active());

    Runner bridge({ all[1] });
    Facts g;
    Command const jump = bridge.Next(g, 0);
    CHECK(jump.Do == Command::What::Teleport && jump.X == -285.0f && jump.Y == 2.0f && jump.Z == -64.0f);
    CHECK(bridge.Next(g, 100).Do == Command::What::Nothing && !bridge.Active());        // a teleport has no answer to wait for
}

} // namespace

int main()
{
    TheFileIsParsedAndBadBlocksAreDropped();
    TheTriggersFire();
    AnEventWalksUsesAndWaits();
    OutOfRangeBringsTheLeaderCloserNotToFailure();
    AFailedEventRetriesWithBackoffThenGivesUpForGood();
    ATimedOutEventRetriesThenCanSucceed();
    TheLeaderFightsFirst();
    TheNaralexEscortRuns();
    if (failures == 0)
        std::printf("dungeon_events_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
