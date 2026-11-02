// SPDX-License-Identifier: GPL-2.0-or-later
#include "guild_join.h"

namespace guild {

Action Decide(Settings const& settings, Context const& ctx)
{
    if (!settings.Enabled)
        return Action::None;
    if (ctx.LeaveRequested && ctx.HasGuild && ctx.Idle && !ctx.InCombat)
        return Action::Leave;
    if (ctx.InvitePending && !ctx.HasGuild && ctx.Idle && !ctx.InCombat)
        return Action::Join;
    return Action::None;
}

}  // namespace guild
