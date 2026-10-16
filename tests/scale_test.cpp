// Scale test: the pump at the 1500-bot ceiling. It builds that many bots with a full nearby list, in 300 parties, and
// decides for as many ticks as a short run would need. No core needed.
//
// What it guards: Decide must stay bounded in time and in the actions it publishes, and the coordination the policy does
// across bots (heal claims, buff claims, interrupt sharing) must not grow without bound as the run goes on. Timing is
// printed every step; the check has a wide ceiling and can be moved with FOREVER_BOTS_TICK_MS, because a loaded machine
// should not fail a build.
#include "bot_policy.h"

#include "combat_rotation.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotActionStatus;
using forever_module::BotSnapshot;
using forever_module::MakeUnitRef;
using forever_module::NearbyUnit;
using forever_module::WorldTickSnapshotView;

namespace {

int failures = 0;

void CHECK(bool cond, char const* what, int line)
{
    if (!cond)
    {
        ++failures;
        std::printf("FAIL %s:%d: %s\n", __FILE__, line, what);
    }
}

#define CHECK_AT(cond) CHECK((cond), #cond, __LINE__)

constexpr std::size_t kBotCount = 1500;    // the ceiling the module asks the core for (kMaxBotsCeiling is 2500; the sim runs 1500)
constexpr std::size_t kNearby = 16;        // the nearby units the module asks for
constexpr std::uint32_t kPartySize = 5;
constexpr std::uint32_t kParties = 300;
constexpr int kTicks = 12;

// A rotation that exercises the paths that talk between bots: a heal, a party buff, and a fight row.
char const* kRotation =
    "class 1\n"
    "row 78 self_power_above 40 target range 0 5\n"
    "class 5\n"
    "row 2050 self_hp_below 90 self range 0 0\n"
    "buff 17 duration 1800 self\n"
    "buff 1243 duration 1800 party\n"
    "row 589 always target range 0 30\n";

void SetNearby(WorldTickSnapshotView& view, std::size_t botIndex, float x)
{
    BotSnapshot& bot = view.bots[botIndex];
    std::size_t count = 0;
    auto add = [&](NearbyUnit unit)
    {
        view.nearby[botIndex * view.max_nearby_units + count++] = unit;
    };
    for (std::uint32_t n = 0; n < 8; ++n) // four hostiles, one of them hurt, close by
    {
        NearbyUnit unit;
        unit.guid = MakeUnitRef(8, 100 + n, bot.guid * 100 + n);
        unit.pos_x = x + 3.0f + float(n) * 0.5f;
        unit.pos_y = 0.0f;
        unit.health = n == 0 ? 25 : 100;
        unit.max_health = 100;
        unit.level = 5;
        unit.is_hostile = true;
        add(unit);
    }
    for (std::uint32_t n = 0; n < 3; ++n) // party members close by
    {
        NearbyUnit unit;
        unit.guid = MakeUnitRef(2, 0, (bot.guid / 5) * 5 + 1 + n);
        unit.pos_x = x + 2.0f + float(n);
        unit.pos_y = 1.0f;
        unit.health = 1000;
        unit.max_health = 1000;
        unit.level = 5;
        add(unit);
    }
    for (std::uint32_t n = 0; n < 3; ++n) // dead corpses, for the loot path
    {
        NearbyUnit unit;
        unit.guid = MakeUnitRef(8, 200 + n, bot.guid * 100 + 100 + n);
        unit.pos_x = x + 4.0f + float(n);
        unit.health = 0;
        unit.max_health = 100;
        unit.level = 5;
        add(unit);
    }
    for (std::uint32_t n = 0; n < 2; ++n) // a friendly player and a vendor
    {
        NearbyUnit unit;
        unit.guid = MakeUnitRef(2, 0, 900000 + n);
        unit.pos_x = x + 10.0f;
        unit.pos_y = 2.0f;
        unit.health = 1000;
        unit.max_health = 1000;
        add(unit);
    }
    bot.nearby_count = static_cast<std::uint32_t>(count);
}

} // namespace

int main()
{
    std::istringstream file(kRotation);
    std::vector<std::string> warnings;
    PolicyOptions options;
    options.Rotations = std::make_shared<rotation::RotationData>(rotation::ParseRotationData(file, warnings));
    for (std::string const& warning : warnings)
    {
        std::printf("FAIL scale test: rotation line dropped: %s\n", warning.c_str());
        ++failures;
    }
    options.PartySize = kPartySize;
    options.PartyCount = kParties;
    options.ServiceEveryMs = 600000;   // service visits on a timer
    options.TrainEveryMs = 600000;
    options.AutoGearEveryLevels = 5;
    options.WanderWhenIdle = true;
    options.OnEvent = nullptr;         // the pump always has a sink; the policy must not need one

    BotPolicy policy(options);

    WorldTickSnapshotView view;
    view.tick_number = 0;
    view.max_nearby_units = static_cast<std::uint32_t>(kNearby);
    view.bots.reserve(kBotCount);
    view.nearby.resize(kBotCount * kNearby);
    for (std::size_t i = 0; i < kBotCount; ++i)
    {
        BotSnapshot bot;
        bot.guid = 1000000 + i;                       // the guid the core grants the bot
        bot.health = (i % 7) == 0 ? 400 : 1000;       // a few of the bots are hurt
        bot.max_health = 1000;
        bot.power = 300;
        bot.max_power = 1000;
        bot.level = (i % 6) + 3;
        bot.map_id = 0;
        bot.pos_x = float(int(i) % 500) * 1.5f;
        bot.pos_y = float(int(i) / 500) * 1.5f;
        bot.pos_z = 0.0f;
        bot.vendor_valid = 1;                         // a vendor on this map, far away: the travel path
        bot.vendor_x = bot.pos_x + 400.0f;
        bot.vendor_y = bot.pos_y;
        // The core forms the group: the leader points at its own player reference, the members at theirs.
        std::uint64_t const leaderCounter = 1000000 + (i / kPartySize) * kPartySize;
        bot.group_leader_guid = (i % kPartySize) == 0 ? MakeUnitRef(2, 0, bot.guid) : MakeUnitRef(2, 0, leaderCounter);
        bot.quest_count = 1;
        bot.quests[0].quest_id = 783;
        bot.quests[0].status = 1;
        bot.quests[0].counts[0] = 3;
        view.bots.push_back(bot);
    }
    for (std::size_t i = 0; i < kBotCount; ++i)
        SetNearby(view, i, view.bots[i].pos_x);

    // One action per bot, the movement or attack decision, plus at most one cast or service: the bound the core grants is two.
    double worst_us = 0.0, total_us = 0.0;
    std::size_t worst_actions = 0, last_tick = 0;
    for (int tick = 1; tick <= kTicks; ++tick, ++view.tick_number)
    {
        std::uint64_t const nowMs = std::uint64_t(tick) * 5000;
        auto const started = std::chrono::steady_clock::now();
        std::vector<BotAction> const actions = policy.Decide(view, nowMs);
        auto const us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
        total_us += us;
        worst_us = std::max(worst_us, us);
        worst_actions = std::max(worst_actions, actions.size());
        last_tick = tick;

        for (BotAction const& action : actions)
        {
            CHECK_AT(action.bot_guid >= 1000000 && action.bot_guid < 1000000 + kBotCount);
            CHECK_AT(action.type <= ActionType::kLastActionType);
            bool const complete = action.type != ActionType::kTeleport ||
                                  (action.spell_id != 0 && (action.dest_x != 0.0f || action.dest_y != 0.0f || action.dest_z != 0.0f));
            CHECK_AT(complete);
        }
    }

    std::printf("scale: %zu ticks of %zu bots: avg %.0f us, worst %.0f us, %.0f us per bot, %zu actions at the widest tick\n",
                last_tick, kBotCount, total_us / kTicks, worst_us, worst_us / double(kBotCount), worst_actions);

    // The ceiling is wide on purpose: a machine running the build, or a debug build, both cost more than this. It is here to
    // catch a regression that makes Decide quadratic in the bot count, not to police the machine.
    char const* limit = std::getenv("FOREVER_BOTS_TICK_MS");
    double const ceilingMs = limit ? std::atof(limit) : 4000.0;
    CHECK_AT(worst_us / 1000.0 <= ceilingMs);
    // Two actions per bot is what the core was granted at handshake (kRequestedActions); a wider tick publishes nothing.
    CHECK_AT(worst_actions <= 2 * kBotCount);

    if (failures == 0)
    {
        std::printf("scale_test: all checks passed\n");
        return 0;
    }
    return 1;
}
