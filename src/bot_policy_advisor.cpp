// SPDX-License-Identifier: GPL-2.0-or-later
// The advisor features that need the policy's state (LFG queue fill, trade ads, party mana, guild milestones, city inspection, quest invites): each is a pure
// decision module (lfg_queue_fill.h and so on) asked here. All are off by default; each acts only for a bot that is out of combat, has no target and sees no
// hostile. The orders they make are sent by advisor_send.h.
#include "bot_policy_internal.h"

std::vector<advisor::Order> BotPolicy::TakeAdvisorOrders()
{
    std::vector<advisor::Order> out;
    out.swap(_advisorOrders);
    return out;
}
