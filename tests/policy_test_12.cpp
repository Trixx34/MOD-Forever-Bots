// SPDX-License-Identifier: GPL-2.0-or-later
// Policy tests, part 12: dynamic parties (DynamicParties, party_reason.h). The owner's rule (2026-11-14): a bot is in a party only while it makes sense
// (the same quests, a group quest, a dungeon), it leaves for its errands, and it never groups with a bot on another map.
#include "policy_test_support.h"
#include "bot_policy_internal.h"
#include "flex_roles.h"

namespace {

constexpr std::uint32_t kInvite = 1;
constexpr std::uint32_t kLeave = 3;

PolicyOptions DynamicOptions(std::vector<std::string>* events = nullptr)
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 2;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    options.DynamicParties.Enabled = true;
    if (events)
        options.OnEvent = [events](std::string const& text) { events->push_back(text); };
    return options;
}

void AddQuest(BotSnapshot& bot, std::uint32_t quest, std::uint8_t status = 1)
{
    bot.quests[bot.quest_count].quest_id = quest;
    bot.quests[bot.quest_count].status = status;
    ++bot.quest_count;
}

// Member 10 and leader 12, level 5, at the origin of map 0.
WorldTickSnapshotView Pair()
{
    WorldTickSnapshotView view = PartyView({ 10, 12 }, WarriorLeads);
    for (BotSnapshot& bot : view.bots)
        bot.level = 5;
    return view;
}

void Grouped(WorldTickSnapshotView& view)
{
    for (BotSnapshot& bot : view.bots)
    {
        bot.group_role = bot.guid == 12 ? 2 : 1;
        bot.group_leader_guid = PlayerOf(12);
        bot.group_size = 2;
    }
}

void NoSharedQuestNoInvite()
{
    BotPolicy policy(DynamicOptions());
    WorldTickSnapshotView view = Pair();
    CHECK(!HasGroup(policy.Decide(view, 1000), 12, kInvite, PlayerOf(10)));   // nothing in common: each plays on its own

    AddQuest(view.bots[0], 100);
    AddQuest(view.bots[1], 100);
    CHECK(HasGroup(policy.Decide(view, 2000), 12, kInvite, PlayerOf(10)));    // the same quest: the leader invites
}

void AFinishedQuestIsNoReason()
{
    BotPolicy policy(DynamicOptions());
    WorldTickSnapshotView view = Pair();
    AddQuest(view.bots[0], 100, 2);   // the member has done it
    AddQuest(view.bots[1], 100);
    CHECK(!HasGroup(policy.Decide(view, 1000), 12, kInvite, PlayerOf(10)));
}

void AGroupedPairWithNoReasonSplitsUp()
{
    BotPolicy policy(DynamicOptions());
    WorldTickSnapshotView view = Pair();
    Grouped(view);
    auto const actions = policy.Decide(view, 1000);
    CHECK(HasGroup(actions, 10, kLeave));   // a party left from before, with no reason: the member leaves
    CHECK(HasGroup(actions, 12, kLeave));   // and so does the leader
}

void AMemberWhoseLeaderChangesMapLeaves()
{
    std::vector<std::string> events;
    BotPolicy policy(DynamicOptions(&events));
    WorldTickSnapshotView view = Pair();
    AddQuest(view.bots[0], 100);
    AddQuest(view.bots[1], 100);
    Grouped(view);
    auto const together = policy.Decide(view, 1000);
    CHECK(!HasGroup(together, 10, kLeave));
    CHECK(LoggedWith(events, "party_join: bot=10 leader=12 why=quest"));

    view.bots[1].map_id = 1;   // the leader is on another continent now
    view.tick_number = 2;
    auto const apart = policy.Decide(view, 2000);
    CHECK(HasGroup(apart, 10, kLeave));                       // the member leaves instead of standing still
    CHECK(!HasGroup(apart, 12, kInvite, PlayerOf(10)));       // and is not invited across maps
    CHECK(LoggedWith(events, "party_leave: bot=10 leader=12 why=other_map map=0 leader_map=1"));
}

void TooFarIsNotInvited()
{
    BotPolicy policy(DynamicOptions());
    WorldTickSnapshotView view = Pair();
    AddQuest(view.bots[0], 100);
    AddQuest(view.bots[1], 100);
    view.bots[0].pos_x = 300.0f;   // more than PartyJoinYards (150) away
    CHECK(!HasGroup(policy.Decide(view, 1000), 12, kInvite, PlayerOf(10)));
    view.bots[0].pos_x = 50.0f;
    CHECK(HasGroup(policy.Decide(view, 2000), 12, kInvite, PlayerOf(10)));
}

void AMemberOnAnErrandLeaves()
{
    std::vector<std::string> events;
    BotPolicy policy(DynamicOptions(&events));
    WorldTickSnapshotView view = Pair();
    AddQuest(view.bots[0], 100);
    AddQuest(view.bots[1], 100);
    Grouped(view);
    policy.Decide(view, 1000);
    ErrandOrder order;
    order.Map = 0;
    order.Dest = Point{ 500.0f, 0.0f, 0.0f };
    order.Auction = true;
    policy.StartErrand(10, order);   // off to the Auction House
    view.tick_number = 2;
    auto const actions = policy.Decide(view, 2000);
    CHECK(HasGroup(actions, 10, kLeave));
    CHECK(LoggedWith(events, "party_leave: bot=10 leader=12 why=errand"));
}

void AnAssignedRouteKeepsItsParty()
{
    // A dungeon route (and with it a scenario party): every member is in, quests or not.
    BotPolicy policy(DynamicOptions());
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = Pair();
    CHECK(HasGroup(policy.Decide(view, 1000), 12, kInvite, PlayerOf(10)));
}

void OffKeepsTheStaticParties()
{
    PolicyOptions options = DynamicOptions();
    options.DynamicParties.Enabled = false;
    BotPolicy policy(options);
    WorldTickSnapshotView view = Pair();
    CHECK(HasGroup(policy.Decide(view, 1000), 12, kInvite, PlayerOf(10)));   // the old behaviour: the cut is the party
}

// FlexRoles (flex_roles.h). Five bots 10..14: a priest, a mage, a rogue and a hunter, and a paladin (13) in the guid order's last place. Without a warrior the lowest guid leads;
// with the key on the paladin tanks and so leads.
std::uint32_t NoWarriorClasses(std::uint64_t guid)
{
    switch (guid)
    {
        case 10: return 5;    // priest
        case 11: return 8;    // mage
        case 12: return 4;    // rogue
        case 13: return 2;    // paladin
        default: return 3;    // hunter
    }
}

BotPolicy FlexPolicy(bool on, std::function<std::uint32_t(std::uint64_t)> classes, std::vector<std::string>* events = nullptr,
                     std::function<std::string(std::uint64_t)> specOf = nullptr)
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = classes;
    options.FlexRoles = on;
    options.SpecOf = specOf;
    if (events)
        options.OnEvent = [events](std::string const& text) { events->push_back(text); };
    return BotPolicy(options);
}

void FlexRolesOffKeepsTheLowestGuidAsLeader()
{
    BotPolicy policy = FlexPolicy(false, NoWarriorClasses);
    auto const actions = policy.Decide(PartyView({ 10, 11, 12, 13, 14 }, NoWarriorClasses), 0);
    CHECK(HasGroup(actions, 10, kInvite, PlayerOf(13)));
    CHECK(!HasAnyGroup(actions, 13));
}

void FlexRolesPicksAPaladinAsTankAndLeader()
{
    std::vector<std::string> events;
    BotPolicy policy = FlexPolicy(true, NoWarriorClasses, &events);
    auto const actions = policy.Decide(PartyView({ 10, 11, 12, 13, 14 }, NoWarriorClasses), 0);
    CHECK(HasGroup(actions, 13, kInvite, PlayerOf(10)));      // the paladin tanks and leads
    CHECK(HasGroup(actions, 13, kInvite, PlayerOf(14)));
    CHECK(!HasAnyGroup(actions, 10));
    bool logged = false;
    for (std::string const& line : events)
        logged = logged || (line.find("flex_roles: party=") == 0 && line.find("tank=13(class 2) healer=10(class 5)") != std::string::npos);
    CHECK(logged);
}

void AWarriorStaysTheTankWithFlexRoles()
{
    // 13 is a warrior this time and 10 a paladin: the warrior leads, the paladin is no tank.
    auto const classes = [](std::uint64_t guid) -> std::uint32_t { return guid == 13 ? 1 : guid == 10 ? 2 : guid == 11 ? 5 : 8; };
    BotPolicy policy = FlexPolicy(true, classes);
    auto const actions = policy.Decide(PartyView({ 10, 11, 12, 13, 14 }, classes), 0);
    CHECK(HasGroup(actions, 13, kInvite, PlayerOf(10)));
    CHECK(!HasAnyGroup(actions, 10));
}

void ARosterSpecIsNeverChangedByFlexRoles()
{
    // The paladin has the roster spec ret (damage): explicit, so it is no tank candidate and the party is led by the lowest guid as before.
    auto const specOf = [](std::uint64_t guid) { return guid == 13 ? std::string("ret") : std::string(); };
    BotPolicy policy = FlexPolicy(true, NoWarriorClasses, nullptr, specOf);
    auto const actions = policy.Decide(PartyView({ 10, 11, 12, 13, 14 }, NoWarriorClasses), 0);
    CHECK(HasGroup(actions, 10, kInvite, PlayerOf(13)));
    CHECK(!HasAnyGroup(actions, 13));
}

void ADruidTanksWhenNoPaladinIsThere()
{
    auto const classes = [](std::uint64_t guid) -> std::uint32_t { return guid == 14 ? 11 : guid == 10 ? 5 : 8; };
    BotPolicy policy = FlexPolicy(true, classes);
    auto const actions = policy.Decide(PartyView({ 10, 11, 12, 13, 14 }, classes), 0);
    CHECK(HasGroup(actions, 14, kInvite, PlayerOf(10)));
}

void AFlexTankPullsFewerMobsThanAWarrior()
{
    BotSnapshot bot;
    bot.guid = 5;
    std::vector<NearbyUnit> units;
    for (std::uint64_t i = 0; i < 4; ++i)
        units.push_back(Hostile(100 + i, 5.0f + float(i), 100));
    auto const none = [](std::uint64_t) { return false; };
    CHECK(PullTarget(bot, units, none, false, {}, true, {}, kMaxDungeonPull) != nullptr);
    CHECK(PullTarget(bot, units, none, false, {}, true, {}, flex_roles::DungeonPullCap(2, kMaxDungeonPull)) == nullptr);
    CHECK(flex_roles::DungeonPullCap(1, kMaxDungeonPull) == kMaxDungeonPull);
    CHECK(flex_roles::DungeonPullCap(11, kMaxDungeonPull) < kMaxDungeonPull);
}

} // namespace

void RunPolicyTestsPart12()
{
    FlexRolesOffKeepsTheLowestGuidAsLeader();
    FlexRolesPicksAPaladinAsTankAndLeader();
    AWarriorStaysTheTankWithFlexRoles();
    ARosterSpecIsNeverChangedByFlexRoles();
    ADruidTanksWhenNoPaladinIsThere();
    AFlexTankPullsFewerMobsThanAWarrior();
    NoSharedQuestNoInvite();
    AFinishedQuestIsNoReason();
    AGroupedPairWithNoReasonSplitsUp();
    AMemberWhoseLeaderChangesMapLeaves();
    TooFarIsNotInvited();
    AMemberOnAnErrandLeaves();
    AnAssignedRouteKeepsItsParty();
    OffKeepsTheStaticParties();
}
