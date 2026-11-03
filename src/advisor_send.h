// SPDX-License-Identifier: GPL-2.0-or-later
// The one place that names the protocol 21 SDK calls (BotEmote, BotLfgJoin, BotSpeak kParty / kGuild) and decodes the two protocol 21 events, so an SDK
// rename is a one-file change.
#pragma once

#include "advisor_orders.h"

#include "lfg_queue_fill.h"

#include <cstdint>

namespace forever_module {
class ModuleClient;
class EventNotify;
}

namespace advisor {

// Sends the order. False when it could not be sent (an order with no bot, or an empty line).
bool Send(forever_module::ModuleClient& client, Order const& order);

// The payload of EventNotify::lfg_queue() (hook LfgScript.OnQueueUpdate); false when the notify carries none.
bool Decode(forever_module::EventNotify const& notify, lfg_queue_fill::Queue& out);

}  // namespace advisor
