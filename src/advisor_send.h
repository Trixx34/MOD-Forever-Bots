// SPDX-License-Identifier: GPL-2.0-or-later
// The one place that names the protocol 21 SDK calls (BotEmote, BotLfgJoin, BotSpeak kParty / kGuild) and decodes the two protocol 21 events, so an SDK
// rename is a one-file change.
#pragma once

#include "advisor_orders.h"

#include <cstdint>

namespace forever_module {
class ModuleClient;
}

namespace advisor {

// Sends the order. False when it could not be sent (an order with no bot, or an empty line).
bool Send(forever_module::ModuleClient& client, Order const& order);

}  // namespace advisor
