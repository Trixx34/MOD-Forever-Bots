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

} // namespace

int main()
{
    CapCountsPendingAndOwned();
    DeniedRequestFreesSlot();
    RevokeRemovesOwnership();
    ZeroGrantKeepsNothing();

    if (failures == 0)
        std::printf("roster_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
