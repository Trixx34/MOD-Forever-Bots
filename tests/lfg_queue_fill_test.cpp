// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for LFG queue fill (src/lfg_queue_fill.h) and its policy step (BotPolicy::OnLfgQueue): off by default, the wait, the cooldown per queue, and
// that only an idle bot of the missing role is sent. No core needed.
#include "bot_policy.h"
#include "lfg_queue_fill.h"

#include <cstdio>

using namespace lfg_queue_fill;
using forever_module::BotSnapshot;
using forever_module::WorldTickSnapshotView;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

Settings On()
{
    Settings settings;
    settings.Enabled = true;
    return settings;
}

Queue Waiting(std::uint32_t sec, std::vector<std::uint32_t> roles)
{
    Queue queue;
    queue.MapId = 33;
    queue.QueuedSec = sec;
    queue.OpenRoles = std::move(roles);
    return queue;
}

void OffByDefault()
{
    CHECK(!Settings{}.Enabled);
    CHECK(RoleToFill(Settings{}, Waiting(9999, { kTank })) == 0);
}

void WaitsLongEnoughAndPicksTheMissingRole()
{
    CHECK(RoleToFill(On(), Waiting(59, { kTank })) == 0);
    CHECK(RoleToFill(On(), Waiting(60, { kTank })) == kTank);
    CHECK(RoleToFill(On(), Waiting(90, { kHealer | kDps })) == kHealer);
    CHECK(RoleToFill(On(), Waiting(90, { kDps, kHealer })) == kHealer);
    CHECK(RoleToFill(On(), Waiting(90, {})) == 0);
}

void OneBotPerQueuePerCooldown()
{
    Limiter limiter;
    CHECK(limiter.Ready(33, 1000, 120));
    limiter.Note(33, 1000);
    CHECK(!limiter.Ready(33, 1000 + 119000, 120));
    CHECK(limiter.Ready(33, 1000 + 120000, 120));
    CHECK(limiter.Ready(34, 2000, 120));   // another queue is independent
}

BotSnapshot Bot(std::uint64_t guid)
{
    BotSnapshot bot;
    bot.guid = guid;
    bot.health = bot.max_health = 1000;
    bot.level = 20;
    return bot;
}

WorldTickSnapshotView View(std::vector<BotSnapshot> bots)
{
    WorldTickSnapshotView view;
    view.max_nearby_units = 4;
    view.bots = std::move(bots);
    view.nearby.resize(view.max_nearby_units * view.bots.size());
    return view;
}

void PolicySendsOneIdleBotOfTheRole()
{
    PolicyOptions options;
    options.LfgQueueFill = On();
    options.ClassOf = [](std::uint64_t guid) -> std::uint32_t { return guid == 1 ? 1u : guid == 2 ? 5u : 3u; };   // warrior, priest, hunter
    BotPolicy policy(options);
    BotSnapshot busy = Bot(1);
    busy.in_combat = 1;
    BotSnapshot grouped = Bot(3);
    grouped.group_role = 1;
    WorldTickSnapshotView view = View({ busy, Bot(2), grouped });

    policy.OnLfgQueue(view, Waiting(30, { kHealer }), 1000);   // not waited long enough
    CHECK(policy.TakeAdvisorOrders().empty());
    policy.OnLfgQueue(view, Waiting(70, { kTank }), 1000);     // the only warrior is in combat
    CHECK(policy.TakeAdvisorOrders().empty());
    policy.OnLfgQueue(view, Waiting(70, { kHealer }), 1000);
    auto orders = policy.TakeAdvisorOrders();
    CHECK(orders.size() == 1);
    if (orders.size() == 1)
    {
        CHECK(orders[0].What == advisor::Kind::LfgJoin);
        CHECK(orders[0].Bot == 2);
        CHECK(orders[0].Arg == kHealer);
        CHECK(orders[0].Map == 33);
    }
    policy.OnLfgQueue(view, Waiting(80, { kHealer }), 2000);   // the cooldown of the queue
    CHECK(policy.TakeAdvisorOrders().empty());
    policy.OnLfgQueue(view, Waiting(200, { kHealer }), 1000 + 121000);
    CHECK(policy.TakeAdvisorOrders().size() == 1);
}

void PolicyIsSilentWhenOff()
{
    BotPolicy policy{ PolicyOptions{} };
    WorldTickSnapshotView view = View({ Bot(2) });
    policy.OnLfgQueue(view, Waiting(500, { kTank, kHealer, kDps }), 1000);
    CHECK(policy.TakeAdvisorOrders().empty());
}

}  // namespace

int main()
{
    OffByDefault();
    WaitsLongEnoughAndPicksTheMissingRole();
    OneBotPerQueuePerCooldown();
    PolicySendsOneIdleBotOfTheRole();
    PolicyIsSilentWhenOff();
    if (failures == 0)
        std::printf("lfg_queue_fill_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
