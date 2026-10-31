// SPDX-License-Identifier: GPL-2.0-or-later
// The move gate (src/move_gate.h): a move to a place the core found no route to waits, longer each time; other moves pass.
#include <cstdio>
#include <vector>

#include "move_gate.h"

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotActionStatus;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

BotAction Move(std::uint64_t bot, float x, float y)
{
    BotAction action;
    action.bot_guid = bot;
    action.type = ActionType::kMoveTo;
    action.dest_x = x;
    action.dest_y = y;
    return action;
}

std::size_t Sent(move_gate::MoveGate& gate, std::vector<BotAction> actions, std::uint64_t now)
{
    gate.Filter(actions, now);
    return actions.size();
}

constexpr std::uint8_t kMove = static_cast<std::uint8_t>(ActionType::kMoveTo);

void ARefusedMoveWaitsAndTheWaitDoubles()
{
    move_gate::MoveGate gate;
    CHECK(Sent(gate, { Move(1, 100, 200) }, 0) == 1);                          // the first move passes
    gate.Observe(1, kMove, BotActionStatus::kNoRoute, 1, 100);                 // no route: held for 5 s
    CHECK(Sent(gate, { Move(1, 100, 200) }, 1000) == 0);
    CHECK(Sent(gate, { Move(1, 105, 203) }, 4000) == 0);                       // about the same place
    CHECK(Sent(gate, { Move(1, 500, 200) }, 4000) == 1);                       // another place passes
    CHECK(Sent(gate, { Move(1, 100, 200) }, 5200) == 1);                       // after the pause the move goes again
    gate.Observe(1, kMove, BotActionStatus::kNoRoute, 2, 5300);                // refused again: 10 s
    CHECK(Sent(gate, { Move(1, 100, 200) }, 14000) == 0);
    CHECK(Sent(gate, { Move(1, 100, 200) }, 15400) == 1);
}

void OtherBotsAndOtherActionsAreNotHeld()
{
    move_gate::MoveGate gate;
    Sent(gate, { Move(1, 100, 200) }, 0);
    gate.Observe(1, kMove, BotActionStatus::kNoRoute, 1, 100);
    BotAction stop;
    stop.bot_guid = 1;
    stop.type = ActionType::kStop;
    CHECK(Sent(gate, { Move(2, 100, 200), stop }, 1000) == 2);                 // bot 2 and a stop of bot 1 pass
}

void AnAppliedMoveEndsTheRunOfRefusals()
{
    move_gate::MoveGate gate;
    Sent(gate, { Move(1, 100, 200) }, 0);
    gate.Observe(1, kMove, BotActionStatus::kNoRoute, 1, 100);
    gate.Observe(1, kMove, BotActionStatus::kApplied, 2, 6000);
    Sent(gate, { Move(1, 100, 200) }, 6100);
    gate.Observe(1, kMove, BotActionStatus::kNoRoute, 3, 6200);                // the pause starts from 5 s again
    CHECK(Sent(gate, { Move(1, 100, 200) }, 11500) == 1);
}

void TheSameAnswerIsCountedOnce()
{
    move_gate::MoveGate gate;
    Sent(gate, { Move(1, 100, 200) }, 0);
    for (std::uint64_t now = 100; now < 3000; now += 50)
        gate.Observe(1, kMove, BotActionStatus::kNoRoute, 1, now);             // the snapshot repeats the answer every tick
    CHECK(Sent(gate, { Move(1, 100, 200) }, 5200) == 1);                       // still 5 s, not doubled
}

} // namespace

int main()
{
    ARefusedMoveWaitsAndTheWaitDoubles();
    OtherBotsAndOtherActionsAreNotHeld();
    AnAppliedMoveEndsTheRunOfRefusals();
    TheSameAnswerIsCountedOnce();
    if (failures == 0)
        std::printf("move_gate_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
