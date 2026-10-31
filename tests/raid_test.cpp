// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for 40-man raid support (doc 108): raid-convert and subgroup assignment on top of the existing party-formation
// logic (bot_policy.cpp's "Parties (M8)" block, unchanged), and that it leaves five-man scenarios untouched. No core needed:
// kGroup spell_id 4 (ConvertToRaid) and 5 (AssignSubgroup) are asserted the same way policy_test.cpp asserts spell_id 1-3.
#include "bot_policy.h"

#include <cstdio>
#include <string>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotSnapshot;
using forever_module::MakeUnitRef;
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

std::uint64_t PlayerRefOf(std::uint64_t counter)
{
    return MakeUnitRef(2, 0, counter); // HighGuid::Player = 2, same packing as bot_policy.cpp's PlayerRef
}

// `count` bots, guid 1..count, already members of one party/raid led by bot 1 (group_role 2), the rest group_role 1 with
// group_leader_guid pointing at bot 1 -- the state once every invite has been accepted, which is what the raid-convert and
// subgroup-assign logic acts on. classOf(guid) picks the class (1 = warrior at positions a multiple of 5, so one tank per
// intended subgroup; 5 = priest heals; 1-off dps otherwise), mirroring the "8 repeats of a 5-man composition" roster the
// scenario config doc asks for.
WorldTickSnapshotView RaidRosterView(std::size_t count)
{
    WorldTickSnapshotView view;
    view.tick_number = 1;
    view.max_nearby_units = 1;
    std::uint64_t const leaderRef = PlayerRefOf(1);
    for (std::uint64_t guid = 1; guid <= count; ++guid)
    {
        BotSnapshot bot;
        bot.guid = guid;
        bot.health = 1000;
        bot.max_health = 1000;
        bot.group_role = guid == 1 ? 2 : 1;
        bot.group_leader_guid = leaderRef;
        bot.group_size = static_cast<std::uint32_t>(count);
        view.bots.push_back(bot);
    }
    view.nearby.resize(count);
    return view;
}

std::uint32_t ClassFor5PerGroup(std::uint64_t guid)
{
    std::uint64_t const posInGroup = (guid - 1) % 5; // 0 = tank, 1 = healer, 2-4 = dps, same cut as a 5-man roster
    return posInGroup == 0 ? 1u /* warrior */ : posInGroup == 1 ? 5u /* priest */ : 9u /* warlock */;
}

bool HasGroupAction(std::vector<BotAction> const& actions, std::uint64_t bot, std::uint32_t spell, std::uint64_t target = 0)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kGroup && action.bot_guid == bot && action.spell_id == spell &&
            (target == 0 || action.target_guid == target))
            return true;
    return false;
}

PolicyOptions RaidOptions(std::uint32_t rosterSize)
{
    PolicyOptions options;
    options.PartySize = rosterSize;
    options.PartyCount = 1;
    options.PartyPercent = 100;
    options.RandomPct = 0;
    options.Raid = true;
    options.ClassOf = ClassFor5PerGroup;
    return options;
}

// The leader of a 40-bot raid roster asks ConvertToRaid (idempotent, retried regardless of whether it already is one: the
// snapshot has no is_raid field to check against).
void LeaderConvertsItsPartyToARaid()
{
    BotPolicy policy(RaidOptions(40));
    auto const actions = policy.Decide(RaidRosterView(40), 0);
    CHECK(HasGroupAction(actions, 1, 4));
}

// Every already-joined member is put in the subgroup its 1-based roster position implies (5 per subgroup, leader first):
// bot 6 (position 5, 0-based) is the first member of subgroup 1, bot 40 (position 39) is the last of subgroup 7.
void MembersAreAssignedToTheSubgroupTheirPositionImplies()
{
    BotPolicy policy(RaidOptions(40));
    auto const actions = policy.Decide(RaidRosterView(40), 0);
    CHECK(HasGroupAction(actions, 1, 5, PlayerRefOf(2)));  // position 1 -> subgroup 0 (2/5 = 0)
    CHECK(HasGroupAction(actions, 1, 5, PlayerRefOf(6)));  // position 5 -> subgroup 1 (6/5 = 1)
    CHECK(HasGroupAction(actions, 1, 5, PlayerRefOf(40))); // position 39 -> subgroup 7 (39/5 = 7)
    for (BotAction const& action : actions)
    {
        if (action.type != ActionType::kGroup || action.spell_id != 5)
            continue;
        if (action.target_guid == PlayerRefOf(2))
            CHECK(action.dest_x == 0.0f);
        else if (action.target_guid == PlayerRefOf(6))
            CHECK(action.dest_x == 1.0f);
        else if (action.target_guid == PlayerRefOf(21))
            CHECK(action.dest_x == 4.0f); // position 20 -> 21/5 = 4
    }
}

// A member not seen yet (offline, or not yet confirmed into this leader's party) is not assigned: it is picked up once it
// shows group_leader_guid == the leader, the same tick its invite is accepted.
void AnUnconfirmedMemberIsNotAssignedYet()
{
    PolicyOptions options = RaidOptions(10);
    BotPolicy policy(options);
    WorldTickSnapshotView view = RaidRosterView(10);
    view.bots[9].group_leader_guid = 0; // bot 10 (guid 10) has not joined yet
    view.bots[9].group_role = 0;
    auto const actions = policy.Decide(view, 0);
    CHECK(!HasGroupAction(actions, 1, 5, PlayerRefOf(10)));
}

// Five-man scenarios (and the plain PartySize/PartyCount feature) never set PolicyOptions::Raid, so spell_id 4/5 are never
// sent: a dungeon run's grouping is exactly as before this feature.
void FiveManScenarioNeverSendsRaidActions()
{
    PolicyOptions options;
    options.PartySize = 5;
    options.PartyCount = 1;
    options.PartyPercent = 100;
    options.ClassOf = ClassFor5PerGroup;
    options.Raid = false; // the default; spelled out for clarity
    BotPolicy policy(options);
    auto const actions = policy.Decide(RaidRosterView(5), 0);
    for (BotAction const& action : actions)
        CHECK(!(action.type == ActionType::kGroup && (action.spell_id == 4 || action.spell_id == 5)));
}

} // namespace

int main()
{
    LeaderConvertsItsPartyToARaid();
    MembersAreAssignedToTheSubgroupTheirPositionImplies();
    AnUnconfirmedMemberIsNotAssignedYet();
    FiveManScenarioNeverSendsRaidActions();

    if (failures == 0)
        std::printf("raid_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
