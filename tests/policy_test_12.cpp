// SPDX-License-Identifier: GPL-2.0-or-later
// Policy tests, part 12: dynamic parties (DynamicParties, party_reason.h). The owner's rule (2026-11-14): a bot is in a party only while it makes sense
// (the same quests, a group quest, a dungeon), it leaves for its errands, and it never groups with a bot on another map.
#include "policy_test_support.h"
#include "tame_beasts.h"
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

// Hunter taming with a dead stored pet and no Revive Pet (prod 2026-11-17: tame_failed 814 of 817): the Tame / Call Pet / Revive Pet loop stops and waits
// TameDeadPetBackoffMs, with one event, when the key is on; with it off the loop goes on as before.
static std::vector<std::string> DeadPetRun(std::uint64_t backoffMs, bool& tamedAgain)
{
    auto beasts = std::make_shared<tame::TameBeasts>();
    beasts->ByEntry[1] = tame::Beast{ 1, 1, 12 };
    PolicyOptions options = AggressiveOptions();
    options.ClassId = 3;
    options.TameBeasts = beasts;
    options.TameDeadPetBackoffMs = backoffMs;
    std::vector<std::string> events;
    options.OnEvent = [&](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    NearbyUnit beast = Unit(8, 7, 5.0f, false, 100);
    beast.level = 8;
    auto const build = [&](std::uint64_t tick)
    {
        WorldTickSnapshotView view = HunterView(tick, 14, { beast });
        view.bots[0].pet_state = 0;
        return view;
    };
    auto const result = [&](WorldTickSnapshotView& view, std::uint32_t spell, BotActionStatus status, std::uint64_t seq)
    {
        view.bots[0].last_action_sequence = seq;
        view.bots[0].last_action_type = static_cast<std::uint8_t>(ActionType::kCastSpell);
        view.bots[0].last_action_spell = spell;
        view.bots[0].last_action_status = status;
    };
    std::uint64_t now = 1000;
    std::uint64_t tick = 1;
    std::uint64_t seq = 0;
    auto step = [&](std::uint32_t spell, BotActionStatus status) -> std::vector<BotAction>
    {
        WorldTickSnapshotView view = build(++tick);
        if (spell != 0)
            result(view, spell, status, ++seq);
        now += 3000;
        return policy.Decide(view, now);
    };
    bool tamed = false;
    for (int i = 0; i < 4 && !tamed; ++i)
        tamed = HasCast(step(0, BotActionStatus::kApplied), 1515);
    CHECK(tamed);
    step(1515, BotActionStatus::kRefused);
    now += 30000;
    for (int i = 0; i < 3; ++i)
        step(0, BotActionStatus::kApplied);
    step(883, BotActionStatus::kRefused);
    step(982, BotActionStatus::kNotKnown);
    tamedAgain = false;
    for (int i = 0; i < 8; ++i)
    {
        now += 3000;
        tamedAgain = tamedAgain || HasCast(step(0, BotActionStatus::kApplied), 1515);
    }
    return events;
}

void ADeadPetWithoutRevivePetStopsTheTameLoopWhenKeyed()
{
    bool again = true;
    auto const on = DeadPetRun(1800000, again);
    bool seen = false;
    for (std::string const& e : on)
        seen = seen || e.find("why=pet_dead_no_revive") != std::string::npos;
    CHECK(seen);
    CHECK(!again);
    bool offAgain = false;
    auto const off = DeadPetRun(0, offAgain);
    for (std::string const& e : off)
        CHECK(e.find("why=pet_dead_no_revive") == std::string::npos);
}

// ---- Level dungeons (DriveLevelDungeons): the early, side, roles, file-duration and decision-log keys ----

constexpr std::uint32_t kRfcMap = 389;

// A formed party of three on map 0 led by warrior 12 (core group: 12 leads, 10 and 11 follow), all at `level`. Classes: 12 warrior, 10 `class10`, 11 mage.
struct LevelParty
{
    PolicyOptions Options;
    WorldTickSnapshotView View;
    std::vector<std::string> Events;
};

void MakeLevelParty(LevelParty& party, std::uint32_t level, std::uint32_t class10 = 5, std::uint8_t side = 2)
{
    PolicyOptions& options = party.Options;
    options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.LevelDungeons = true;
    options.ClassOf = [class10](std::uint64_t guid) { return guid == 12 ? 1u : guid == 10 ? class10 : 8u; };
    options.PartySideOf = [side](std::uint64_t) { return side; };
    options.OnEvent = [events = &party.Events](std::string const& text) { events->push_back(text); };
    PolicyOptions::DungeonRoute rfc;
    rfc.Id = "rfc";
    rfc.Name = "Ragefire Chasm";
    rfc.MinLevel = 13;
    rfc.MaxLevel = 18;
    rfc.HasEntry = true;
    rfc.Entry = Point{ 1.0f, 2.0f, 3.0f };
    rfc.Route = { Point{ 500.0f, 500.0f, 0.0f }, Point{ 900.0f, 900.0f, 0.0f } };   // far from the entry: the lead does not reach the end in the test
    rfc.DurationSec = 60;
    options.DungeonsByMap[kRfcMap] = rfc;
    party.View = PartyView({ 10, 11, 12 }, options.ClassOf);
    for (BotSnapshot& bot : party.View.bots)
    {
        bot.level = level;
        bot.group_role = bot.guid == 12 ? 2 : 1;
        bot.group_leader_guid = PlayerOf(12);
        bot.group_size = 3;
    }
}

bool TeleportsInto(std::vector<BotAction> const& actions, std::uint64_t bot, std::uint32_t map)
{
    for (BotAction const& action : actions)
        if (action.bot_guid == bot && action.type == ActionType::kTeleport && action.spell_id == map)
            return true;
    return false;
}

std::size_t CountEvents(std::vector<std::string> const& events, std::string const& prefix)
{
    std::size_t count = 0;
    for (std::string const& e : events)
        count += e.rfind(prefix, 0) == 0 ? 1 : 0;
    return count;
}

bool HasEvent(std::vector<std::string> const& events, std::string const& part)
{
    for (std::string const& e : events)
        if (e.find(part) != std::string::npos)
            return true;
    return false;
}

void LevelDungeonDefaultsGoOnlyAtTheMaxLevel()
{
    LevelParty early;
    MakeLevelParty(early, 15);
    BotPolicy policy(early.Options);
    CHECK(!TeleportsInto(policy.Decide(early.View, 1000), 12, kRfcMap));   // 15 is below rfc's max (18): not due by default
    CHECK(CountEvents(early.Events, "level_dungeon_") == 0);              // no decision log by default

    LevelParty atMax;
    MakeLevelParty(atMax, 18);
    BotPolicy policy18(atMax.Options);
    auto const actions = policy18.Decide(atMax.View, 1000);
    CHECK(TeleportsInto(actions, 12, kRfcMap) && TeleportsInto(actions, 10, kRfcMap) && TeleportsInto(actions, 11, kRfcMap));
    CHECK(CountEvents(atMax.Events, "level_dungeon_start") == 1);
    CHECK(CountEvents(atMax.Events, "level_dungeon_due") == 0);
}

void LevelDungeonEarlyLevelsSendsBelowTheMax()
{
    LevelParty party;
    MakeLevelParty(party, 15);
    party.Options.LevelDungeonEarlyLevels = 3;   // due from 18 - 3 = 15
    party.Options.LevelDungeonLog = true;
    BotPolicy policy(party.Options);
    CHECK(TeleportsInto(policy.Decide(party.View, 1000), 12, kRfcMap));
    CHECK(HasEvent(party.Events, "level_dungeon_due: leader=12 dungeon=rfc level=15 band=13-18 early=1"));

    LevelParty low;
    MakeLevelParty(low, 14);
    low.Options.LevelDungeonEarlyLevels = 3;
    low.Options.LevelDungeonLog = true;
    BotPolicy lowPolicy(low.Options);
    CHECK(!TeleportsInto(lowPolicy.Decide(low.View, 1000), 12, kRfcMap));   // 14 is below 15
    CHECK(HasEvent(low.Events, "level_dungeon_skip: leader=12 level=14 reason=no_dungeon"));
}

void LevelDungeonSidesKeepAPartyOutOfTheOtherSidesDungeon()
{
    LevelParty alliance;
    MakeLevelParty(alliance, 18, 5, 1);
    alliance.Options.LevelDungeonSides = { { "rfc", 2 } };   // Ragefire is Horde only
    alliance.Options.LevelDungeonLog = true;
    BotPolicy policy(alliance.Options);
    CHECK(!TeleportsInto(policy.Decide(alliance.View, 1000), 12, kRfcMap));
    CHECK(HasEvent(alliance.Events, "reason=wrong_side"));

    LevelParty horde;
    MakeLevelParty(horde, 18, 5, 2);
    horde.Options.LevelDungeonSides = { { "rfc", 2 } };
    BotPolicy hordePolicy(horde.Options);
    CHECK(TeleportsInto(hordePolicy.Decide(horde.View, 1000), 12, kRfcMap));
}

void LevelDungeonNeedRolesWantsATankAndAHealer()
{
    LevelParty noHealer;
    MakeLevelParty(noHealer, 18, 4);   // warrior, rogue, mage
    noHealer.Options.LevelDungeonNeedRoles = true;
    noHealer.Options.LevelDungeonLog = true;
    BotPolicy policy(noHealer.Options);
    CHECK(!TeleportsInto(policy.Decide(noHealer.View, 1000), 12, kRfcMap));
    CHECK(HasEvent(noHealer.Events, "reason=no_tank_healer"));
    // The skip is logged once, not every tick.
    policy.Decide(noHealer.View, 2000);
    policy.Decide(noHealer.View, 3000);
    CHECK(CountEvents(noHealer.Events, "level_dungeon_skip") == 1);

    LevelParty keyOff;
    MakeLevelParty(keyOff, 18, 4);
    BotPolicy offPolicy(keyOff.Options);
    CHECK(TeleportsInto(offPolicy.Decide(keyOff.View, 1000), 12, kRfcMap));   // the key off: the old behaviour, no role check

    LevelParty withPriest;
    MakeLevelParty(withPriest, 18, 5);
    withPriest.Options.LevelDungeonNeedRoles = true;
    BotPolicy priestPolicy(withPriest.Options);
    CHECK(TeleportsInto(priestPolicy.Decide(withPriest.View, 1000), 12, kRfcMap));
}

// A party due by level but with the leader in combat logs leader_busy once, then goes when the fight is over; each change is a new line.
void LevelDungeonLogNamesWhyADuePartyWaits()
{
    LevelParty party;
    MakeLevelParty(party, 18);
    party.Options.LevelDungeonLog = true;
    BotPolicy policy(party.Options);
    party.View.bots[2].in_combat = 1;   // leader 12
    CHECK(!TeleportsInto(policy.Decide(party.View, 1000), 12, kRfcMap));
    policy.Decide(party.View, 1100);
    CHECK(CountEvents(party.Events, "level_dungeon_skip: leader=12 level=18 reason=leader_busy") == 1);
    party.View.bots[2].in_combat = 0;
    party.View.bots[0].life_state = 1;  // member 10 dead
    CHECK(!TeleportsInto(policy.Decide(party.View, 1200), 12, kRfcMap));
    CHECK(HasEvent(party.Events, "reason=member_not_ready"));
    party.View.bots[0].life_state = 0;
    CHECK(TeleportsInto(policy.Decide(party.View, 1300), 12, kRfcMap));
    CHECK(CountEvents(party.Events, "level_dungeon_due") == 1 && CountEvents(party.Events, "level_dungeon_start") == 1);
}

// Runs the party into rfc and keeps it there; returns whether the run ended as a timeout by `checkAtMs`.
bool LevelRunTimedOut(bool fileDuration, std::uint64_t checkAtMs)
{
    LevelParty party;
    MakeLevelParty(party, 18);
    party.Options.LevelDungeonFileDuration = fileDuration;
    BotPolicy policy(party.Options);
    CHECK(TeleportsInto(policy.Decide(party.View, 1000), 12, kRfcMap));
    for (BotSnapshot& bot : party.View.bots)
    {
        bot.map_id = kRfcMap;
        bot.pos_x = 1.0f;
        bot.pos_y = 2.0f;
        bot.pos_z = 3.0f;
    }
    policy.Decide(party.View, 2000);                       // in: the run starts now
    CHECK(HasEvent(party.Events, "level_dungeon_in"));
    policy.Decide(party.View, checkAtMs);
    return HasEvent(party.Events, "result=timeout");
}

void LevelDungeonFileDurationCapsTheRun()
{
    CHECK(LevelRunTimedOut(true, 2000 + 61 * 1000));       // the file's 60 s
    CHECK(!LevelRunTimedOut(false, 2000 + 61 * 1000));     // the fixed 45 minutes
    CHECK(LevelRunTimedOut(false, 2000 + 46 * 60 * 1000));
}


// Professions (doc/professions.md): the craft plan names the easiest recipe of the table after a kSkillUp with nothing to make, skips a recipe the bot does not
// know, and stops at the cap; a rise of the skill writes profession_skillup.
std::vector<std::uint32_t> PlannedCrafts(std::vector<BotAction> const& actions)
{
    std::vector<std::uint32_t> spells;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kCraft)
            spells.push_back(action.spell_id);
    return spells;
}

void ProfessionsCraftPlanNamesTheEasiestRecipeAndStopsAtTheCap()
{
    std::vector<std::string> events;
    PolicyOptions options = AggressiveOptions();
    options.Crafting = true;
    options.Professions.Enabled = true;
    options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
    std::vector<std::string> errors;
    options.ProfessionRecipes = std::make_shared<std::vector<professions::Recipe>>(professions::ParseRecipeTable("197 100 1 40 75 2589:1\n197 101 1 25 50 2589:2\n", errors));
    BotPolicy policy(options);
    WorldTickSnapshotView view = View(1, 0, {});
    view.bots[0].level = 10;
    view.bots[0].profession_skill[0] = 197; // Tailoring
    view.bots[0].profession_value[0] = 10;
    view.bots[0].profession_max[0] = 75;
    CHECK((CraftsOf(policy.Decide(view, 1000)) == std::vector<std::uint32_t>{ 129 }));
    CraftAnswer(view, BotActionStatus::kRefused, 1);
    CHECK((CraftsOf(policy.Decide(view, 3000)) == std::vector<std::uint32_t>{ 185 }));
    CraftAnswer(view, BotActionStatus::kRefused, 2);
    CHECK((CraftsOf(policy.Decide(view, 5000)) == std::vector<std::uint32_t>{ 197 }));
    CraftAnswer(view, BotActionStatus::kRefused, 3);                                     // no recipe with materials
    policy.Decide(view, 7000);
    CHECK((PlannedCrafts(policy.Decide(view, 9000)) == std::vector<std::uint32_t>{ 101 })); // the easiest that still raises 10
    view.bots[0].last_action_type = std::uint8_t(ActionType::kCraft);
    view.bots[0].last_action_status = BotActionStatus::kNotKnown;
    view.bots[0].last_action_sequence = 4;
    policy.Decide(view, 11000);
    CHECK(HasEvent(events, "profession_craft_plan: bot=42 skill=197 recipe=101 status=not_known"));
    CHECK((PlannedCrafts(policy.Decide(view, 13000)) == std::vector<std::uint32_t>{ 100 })); // the next one
    // A rise of the skill: one line.
    view.bots[0].profession_value[0] = 11;
    policy.Decide(view, 14000);
    CHECK(HasEvent(events, "profession_skillup: bot=42 skill=197 from=10 to=11"));
    // At the cap (level 2: 10): no Tailoring ask, one profession_cap line.
    PolicyOptions capped = options;
    BotPolicy low(capped);
    WorldTickSnapshotView lowView = View(1, 0, {});
    lowView.bots[0].level = 2;
    lowView.bots[0].profession_skill[0] = 197;
    lowView.bots[0].profession_value[0] = 10;
    lowView.bots[0].profession_max[0] = 75;
    for (std::uint64_t t = 1000; t < 20000; t += 2000)
    {
        for (std::uint32_t skill : CraftsOf(low.Decide(lowView, t)))
            CHECK(skill != 197);
        CraftAnswer(lowView, BotActionStatus::kRefused, std::uint32_t(t));
    }
    CHECK(HasEvent(events, "profession_cap: bot=42 skill=197 value=10 cap=10 level=2"));
}

// Professions node walk: a node near the bot, never one found empty, never one beyond ProfessionNodeMaxYards; Gathering itself may be off.
void ProfessionsNodeWalkSkipsEmptyAndFarNodes()
{
    auto nodes = std::make_shared<std::vector<gather::NodePoint>>();
    for (float x : { 100.0f, 160.0f, 400.0f })   // 60 yards apart: more than the 25 yards of "arrived"
    {
        gather::NodePoint node;
        node.Skill = 186; node.Map = 0; node.X = x; node.RequiredSkill = 1; node.AreaLevel = 1; node.Entry = std::uint32_t(x);
        nodes->push_back(node);
    }
    std::vector<std::string> events;
    PolicyOptions options = AggressiveOptions();
    options.Professions.Enabled = true;
    options.Professions.NodeGapMs = 0;
    options.GatherNodes = nodes;
    options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
    BotPolicy policy(options);
    WorldTickSnapshotView view = View(1, 0, {});
    view.bots[0].level = 10;
    view.bots[0].profession_skill[0] = 186; // Mining
    view.bots[0].profession_value[0] = 10;
    CHECK((GathersOf(policy.Decide(view, 1000)) == std::vector<std::uint32_t>{ 186 }));
    view.bots[0].last_action_type = std::uint8_t(ActionType::kGather);
    view.bots[0].last_action_status = BotActionStatus::kRefused;
    view.bots[0].last_action_sequence = 1;
    float const first = WalkTo(policy.Decide(view, 3000), kBot);
    float const second = WalkTo(policy.Decide(view, 4000), kBot);
    CHECK(first == 100.0f || second == 100.0f);                                             // the walk starts with the answer (or the tick after a mount)
    CHECK(HasEvent(events, "profession_node_walk: bot=42 skill=186 node=100 yards=100"));
    view.bots[0].pos_x = 98.0f;                                                             // arrived
    view.bots[0].last_action_type = std::uint8_t(ActionType::kMoveTo);
    view.bots[0].last_action_status = BotActionStatus::kApplied;
    CHECK((GathersOf(policy.Decide(view, 20000)) == std::vector<std::uint32_t>{ 186 }));
    view.bots[0].last_action_type = std::uint8_t(ActionType::kGather);
    view.bots[0].last_action_status = BotActionStatus::kRefused;                            // the node is empty
    view.bots[0].last_action_sequence = 2;
    float const third = WalkTo(policy.Decide(view, 23000), kBot);
    CHECK(HasEvent(events, "profession_node_empty: bot=42 skill=186 node=100"));
    float const fourth = WalkTo(policy.Decide(view, 24000), kBot);
    CHECK(third == 160.0f || fourth == 160.0f);  // the next one, not the empty one
    view.bots[0].pos_x = 158.0f;
    view.bots[0].last_action_type = std::uint8_t(ActionType::kMoveTo);
    view.bots[0].last_action_status = BotActionStatus::kApplied;
    CHECK((GathersOf(policy.Decide(view, 40000)) == std::vector<std::uint32_t>{ 186 }));
    view.bots[0].last_action_type = std::uint8_t(ActionType::kGather);
    view.bots[0].last_action_status = BotActionStatus::kRefused;
    view.bots[0].last_action_sequence = 3;
    policy.Decide(view, 43000);
    CHECK(WalkTo(policy.Decide(view, 44000), kBot) < 0.0f);                                 // 400 is beyond 150 yards: no walk
    // Off: Gathering and Professions both off, no ask at all.
    BotPolicy off(AggressiveOptions());
    WorldTickSnapshotView miner = View(1, 0, {});
    miner.bots[0].profession_skill[0] = 186;
    miner.bots[0].profession_value[0] = 10;
    CHECK(GathersOf(off.Decide(miner, 1000)).empty());
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
    ADeadPetWithoutRevivePetStopsTheTameLoopWhenKeyed();
    LevelDungeonDefaultsGoOnlyAtTheMaxLevel();
    LevelDungeonEarlyLevelsSendsBelowTheMax();
    LevelDungeonSidesKeepAPartyOutOfTheOtherSidesDungeon();
    LevelDungeonNeedRolesWantsATankAndAHealer();
    LevelDungeonFileDurationCapsTheRun();
    LevelDungeonLogNamesWhyADuePartyWaits();
    ProfessionsCraftPlanNamesTheEasiestRecipeAndStopsAtTheCap();
    ProfessionsNodeWalkSkipsEmptyAndFarNodes();
}
