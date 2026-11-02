// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the guild decision (src/guild_join.h). No core or SDK needed.
#include "guild_join.h"
#include <cstdio>
namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
}  // namespace

using namespace guild;

int main()
{
    Settings on;
    on.Enabled = true;
    Context invited;
    invited.Idle = true;
    invited.InvitePending = true;

    CHECK(Decide(Settings{}, invited) == Action::None);                       // off by default
    CHECK(Decide(on, invited) == Action::Join);
    Context busy = invited; busy.InCombat = true;
    CHECK(Decide(on, busy) == Action::None);
    Context member = invited; member.HasGuild = true;
    CHECK(Decide(on, member) == Action::None);                                // already in one: no second join
    Context leave; leave.HasGuild = true; leave.LeaveRequested = true; leave.Idle = true;
    CHECK(Decide(on, leave) == Action::Leave);
    leave.InCombat = true;
    CHECK(Decide(on, leave) == Action::None);

    if (failures == 0) std::printf("guild_join: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
