// SPDX-License-Identifier: GPL-2.0-or-later
// World events: the site table, the event id list, the caps and the rules of the scheduler (src/world_events.h).
#include "world_events.h"

#include <cstdio>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

using namespace world_events;

std::vector<Site> Sites()
{
    std::vector<std::string> errors;
    auto sites = ParseSites(
        "# event|side|map|x|y|z|minLevel|maxLevel|name|quests\n"
        "7|1|0|-5000|-900|500|1|60|Elder Near|8619,8620\n"
        "7|1|0|-5100|-900|500|1|60|Elder Second|8621\n"
        "7|2|1|1500|-4300|20|1|60|Elder Horde|8635\n"
        "12|0|0|-9000|500|60|10|60|Spooky Inn|\n",
        errors);
    CHECK(errors.empty());
    return sites;
}

Settings On()
{
    Settings s;
    s.Enabled = true;
    s.SharePct = 20;
    s.MaxBots = 10;
    s.MaxPerSite = 2;
    s.MaxMinutes = 10;
    s.GapMinutes = 60;
    s.MaxStartsPerTick = 5;
    return s;
}

std::vector<Bot> Bots(std::size_t count, std::uint32_t side = 1, std::uint32_t map = 0)
{
    std::vector<Bot> bots;
    for (std::size_t i = 0; i < count; ++i)
    {
        Bot b;
        b.Guid = 100 + i;
        b.Available = true;
        b.Side = side;
        b.Level = 30;
        b.Map = map;
        b.X = -5000;
        b.Y = -900;
        bots.push_back(b);
    }
    return bots;
}

void TheTableParsesAndABadLineIsDescribed()
{
    std::vector<std::string> errors;
    auto const sites = ParseSites(
        "7|1|0|-5000|-900|500|1|60|Elder Near|8619,8620\n"
        "7|3|0|1|2|3|1|60|Bad side|\n"
        "7|1|0|1|2|3|40|10|Bad levels|\n"
        "7|1|0|1|2|3|1|60|Bad quest|8619,x\n"
        "0|1|0|1|2|3|1|60|No event|\n"
        "7|0|0|1|2|3|1|60|Visit only|\n",
        errors);
    CHECK(sites.size() == 2);
    CHECK(errors.size() == 4);
    CHECK(sites.size() == 2 && sites[0].Quests.size() == 2 && sites[0].Name == "Elder Near" && sites[1].Quests.empty());
}

void EventIdListsParse()
{
    std::vector<std::string> errors;
    auto const ids = ParseEventIds(" 7, 12 ,7,,x,0", errors);
    CHECK(ids.size() == 2 && ids[0] == 7 && ids[1] == 12);
    CHECK(errors.size() == 2);
    std::vector<std::string> none;
    CHECK(ParseEventIds("", none).empty() && none.empty());
}

void TheCoreReportReplacesTheConfiguredList()
{
    ActiveEvents active;
    active.SetConfigured({ 7 });
    CHECK(!active.HasReport() && active.Current() == std::vector<std::uint32_t>{ 7 });
    active.SetReported({ 12, 2 });
    CHECK(active.HasReport() && active.Current() == (std::vector<std::uint32_t>{ 12, 2 }));
    active.SetReported({});                       // the core says none is active: none is
    CHECK(active.Current().empty());
}

void TheShareIsRoundedUpAndCapped()
{
    Settings s = On();
    CHECK(AllowedAtOnce(s, 0) == 0);
    s.SharePct = 2;
    CHECK(AllowedAtOnce(s, 10) == 1);             // 0.2 rounds up to one
    CHECK(AllowedAtOnce(s, 300) == 6);
    CHECK(AllowedAtOnce(s, 1500) == 10);          // MaxBots
    s.SharePct = 0;
    CHECK(AllowedAtOnce(s, 300) == 0);
    s = On();
    s.Enabled = false;
    CHECK(AllowedAtOnce(s, 300) == 0);
}

void SuitsChecksEventSideLevelAndMap()
{
    auto const sites = Sites();
    Bot b = Bots(1)[0];
    CHECK(Suits(sites[0], b, { 7 }));
    CHECK(!Suits(sites[0], b, { 12 }));           // its event is not active
    CHECK(!Suits(sites[0], b, {}));
    CHECK(!Suits(sites[2], b, { 7 }));            // a Horde site, an Alliance bot (and another map)
    b.Side = 0;
    CHECK(!Suits(sites[0], b, { 7 }));            // unknown side: only two-sided sites
    CHECK(Suits(sites[3], b, { 12 }));
    b.Level = 5;
    CHECK(!Suits(sites[3], b, { 12 }));           // the inn wants level 10
    b.Level = 30;
    b.Map = 1;
    CHECK(!Suits(sites[3], b, { 12 }));           // another continent
}

void NothingStartsWithoutAnActiveEventOrWhenOff()
{
    auto const sites = Sites();
    Scheduler off{ Settings{} };
    CHECK(off.Tick(1000, sites, { 7 }, Bots(50)).Starts.empty());
    Scheduler on{ On() };
    CHECK(on.Tick(1000, sites, {}, Bots(50)).Starts.empty());
    CHECK(on.Tick(2000, sites, { 99 }, Bots(50)).Starts.empty());   // an event with no site
}

void TheCapsHold()
{
    auto const sites = Sites();
    Settings s = On();
    s.SharePct = 20;
    Scheduler sched{ s };
    auto const bots = Bots(20);                    // 4 allowed at once; 2 sites for event 7 with 2 per site
    Decisions d = sched.Tick(1000, sites, { 7 }, bots);
    CHECK(d.Starts.size() == 4);
    std::vector<std::uint32_t> perSite(sites.size(), 0);
    for (Start const& st : d.Starts)
        ++perSite[st.Site];
    CHECK(perSite[0] <= 2 && perSite[1] <= 2 && perSite[2] == 0 && perSite[3] == 0);
    CHECK(sched.Tick(11000, sites, { 7 }, bots).Starts.empty());   // the share is used up
    // The starts per tick.
    s.MaxStartsPerTick = 1;
    Scheduler slow{ s };
    CHECK(slow.Tick(1000, sites, { 7 }, bots).Starts.size() == 1);
}

void AVisitEndsAfterTheTimeLimitTheCallOrTheEvent()
{
    auto const sites = Sites();
    Scheduler sched{ On() };
    auto bots = Bots(5);                           // 1 allowed
    Decisions d = sched.Tick(1000, sites, { 7 }, bots);
    CHECK(d.Starts.size() == 1);
    std::uint64_t const guid = d.Starts[0].Guid;
    CHECK(sched.Active(guid) && sched.RunningCount() == 1);
    CHECK(sched.Tick(2000, sites, { 7 }, bots).Ends.empty());
    d = sched.Tick(1000 + 10 * 60000, sites, { 7 }, bots);
    CHECK(d.Ends.size() == 1 && d.Ends[0].Guid == guid && d.Ends[0].Why == EndReason::kTimeLimit && d.Ends[0].TookMs == 10 * 60000);

    Scheduler called{ On() };
    d = called.Tick(1000, sites, { 7 }, bots);
    std::uint64_t const g2 = d.Starts[0].Guid;
    for (Bot& b : bots)
        if (b.Guid == g2)
            b.Called = true;
    d = called.Tick(5000, sites, { 7 }, bots);
    CHECK(d.Ends.size() == 1 && d.Ends[0].Why == EndReason::kCalled);

    Scheduler over{ On() };
    bots = Bots(5);
    d = over.Tick(1000, sites, { 7 }, bots);
    CHECK(d.Starts.size() == 1);
    d = over.Tick(2000, sites, {}, bots);          // the event ended
    CHECK(d.Ends.size() == 1 && d.Ends[0].Why == EndReason::kEventOver && !over.Active(d.Ends[0].Guid));

    Scheduler gone{ On() };
    d = gone.Tick(1000, sites, { 7 }, bots);
    d = gone.Tick(2000, sites, { 7 }, std::vector<Bot>{});   // the bot left the feed
    CHECK(d.Ends.size() == 1 && d.Ends[0].Why == EndReason::kUnavailable);
}

void AFinishedBotWaitsTheGapAndNeverRepeatsASite()
{
    std::vector<std::string> errors;
    auto const sites = ParseSites("7|1|0|-5000|-900|500|1|60|Only|8619\n", errors);
    Scheduler sched{ On() };
    auto const bots = Bots(1);
    Decisions d = sched.Tick(1000, sites, { 7 }, bots);
    CHECK(d.Starts.size() == 1);
    std::uint64_t const took = sched.Finish(100, 31000);
    CHECK(took == 30000 && !sched.Active(100));
    CHECK(sched.Tick(40000, sites, { 7 }, bots).Starts.empty());                 // inside the gap
    CHECK(sched.Tick(31000 + 61 * 60000, sites, { 7 }, bots).Starts.empty());    // gap over, but the one site is done while the event runs
    CHECK(sched.Tick(31000 + 62 * 60000, sites, {}, bots).Starts.empty());       // the event ends: the visited list is forgotten
    CHECK(sched.Tick(31000 + 63 * 60000, sites, { 7 }, bots).Starts.size() == 1);// the event is back
    CHECK(sched.Finish(999, 1) == 0);                                            // not on a visit
}

void BotsOnTheWrongSideOrMapAreNotSent()
{
    auto const sites = Sites();
    Scheduler sched{ On() };
    Decisions const d = sched.Tick(1000, sites, { 7 }, Bots(20, 2, 0));   // Horde on map 0: the only Horde site is on map 1
    CHECK(d.Starts.empty());
    Scheduler horde{ On() };
    Decisions const h = horde.Tick(1000, sites, { 7 }, Bots(20, 2, 1));
    CHECK(!h.Starts.empty());
    for (Start const& st : h.Starts)
        CHECK(st.Site == 2);
}

void TheSameSeedGivesTheSameChoices()
{
    auto const sites = Sites();
    Scheduler a{ On() };
    Scheduler b{ On() };
    auto const da = a.Tick(1000, sites, { 7 }, Bots(20));
    auto const db = b.Tick(1000, sites, { 7 }, Bots(20));
    CHECK(da.Starts.size() == db.Starts.size());
    for (std::size_t i = 0; i < da.Starts.size() && i < db.Starts.size(); ++i)
        CHECK(da.Starts[i].Guid == db.Starts[i].Guid && da.Starts[i].Site == db.Starts[i].Site);
}

void TheJournalLinesHaveNoSpacesInValues()
{
    auto const sites = Sites();
    CHECK(StartLine(100, sites[0]) == "world_event_start bot=100 event=7 site=Elder_Near quests=2");
    CHECK(EndLine(100, sites[0], EndReason::kDone, false, 61000) == "world_event_done bot=100 event=7 site=Elder_Near took_s=61");
    CHECK(EndLine(100, sites[0], EndReason::kTimeLimit, false, 1000).rfind("world_event_abandoned reason=timeout", 0) == 0);
    CHECK(EndLine(100, sites[0], EndReason::kDone, true, 1000).rfind("world_event_abandoned reason=failed", 0) == 0);
}

} // namespace

int main()
{
    TheTableParsesAndABadLineIsDescribed();
    EventIdListsParse();
    TheCoreReportReplacesTheConfiguredList();
    TheShareIsRoundedUpAndCapped();
    SuitsChecksEventSideLevelAndMap();
    NothingStartsWithoutAnActiveEventOrWhenOff();
    TheCapsHold();
    AVisitEndsAfterTheTimeLimitTheCallOrTheEvent();
    AFinishedBotWaitsTheGapAndNeverRepeatsASite();
    BotsOnTheWrongSideOrMapAreNotSent();
    TheSameSeedGivesTheSameChoices();
    TheJournalLinesHaveNoSpacesInValues();
    if (failures == 0)
        std::printf("world_events_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
