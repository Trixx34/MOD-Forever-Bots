// Unit tests for BotRoster. No core or SDK needed.
#include "bot_roster.h"

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

void CapCountsPendingAndOwned()
{
    BotRoster roster(2);
    CHECK(roster.CanRequest());
    roster.NoteRequestSent(0);
    CHECK(roster.CanRequest());
    roster.NoteRequestSent(0);
    CHECK(!roster.CanRequest()); // two spawns in flight fill the cap

    roster.OnGranted(0, 101);
    CHECK(roster.PendingCount() == 1);
    CHECK(roster.Owned().count(101) == 1);
    CHECK(!roster.CanRequest()); // one owned + one pending

    roster.OnDenied(0);
    CHECK(roster.PendingCount() == 0);
    CHECK(roster.CanRequest());
}

void DeniedRequestFreesSlot()
{
    BotRoster roster(1);
    roster.NoteRequestSent(555);
    CHECK(!roster.CanRequest());
    roster.OnDenied(555);
    CHECK(roster.CanRequest());
    CHECK(roster.Owned().empty());
}

void RevokeRemovesOwnership()
{
    BotRoster roster(4);
    roster.NoteRequestSent(0);
    roster.OnGranted(0, 7);
    CHECK(roster.OnRevoked(7, "logged out"));
    CHECK(roster.Owned().empty());
    CHECK(roster.LastRevokeReason() == "logged out");
    CHECK(!roster.OnRevoked(7, "again")); // not owned any more
    CHECK(roster.LastRevokeReason() == "logged out");
}

void ZeroGrantKeepsNothing()
{
    BotRoster roster(1);
    roster.NoteRequestSent(0);
    roster.OnGranted(0, 0);
    CHECK(roster.Owned().empty());
    CHECK(roster.PendingCount() == 0);
}

void RaisingTheCapAllowsMoreBots()
{
    // A config reload raises the cap: the roster may now request more, and the bots it already owns stay.
    BotRoster roster(1);
    roster.NoteRequestSent(0);
    roster.OnGranted(0, 101);
    CHECK(!roster.CanRequest());
    roster.SetMaxBots(300);
    CHECK(roster.CanRequest());
    CHECK(roster.Owned().count(101) == 1);
}

void LoweringTheCapKeepsOwnedBots()
{
    // Lowering below the owned count stops new requests but takes nothing back.
    BotRoster roster(3);
    for (std::uint64_t guid = 101; guid <= 103; ++guid)
    {
        roster.NoteRequestSent(0);
        roster.OnGranted(0, guid);
    }
    roster.SetMaxBots(2);
    CHECK(!roster.CanRequest());
    CHECK(roster.Owned().size() == 3);
    CHECK(roster.MaxBots() == 2);
}

void AltsDoNotUseTheCap()
{
    // The cap is full of spawned bots, and an alt is still granted: it is owned, but it does not count against the cap.
    BotRoster roster(1);
    roster.NoteRequestSent(0);
    roster.OnGranted(0, 101);
    CHECK(!roster.CanRequest());
    roster.OnAltGranted(202);
    CHECK(roster.Owned().count(202) == 1);
    CHECK(roster.SpawnedCount() == 1);
    CHECK(!roster.CanRequest()); // the spawn cap is still full from the spawned bot
}

void AltsAreNotCountedForSpawns()
{
    BotRoster roster(2);
    roster.OnAltGranted(301);
    roster.OnAltGranted(302);
    CHECK(roster.Owned().size() == 2);
    CHECK(roster.SpawnedCount() == 0);
    CHECK(roster.CanRequest()); // two alts leave both spawn slots free
    roster.NoteRequestSent(0);
    roster.OnGranted(0, 101);
    roster.NoteRequestSent(0);
    roster.OnGranted(0, 102);
    CHECK(roster.SpawnedCount() == 2);
    CHECK(!roster.CanRequest());
}

void RevokingAnAltFreesNothingForSpawns()
{
    BotRoster roster(1);
    roster.OnAltGranted(401);
    CHECK(roster.OnRevoked(401, "alt removed"));
    CHECK(roster.Owned().empty());
    CHECK(roster.SpawnedCount() == 0);
    CHECK(roster.CanRequest());
}

void ZeroAltGrantKeepsNothing()
{
    BotRoster roster(1);
    roster.OnAltGranted(0);
    CHECK(roster.Owned().empty());
    CHECK(roster.SpawnedCount() == 0);
}

void ABotAwayForAWhileStillUsesTheCapAndComesBack()
{
    BotRoster roster(2);
    roster.OnGranted(0, 11);
    roster.OnGranted(0, 12);
    CHECK(!roster.CanRequest());
    roster.NoteOffline(11);                        // released for presence
    CHECK(roster.Owned().size() == 1 && roster.IsOffline(11) && roster.OfflineCount() == 1);
    CHECK(roster.SpawnedCount() == 2 && !roster.CanRequest());   // no new bot takes its place
    roster.NoteRequestSent(11);
    roster.OnGranted(11, 11);                      // taken back by its guid
    CHECK(!roster.IsOffline(11) && roster.OfflineCount() == 0 && roster.Owned().size() == 2 && roster.SpawnedCount() == 2);
}

void AnAltIsNeverMarkedAway()
{
    BotRoster roster(2);
    roster.OnAltGranted(401);
    roster.NoteOffline(401);
    CHECK(roster.OfflineCount() == 0 && roster.Owned().count(401) == 1);
    roster.NoteOffline(0);
    CHECK(roster.OfflineCount() == 0);
}

} // namespace

int main()
{
    CapCountsPendingAndOwned();
    DeniedRequestFreesSlot();
    RevokeRemovesOwnership();
    ZeroGrantKeepsNothing();
    RaisingTheCapAllowsMoreBots();
    LoweringTheCapKeepsOwnedBots();
    AltsDoNotUseTheCap();
    AltsAreNotCountedForSpawns();
    RevokingAnAltFreesNothingForSpawns();
    ZeroAltGrantKeepsNothing();
    ABotAwayForAWhileStillUsesTheCapAndComesBack();
    AnAltIsNeverMarkedAway();

    if (failures == 0)
        std::printf("roster_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
