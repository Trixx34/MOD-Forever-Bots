// SPDX-License-Identifier: GPL-2.0-or-later
// Policy tests, part 7 of 8 (split out of policy_test.cpp; the helpers are in policy_test_support.h).
#include "policy_test_support.h"

namespace {

// 0.44.14: the wait for mana counts from the last time the party's mana rose, so a healer that is drinking is waited for, up to kRouteManaAbsMaxMs.
// 0.44.8: a held, sitting mana user logs consume_check (hp, pw, food, drink, what) and drinks. Same party as the healer-drinks test, with supplies on.
void AHeldHealerLogsConsumeCheckAndDrinks()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    options.Supplies.Enabled = true;
    std::vector<std::string> events;
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 }, WarriorLeads);
    for (BotSnapshot& bot : view.bots)
    {
        bot.group_role = bot.guid == 12 ? 2 : 1;
        bot.group_leader_guid = (std::uint64_t(2) << 56) | 12;
        if (bot.guid != 12)
            bot.max_power = 0;
        else
        {
            bot.power_type = 0;
            bot.max_power = 1000;
            bot.power = 50;
            bot.drink_count = 5;
            bot.food_count = 3;
        }
    }
    std::vector<BotAction> const actions = policy.Decide(view, 40000);
    bool drank = false;
    for (BotAction const& action : actions)
        if (action.bot_guid == 12 && action.type == ActionType::kConsume)
            drank = action.spell_id == 2;
    CHECK(drank);
    bool logged = false;
    for (std::string const& line : events)
        if (line.find("consume_check: bot=12 ") != std::string::npos && line.find(" drink=5 ") != std::string::npos && line.find(" what=2") != std::string::npos)
            logged = true;
    CHECK(logged);
}

void TheRouteKeepsWaitingWhileTheHealerDrinks()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads;
    options.Route = { Point{ 100.0f, 0.0f, 0.0f } };
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 }, WarriorLeads);
    for (BotSnapshot& bot : view.bots)
    {
        bot.group_role = bot.guid == 12 ? 2 : 1;
        bot.group_leader_guid = (std::uint64_t(2) << 56) | 12;
        if (bot.guid != 10)
            bot.max_power = 0; // only the healer counts toward the party's mana total (8 % of the other casters' 5000 would hide the rises below)
    }
    auto const setHealerMana = [&view](std::uint32_t power, std::uint32_t maxPower = 1000)
    {
        for (BotSnapshot& bot : view.bots)
            if (bot.guid == 10)
            {
                bot.power_type = 0;
                bot.max_power = maxPower;
                bot.power = power;
            }
    };
    auto const walks = [](std::vector<BotAction> const& actions, std::uint64_t guid)
    {
        for (BotAction const& action : actions)
            if (action.bot_guid == guid && action.type == ActionType::kMoveTo && action.dest_x == 100.0f)
                return true;
        return false;
    };
    setHealerMana(50);
    CHECK(!walks(policy.Decide(view, 1000), 12));
    setHealerMana(200);
    CHECK(!walks(policy.Decide(view, 50000), 12));
    CHECK(!walks(policy.Decide(view, 80000), 12));   // 30 s after the last rise: still waiting (the plain 35 s cap would have ended at 36 s)
    setHealerMana(400);
    CHECK(!walks(policy.Decide(view, 110000), 12));
    // No rise for 35 s: goes on.
    BotPolicy stuck(options);
    stuck.SetRouteActive(true);
    setHealerMana(50);
    CHECK(!walks(stuck.Decide(view, 1000), 12));
    setHealerMana(300);
    CHECK(!walks(stuck.Decide(view, 30000), 12));
    CHECK(!walks(stuck.Decide(view, 60000), 12));
    CHECK(walks(stuck.Decide(view, 95000), 12));      // 65 s without a rise (the cap is 35 s)
    // Rising forever: cut off at kRouteManaAbsMaxMs (180 s).
    BotPolicy rising(options);
    rising.SetRouteActive(true);
    std::uint32_t power = 50;
    bool walked = false;
    std::uint64_t at = 1000;
    for (; at <= 200000 && !walked; at += 20000)
    {
        setHealerMana(power, 10000);
        power += 800; // 8 % of the maximum per step: real recovery
        walked = walks(rising.Decide(view, at), 12);
    }
    CHECK(walked);
    CHECK(at > 170000);
    // Plain regeneration (+1 per tick) is not recovery: the 60 s cap holds.
    BotPolicy regen(options);
    regen.SetRouteActive(true);
    bool regenWalked = false;
    std::uint64_t regenAt = 1000;
    std::uint32_t regenPower = 50;
    for (; regenAt <= 100000 && !regenWalked; regenAt += 5000)
    {
        setHealerMana(regenPower++, 1000);
        regenWalked = walks(regen.Decide(view, regenAt), 12);
    }
    CHECK(regenWalked);
    CHECK(regenAt <= 70000); // gave up at about 61 s, not 180 s
}

// Two druids of one party with one party buff: each member gets it once, however many druids know it.
void TwoDruidsDoNotBothCastThePartyBuff()
{
    std::istringstream file("class 11\nbuff 1126 duration 1800 party\n");
    std::vector<std::string> drop;
    PolicyOptions options = AggressiveOptions();
    options.Rotations = std::make_shared<rotation::RotationData>(rotation::ParseRotationData(file, drop));
    CHECK(drop.empty());
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t) { return 11; };
    BotPolicy policy(options);

    auto const group = [] { return MakeUnitRef(2, 0, 10); }; // the core formed the group, the leader is bot 10
    auto const partied = [&group]
    {
        WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
        for (BotSnapshot& bot : view.bots)
            bot.group_leader_guid = group();
        return view;
    };

    std::size_t onMember11 = 0, total = 0;
    for (std::uint64_t now = 0; now < 12000; now += 3000)
        for (BotAction const& action : policy.Decide(partied(), now))
            if (action.type == ActionType::kCastSpell && action.spell_id == 1126)
            {
                ++total;
                onMember11 += action.target_guid == PlayerOf(11) ? 1 : 0;
            }
    CHECK(total == 5);      // one cast per member
    CHECK(onMember11 == 1); // and not one per druid
}

// An upkeep buff that keeps failing (Mark of the Wild, Thorns, Arcane Intellect and Power Word: Fortitude were refused 30,000 times in two hours) waits twice as long
// after each answer in a row: 5 s, 10 s, 20 s.
void AFailingBuffBacksOffLongerEachTime()
{
    std::istringstream file("class 11\nbuff 1126 duration 1800 party\n");
    std::vector<std::string> drop;
    PolicyOptions options = AggressiveOptions();
    options.Rotations = std::make_shared<rotation::RotationData>(rotation::ParseRotationData(file, drop));
    options.PartySize = 2;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t) { return 11; };
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11 });
    for (BotSnapshot& bot : view.bots)
        bot.group_leader_guid = MakeUnitRef(2, 0, 10);
    auto const casts = [&](std::uint64_t from, std::uint64_t to)
    {
        int count = 0;
        for (std::uint64_t at = from; at < to; at += 500)
            for (BotAction const& action : policy.Decide(view, at))
                count += action.bot_guid == 10 && action.type == ActionType::kCastSpell && action.spell_id == 1126;
        return count;
    };
    CHECK(casts(0, 1000) >= 1);                                        // the first try
    view.bots[0].last_action_type = std::uint8_t(ActionType::kCastSpell);
    view.bots[0].last_action_spell = 1126;
    view.bots[0].last_action_status = BotActionStatus::kInvalidTarget;
    view.bots[0].last_action_sequence = 1;
    CHECK(casts(1000, 5500) == 0);                                     // waits 5 s
    view.bots[0].last_action_sequence = 2;
    CHECK(casts(5500, 14500) == 0);                                    // the second answer in a row: 10 s
}

// A hub walk that timed out is not worth a full idle interval: an unreachable hub (in rock, or in another zone) must not
// cost the bot 30 seconds of standing every time it goes round the list.
void HubTimeoutTriesTheNextHubSoonerThanAnArrival()
{
    BotPolicy policy = HubPolicy();
    WorldTickSnapshotView view = OnMap(1);
    policy.Decide(view, 0);                                   // the idle time starts
    CHECK(WalkTo(AfterMount(policy, view, 30000), 10) == 500.0f); // walking to hub 1
    policy.Decide(view, 31000);                              // unchanged
    CHECK(WalkTo(policy.Decide(view, 210001), 10) < 0.0f);    // dropped after the timeout
    CHECK(WalkTo(AfterMount(policy, view, 215001), 10) == 900.0f); // the next hub, after a short pause
}

// The core says a creature leashed or despawned. It is not worth pulling again at once, so it is avoided like a refused one.
void LeashedTargetIsNotPulledAgain()
{
    NearbyUnit const mob = Hostile(7, 3.0f, 100);
    BotPolicy policy(AggressiveOptions());
    WorldTickSnapshotView fighting = PartyView({ 10 });
    AddNearby(fighting, 0, mob);
    fighting.bots[0].threat_target_guid = mob.guid;
    fighting.bots[0].in_combat = 1;
    CHECK(HasAction(policy.Decide(fighting, 0), 10, ActionType::kAttackTarget, mob.guid));

    // The creature is gone and the core says it leashed: not pulled again for the avoid window.
    WorldTickSnapshotView lost = PartyView({ 10 });
    AddNearby(lost, 0, mob); // still in view, but the bot's threat dropped
    lost.bots[0].in_combat = 0;
    lost.bots[0].last_target_lost_reason = 2; // leashed
    auto const after = policy.Decide(lost, 1000);
    CHECK(!HasAction(after, 10, ActionType::kAttackTarget, mob.guid));
    CHECK(!HasAction(after, 10, ActionType::kFollow, mob.guid));
    // A kill is different: the bot goes straight on to the next creature.
    BotPolicy killer(AggressiveOptions());
    WorldTickSnapshotView killed = PartyView({ 10 });
    killed.bots[0].last_target_lost_reason = 1; // killed
    AddNearby(killed, 0, Hostile(8, 5.0f, 100));
    auto const next = killer.Decide(killed, 1000);
    CHECK(HasAction(next, 10, ActionType::kFollow, MakeUnitRef(8, 1, 8)) ||
          HasAction(next, 10, ActionType::kAttackTarget, MakeUnitRef(8, 1, 8)));
}

// The pet is in the snapshot now, so it can be sent at the target and called back.
void PetIsSentAtTheTargetAndCalledBack()
{
    BotPolicy policy = ClassPolicy(3);
    NearbyUnit const mob = Hostile(2, 20.0f, 100);
    WorldTickSnapshotView fight = HunterView(1, 20, { mob }, MakeUnitRef(8, 1, 2));
    fight.bots[0].pet_state = 1; // out
    auto const first = policy.Decide(fight, 1000);
    CHECK(HasAction(first, kBot, ActionType::kPetCommand, mob.guid)); // sent at this fight's target, once

    WorldTickSnapshotView over = HunterView(2, 20, {}, 0);
    over.bots[0].pet_state = 1;
    auto const after = policy.Decide(over, 5000);
    CHECK(HasAction(after, kBot, ActionType::kPetCommand)); // called back
    bool follows = false;
    for (BotAction const& action : after)
        follows = follows || (action.type == ActionType::kPetCommand && action.spell_id == 2);   // to follow its owner, not to stay where the fight was
    CHECK(follows);
    CHECK(!HasAction(policy.Decide(over, 6000), kBot, ActionType::kPetCommand)); // once, not every tick
}

// A pet the snapshot reports as dead is revived directly: no Call Pet first, and no 30-second backoff.
void DeadPetIsRevivedFromTheSnapshot()
{
    BotPolicy policy = ClassPolicy(3);
    WorldTickSnapshotView view = HunterView(1, 20, {});
    view.bots[0].pet_state = 2; // dead
    auto const actions = policy.Decide(view, 1000);
    CHECK(HasCast(actions, 982) && !HasCast(actions, 883)); // Revive Pet, not Call Pet
}

// A pet that is out and healthy is not touched: no probe, no Mend Pet.
void HealthyPetOutIsLeftAlone()
{
    BotPolicy policy = ClassPolicy(3);
    WorldTickSnapshotView view = HunterView(1, 20, {});
    view.bots[0].pet_state = 1;
    view.bots[0].pet_health = 500;
    view.bots[0].pet_max_health = 500;
    auto const actions = policy.Decide(view, 1000);
    CHECK(!HasCast(actions, 136) && !HasCast(actions, 883) && !HasCast(actions, 982));
}

// A pet out and below the health threshold is healed, from fact rather than from a probe.
void HurtPetIsMendedFromTheSnapshot()
{
    BotPolicy policy = ClassPolicy(3);
    WorldTickSnapshotView view = HunterView(1, 20, {});
    view.bots[0].pet_state = 1;
    view.bots[0].pet_health = 100;
    view.bots[0].pet_max_health = 1000;
    auto const actions = policy.Decide(view, 1000);
    CHECK(HasCast(actions, 136)); // Mend Pet
}

// A buff the bot already carries is read from the snapshot's auras, not from the module's own timer.
void SelfBuffIsReadFromTheAura()
{
    std::istringstream file("class 5\nbuff 17 duration 1800 self\n");
    std::vector<std::string> drop;
    PolicyOptions options = AggressiveOptions();
    options.Rotations = std::make_shared<rotation::RotationData>(rotation::ParseRotationData(file, drop));
    CHECK(drop.empty());
    options.ClassId = 5;
    options.WanderWhenIdle = false;

    WorldTickSnapshotView view = View(1, 0, {});
    view.bots[0].level = 30;
    view.bots[0].aura_count = 1;
    view.bots[0].auras[0].spell_id = 17;      // Power Word: Shield, running
    view.bots[0].auras[0].remaining_ms = 15000;
    auto const actions = BotPolicy(options).Decide(view, 1000);
    CHECK(!HasCast(actions, 17)); // it is up: not cast

    WorldTickSnapshotView gone = View(1, 0, {});
    gone.bots[0].level = 30;
    gone.bots[0].aura_count = 1;
    gone.bots[0].auras[0].spell_id = 17;
    gone.bots[0].auras[0].remaining_ms = 0; // just expired
    CHECK(HasCast(BotPolicy(options).Decide(gone, 1000), 17)); // cast again

    WorldTickSnapshotView none = View(1, 0, {});
    none.bots[0].level = 30;
    CHECK(HasCast(BotPolicy(options).Decide(none, 1000), 17)); // a core that reports no auras: the timer still works
}

// The vendor action carries the NPC's guid now, so buy and repair can be aimed at it.
void VendorCarriesItsGuid()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 100.0f);
    view.bots[0].pos_x = 98.0f; // within reach of the vendor
    view.bots[0].vendor_guid = MakeUnitRef(8, 1234, 77);
    auto const actions = VendorPolicy().Decide(view, 0);
    bool aimed = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kVendor && action.bot_guid == 10)
        {
            aimed = true;
            CHECK(action.target_guid == MakeUnitRef(8, 1234, 77));
        }
    CHECK(aimed);
}

void ShortOfAmmoWalksToTheAmmoVendor()
{
    BotPolicy policy = SupplyPolicy(3);
    auto const actions = policy.Decide(ShopperView(10), 0);
    std::optional<BotAction> walk = FindAction(actions, ActionType::kMoveTo);
    CHECK(walk.has_value() && walk->dest_x == 100.0f); // the vendor that sells ammunition, not the nearer ordinary one
    CHECK(!FindAction(actions, ActionType::kVendor).has_value());
}

void NoSuppliesTripWhenTheFeatureIsOff()
{
    PolicyOptions options = AggressiveOptions();
    options.ClassId = 3;
    options.WanderWhenIdle = false;
    BotPolicy policy(options);
    auto const actions = policy.Decide(ShopperView(0), 0);
    CHECK(!FindAction(actions, ActionType::kMoveTo).has_value() || FindAction(actions, ActionType::kMoveTo)->dest_x != 100.0f);
    CHECK(!FindAction(actions, ActionType::kVendor).has_value());
}

void NoTripWithoutMoneyOrBagRoom()
{
    WorldTickSnapshotView poor = ShopperView(0);
    poor.bots[0].copper = 3;
    CHECK(!FindAction(SupplyPolicy(3).Decide(poor, 0), ActionType::kMoveTo).has_value());
    WorldTickSnapshotView full = ShopperView(0);
    full.bots[0].bag_free_slots = 0;
    CHECK(!FindAction(SupplyPolicy(3).Decide(full, 0), ActionType::kMoveTo).has_value());
}

void BuysAmmoAtTheVendor()
{
    BotPolicy policy = SupplyPolicy(3);
    WorldTickSnapshotView view = ShopperView(10);
    view.bots[0].pos_x = 98.0f; // two yards from the ammunition vendor
    auto const actions = policy.Decide(view, 0);
    std::optional<BotAction> buy = FindAction(actions, ActionType::kVendor);
    CHECK(buy.has_value());
    if (buy)
    {
        CHECK((buy->spell_id & 8) != 0 && (buy->spell_id & 16) == 0 && (buy->spell_id & 32) == 0);
        CHECK(buy->dest_x == 400.0f && buy->dest_y == 0.0f && buy->dest_z == 0.0f);
        CHECK(buy->target_guid == MakeUnitRef(8, 4553, 31));
        CHECK((buy->spell_id & 7) == 7); // sells, repairs and equips on the same visit
    }
}

void AManaUserWithoutFoodOrDrinkBuysBoth()
{
    BotPolicy policy = SupplyPolicy(5);
    WorldTickSnapshotView view = PartyView({ 10 });
    BotSnapshot& bot = view.bots[0];
    bot.level = 12;
    bot.power_type = 0;
    bot.max_power = 100;
    bot.power = 100;
    bot.copper = 200;
    bot.bag_free_slots = 3;
    bot.food_vendor_x = 3.0f;
    bot.food_vendor_valid = 1;
    bot.food_vendor_guid = MakeUnitRef(8, 1465, 12);
    bot.food_count = 0;
    bot.drink_count = 0;
    auto const actions = policy.Decide(view, 0);
    std::optional<BotAction> buy = FindAction(actions, ActionType::kVendor);
    CHECK(buy.has_value());
    if (buy)
    {
        CHECK((buy->spell_id & 16) != 0 && (buy->spell_id & 32) != 0 && (buy->spell_id & 8) == 0);
        CHECK(buy->dest_y == 10.0f && buy->dest_z == 10.0f);
        CHECK(buy->target_guid == MakeUnitRef(8, 1465, 12));
    }
    // A class without mana is not asked to buy drink.
    BotPolicy warrior = SupplyPolicy(1);
    WorldTickSnapshotView plain = view;
    plain.bots[0].max_power = 0;
    std::optional<BotAction> foodOnly = FindAction(warrior.Decide(plain, 0), ActionType::kVendor);
    CHECK(foodOnly.has_value() && foodOnly->dest_y == 10.0f && foodOnly->dest_z == 0.0f && (foodOnly->spell_id & 32) == 0);
}

void AFailedTripBacksOffAndASuccessfulOneDoesNot()
{
    BotPolicy failing = SupplyPolicy(3);
    WorldTickSnapshotView atVendor = ShopperView(10);
    atVendor.bots[0].pos_x = 98.0f;
    CHECK(FindAction(failing.Decide(atVendor, 0), ActionType::kVendor).has_value());
    // The counts did not change by the time the trip is judged: it bought nothing, so the next trip waits the first back-off (10 minutes).
    failing.Decide(atVendor, 8000);
    CHECK(!FindAction(failing.Decide(atVendor, 200000), ActionType::kVendor).has_value());
    CHECK(!FindAction(failing.Decide(atVendor, 400000), ActionType::kVendor).has_value());
    CHECK(FindAction(failing.Decide(atVendor, 700000), ActionType::kVendor).has_value());

    // A trip that bought ammunition: still short (below the low mark), so it goes again after the ordinary trip interval, not the back-off.
    BotPolicy working = SupplyPolicy(3);
    CHECK(FindAction(working.Decide(atVendor, 0), ActionType::kVendor).has_value());
    WorldTickSnapshotView bought = atVendor;
    bought.bots[0].ammo_count = 60;
    working.Decide(bought, 8000);
    CHECK(!FindAction(working.Decide(bought, 100000), ActionType::kVendor).has_value());   // inside the two-minute trip interval
    CHECK(FindAction(working.Decide(bought, 130000), ActionType::kVendor).has_value());
}

void WornGearGoesToTheRepairmanBeforeTheSupplyVendor()
{
    BotPolicy policy = SupplyPolicy(3);
    WorldTickSnapshotView view = ShopperView(10);
    view.bots[0].repair_x = -200.0f;
    view.bots[0].repair_valid = 1;
    view.bots[0].durability_pct = 20;
    std::optional<BotAction> walk = FindAction(AfterMount(policy, view, 0), ActionType::kMoveTo);
    CHECK(walk.has_value() && walk->dest_x == -200.0f);
}

void PartyMembersAndHeldBotsDoNotShop()
{
    PolicyOptions options = AggressiveOptions();
    options.ClassOf = WarriorLeads;
    options.PartySize = 5;
    options.PartyCount = 1;
    options.Supplies.Enabled = true;
    options.WanderWhenIdle = false;
    BotPolicy party(options);
    // Bots 11 and 12: the warrior 12 leads, and the priest 11 follows it. The priest is short of ammunition (as a test would have it) but is
    // a party member, so it does not go shopping; a lone bot with the same facts does (ShortOfAmmoWalksToTheAmmoVendor).
    WorldTickSnapshotView view = PartyView({ 11, 12 });
    BotSnapshot const shopper = ShopperView(0).bots[0];
    for (BotSnapshot& bot : view.bots)
    {
        std::uint64_t const guid = bot.guid;
        bot = shopper;
        bot.guid = guid;
    }
    for (BotAction const& action : party.Decide(view, 0))
        if (action.bot_guid == 11)
            CHECK(!(action.type == ActionType::kMoveTo && action.dest_x == 100.0f) && action.type != ActionType::kVendor);
}

void ARestingHurtBotEatsAndDrinks()
{
    BotPolicy policy = SupplyPolicy();
    WorldTickSnapshotView hurt = PartyView({ 10 });
    hurt.bots[0].health = 400;
    hurt.bots[0].food_count = 5;
    auto const first = policy.Decide(hurt, 1000);
    CHECK(FindAction(first, ActionType::kRest).has_value());
    std::optional<BotAction> eat = FindAction(first, ActionType::kConsume);
    CHECK(eat.has_value() && eat->spell_id == 1);
    // Not again until the last piece has done its work.
    CHECK(!FindAction(policy.Decide(hurt, 10000), ActionType::kConsume).has_value());
    std::optional<BotAction> again = FindAction(policy.Decide(hurt, 30000), ActionType::kConsume);
    CHECK(again.has_value() && again->spell_id == 1);

    // Low mana: drink, which comes before food.
    BotPolicy drinker = SupplyPolicy(5);
    WorldTickSnapshotView dry = ManaView(20);
    dry.bots[0].health = 400;
    dry.bots[0].food_count = 5;
    dry.bots[0].drink_count = 5;
    std::optional<BotAction> drink = FindAction(drinker.Decide(dry, 1000), ActionType::kConsume);
    CHECK(drink.has_value() && drink->spell_id == 2);

    // Nothing carried: nothing to use.
    BotPolicy empty = SupplyPolicy();
    WorldTickSnapshotView bare = PartyView({ 10 });
    bare.bots[0].health = 400;
    CHECK(!FindAction(empty.Decide(bare, 1000), ActionType::kConsume).has_value());
    // A refused use is not repeated at once.
    BotPolicy refused = SupplyPolicy();
    CHECK(FindAction(refused.Decide(hurt, 1000), ActionType::kConsume).has_value());
    WorldTickSnapshotView no = hurt;
    no.bots[0].last_action_sequence = 5;
    no.bots[0].last_action_type = uint8_t(ActionType::kConsume);
    no.bots[0].last_action_status = BotActionStatus::kRefused;
    refused.Decide(no, 2000);
    CHECK(!FindAction(refused.Decide(hurt, 60000), ActionType::kConsume).has_value());
    CHECK(FindAction(refused.Decide(hurt, 130000), ActionType::kConsume).has_value());
}

void AHunterWithoutAmmunitionFightsInMelee()
{
    NearbyUnit const mob = Hostile(2, 20.0f, 100);
    PolicyOptions options = AggressiveOptions();
    options.ClassId = 3;
    options.WanderWhenIdle = false;

    BotPolicy armed(options);
    WorldTickSnapshotView withAmmo = HunterView(1, 1, { mob });
    withAmmo.bots[0].ammo_entry = 2512;
    withAmmo.bots[0].ammo_count = 50;
    CHECK(HasCast(armed.Decide(withAmmo, 1000), 75));

    BotPolicy empty(options);
    WorldTickSnapshotView without = HunterView(1, 1, { mob });
    without.bots[0].ammo_entry = 2512;
    without.bots[0].ammo_count = 0;
    CHECK(!HasCast(empty.Decide(without, 1000), 75)); // the core would refuse it
    // A core that does not report ammunition (ammo_entry 0) is not second-guessed.
    BotPolicy unknown(options);
    WorldTickSnapshotView old = HunterView(1, 1, { mob });
    CHECK(HasCast(unknown.Decide(old, 1000), 75));
}

void AWarriorWithoutPotionsOrBandagesBuysThemAtTheGoodsVendor()
{
    BotPolicy policy = GoodsPolicy();
    std::optional<BotAction> buy = FindAction(policy.Decide(GoodsView(), 0), ActionType::kVendor);
    CHECK(buy.has_value());
    if (buy)
    {
        CHECK((buy->spell_id & 64) != 0 && (buy->spell_id & 128) == 0 && (buy->spell_id & 256) != 0); // no mana, so no mana potion
        CHECK((buy->spell_id & (8 | 16 | 32)) == 0);
        CHECK(buy->target_guid == MakeUnitRef(8, 5100, 41));
    }
    // Off unless asked for.
    BotPolicy off = SupplyPolicy(1);
    CHECK(!FindAction(off.Decide(GoodsView(), 0), ActionType::kVendor).has_value());
}

void AFarGoodsVendorIsWalkedTo()
{
    BotPolicy policy = GoodsPolicy();
    WorldTickSnapshotView far = GoodsView();
    far.bots[0].goods_vendor_x = 80.0f;
    std::optional<BotAction> walk = FindAction(policy.Decide(far, 0), ActionType::kMoveTo);
    CHECK(walk.has_value() && walk->dest_x == 80.0f);
}

void AHurtFightingBotDrinksAHealthPotionOnce()
{
    BotPolicy policy = GoodsPolicy();
    WorldTickSnapshotView fight = GoodsView();
    fight.bots[0].health = 300;      // 30%
    fight.bots[0].health_potion_count = 2;
    fight.bots[0].in_combat = 1;
    std::optional<BotAction> drink = FindAction(policy.Decide(fight, 1000), ActionType::kConsume);
    CHECK(drink.has_value() && drink->spell_id == 3);
    // The game's shared cooldown: not asked for again at once.
    CHECK(!FindAction(policy.Decide(fight, 20000), ActionType::kConsume).has_value());
    std::optional<BotAction> again = FindAction(policy.Decide(fight, 130000), ActionType::kConsume);
    CHECK(again.has_value() && again->spell_id == 3);
    // Healthy enough, or no potion: nothing.
    BotPolicy fine = GoodsPolicy();
    WorldTickSnapshotView well = fight;
    well.bots[0].health = 900;
    CHECK(!FindAction(fine.Decide(well, 1000), ActionType::kConsume).has_value());
    BotPolicy none = GoodsPolicy();
    WorldTickSnapshotView empty = fight;
    empty.bots[0].health_potion_count = 0;
    CHECK(!FindAction(none.Decide(empty, 1000), ActionType::kConsume).has_value());
    // Not outside a fight (a resting bot eats or bandages instead).
    BotPolicy calm = GoodsPolicy();
    WorldTickSnapshotView resting = fight;
    resting.bots[0].in_combat = 0;
    resting.bots[0].food_count = 0;
    std::optional<BotAction> notPotion = FindAction(calm.Decide(resting, 1000), ActionType::kConsume);
    CHECK(!notPotion.has_value() || notPotion->spell_id != 3);
}

void ARestingBotWithoutFoodBandagesItself()
{
    BotPolicy policy = GoodsPolicy();
    WorldTickSnapshotView hurt = GoodsView();
    hurt.bots[0].health = 400;
    hurt.bots[0].food_count = 0;
    hurt.bots[0].bandage_count = 4;
    std::optional<BotAction> bandage = FindAction(policy.Decide(hurt, 1000), ActionType::kConsume);
    CHECK(bandage.has_value() && bandage->spell_id == 5);
    // "Recently Bandaged" lasts a minute: not again before then.
    CHECK(!FindAction(policy.Decide(hurt, 40000), ActionType::kConsume).has_value());
    std::optional<BotAction> again = FindAction(policy.Decide(hurt, 70000), ActionType::kConsume);
    CHECK(again.has_value() && again->spell_id == 5);
    // With food, the food is eaten.
    BotPolicy eater = GoodsPolicy();
    WorldTickSnapshotView fed = hurt;
    fed.bots[0].food_count = 5;
    std::optional<BotAction> eat = FindAction(eater.Decide(fed, 1000), ActionType::kConsume);
    CHECK(eat.has_value() && eat->spell_id == 1);
}

void ABotMissingAReagentBuysItAtTheReagentVendor()
{
    PolicyOptions options = AggressiveOptions();
    options.ClassId = 1;
    options.WanderWhenIdle = false;
    options.Supplies.Enabled = true;
    options.Supplies.Reagents = true;
    BotPolicy policy(options);
    WorldTickSnapshotView view = GoodsView();
    view.bots[0].reagent_entry = 17026;
    view.bots[0].reagent_count = 0;
    view.bots[0].reagent_vendor_valid = 1;
    view.bots[0].reagent_vendor_x = 2.0f;
    view.bots[0].reagent_vendor_guid = MakeUnitRef(8, 5200, 7);
    std::optional<BotAction> buy = FindAction(policy.Decide(view, 0), ActionType::kVendor);
    CHECK(buy.has_value());
    if (buy)
    {
        CHECK((buy->spell_id & 512) != 0 && (buy->spell_id & (8 | 16 | 32 | 64 | 128 | 256)) == 0);
        CHECK(buy->target_guid == MakeUnitRef(8, 5200, 7));
    }
    BotPolicy off = GoodsPolicy();
    std::optional<BotAction> offBuy = FindAction(off.Decide(view, 0), ActionType::kVendor);   // one Decide: a second call changes the policy's state
    CHECK(!offBuy.has_value() || (offBuy->spell_id & 512) == 0);
}

void AManaUserDrinksAManaPotionInAFight()
{
    BotPolicy policy = GoodsPolicy(5);
    WorldTickSnapshotView fight = ManaView(10);
    fight.bots[0].in_combat = 1;
    fight.bots[0].mana_potion_count = 1;
    std::optional<BotAction> drink = FindAction(policy.Decide(fight, 1000), ActionType::kConsume);
    CHECK(drink.has_value() && drink->spell_id == 4);
}

// ---- Presence (bot_presence.h) ----
void ABotIsSafeToLogOutOnlyWhenNothingIsGoingOn()
{
    BotPolicy policy = SupplyPolicy(1);
    WorldTickSnapshotView view = GoodsView();
    policy.Decide(view, 0);
    CHECK(policy.SafeToLogOut(view.bots[0]));
    BotSnapshot fighting = view.bots[0];
    fighting.in_combat = 1;
    CHECK(!policy.SafeToLogOut(fighting));
    BotSnapshot dead = view.bots[0];
    dead.life_state = 2;
    CHECK(!policy.SafeToLogOut(dead));
    BotSnapshot grouped = view.bots[0];
    grouped.group_role = 1;
    CHECK(!policy.SafeToLogOut(grouped));
    BotSnapshot invited = view.bots[0];
    invited.group_invite_from = MakeUnitRef(4, 77, 1);
    CHECK(!policy.SafeToLogOut(invited));
    // On the way to a vendor for supplies: not now.
    WorldTickSnapshotView shopper = ShopperView(10);
    BotPolicy walking = SupplyPolicy(3);
    CHECK(FindAction(walking.Decide(shopper, 0), ActionType::kMoveTo).has_value());
    CHECK(!walking.SafeToLogOut(shopper.bots[0]));
    // A bot the policy never saw has nothing going on.
    BotSnapshot stranger = view.bots[0];
    stranger.guid = 999;
    CHECK(policy.SafeToLogOut(stranger));
}

void ALongWalkGoesToTheFlightMasterThenFlies()
{
    BotPolicy policy = TaxiPolicy();
    WorldTickSnapshotView far = TaxiView(0.0f);
    policy.Decide(far, 0);
    auto const first = policy.Decide(far, 30000);
    CHECK(WalkTo(first, 10) == 100.0f);                           // to the flight master, not to the hub
    CHECK(!FirstOf(first, ActionType::kTaxi).has_value());

    BotPolicy flier = TaxiPolicy();
    WorldTickSnapshotView there = TaxiView(98.0f);                // 2 yards from the flight master
    flier.Decide(there, 0);
    auto const fly = flier.Decide(there, 30000);
    std::optional<BotAction> order = FirstOf(fly, ActionType::kTaxi);
    CHECK(order.has_value());
    if (order)
    {
        CHECK(order->target_guid == MakeUnitRef(8, 3838, 5) && order->spell_id == 1 && order->dest_x == 3000.0f);
    }
    CHECK(WalkTo(fly, 10) < 0.0f);                                // no walking order with it
    // The order is not repeated while it is given time to show, and a bot on the flight gets no orders.
    CHECK(!FirstOf(flier.Decide(there, 31000), ActionType::kTaxi).has_value());
    WorldTickSnapshotView flying = there;
    flying.bots[0].in_flight = 1;
    auto const during = flier.Decide(flying, 60000);
    CHECK(!FirstOf(during, ActionType::kTaxi).has_value() && WalkTo(during, 10) < 0.0f);
}

void ARefusedFlightMakesTheBotWalk()
{
    BotPolicy policy = TaxiPolicy();
    WorldTickSnapshotView there = TaxiView(98.0f);
    policy.Decide(there, 0);
    CHECK(FirstOf(policy.Decide(there, 30000), ActionType::kTaxi).has_value());
    WorldTickSnapshotView refused = there;
    refused.bots[0].last_action_sequence = 7;
    refused.bots[0].last_action_type = uint8_t(ActionType::kTaxi);
    refused.bots[0].last_action_status = BotActionStatus::kRefused;
    auto const after = AfterMount(policy, refused, 50000);
    CHECK(!FirstOf(after, ActionType::kTaxi).has_value());
    CHECK(WalkTo(after, 10) == 3000.0f);                          // the walk goes on, to the hub
}

void FlightsAreOffByDefaultAndNotForPartyMembers()
{
    PolicyOptions options = AggressiveOptions();
    options.HubsByMap[1] = { Point{ 3000.0f, 0.0f, 0.0f } };
    BotPolicy off(options);
    WorldTickSnapshotView there = TaxiView(98.0f);
    off.Decide(there, 0);
    CHECK(!FirstOf(off.Decide(there, 30000), ActionType::kTaxi).has_value());
}

void ABotWaitingForAFlightFidgetsWhenIdle()
{
    BotPolicy policy = FidgetPolicy(true);
    WorldTickSnapshotView there = TaxiView(98.0f);
    policy.Decide(there, 0);
    CHECK(FirstOf(policy.Decide(there, 30000), ActionType::kTaxi).has_value());
    CHECK(FidgetsInWait(policy, there, 30500, 33900) == 0);      // not before the take-off had its time
    std::size_t const n = FidgetsInWait(policy, there, 34000, 37500);
    CHECK(n >= 1);
    // The steps stay within two yards of where the bot stands.
    BotPolicy again = FidgetPolicy(true);
    again.Decide(there, 0);
    again.Decide(there, 30000);
    for (std::uint64_t t = 34000; t <= 37500; t += 500)
        for (BotAction const& action : again.Decide(there, t))
            if (action.bot_guid == 10 && action.type == ActionType::kMoveTo)
                CHECK(std::hypot(action.dest_x - 98.0f, action.dest_y) <= 2.0f);
}

void NoFidgetByDefaultInCombatOrWithAnEnemyNear()
{
    {
        BotPolicy policy = FidgetPolicy(false);
        WorldTickSnapshotView there = TaxiView(98.0f);
        policy.Decide(there, 0);
        policy.Decide(there, 30000);
        CHECK(FidgetsInWait(policy, there, 34000, 37500) == 0);
    }
    {
        BotPolicy policy = FidgetPolicy(true);
        WorldTickSnapshotView there = TaxiView(98.0f);
        policy.Decide(there, 0);
        policy.Decide(there, 30000);
        WorldTickSnapshotView fight = there;
        fight.bots[0].in_combat = 1;
        CHECK(FidgetsInWait(policy, fight, 34000, 37500) == 0);
    }
    {
        BotPolicy policy = FidgetPolicy(true);
        WorldTickSnapshotView there = TaxiView(98.0f);
        policy.Decide(there, 0);
        policy.Decide(there, 30000);
        WorldTickSnapshotView near = there;
        near.bots[0].nearby_count = 1;
        near.nearby[0] = Unit(8, 77, 20.0f, true);               // a hostile creature in view
        near.nearby[0].level = 1;
        CHECK(FidgetsInWait(policy, near, 34000, 37500) == 0);
    }
}

void ABotBuffsAPassingPlayerOutOfCombat()
{
    BotPolicy policy(BuffOptions());
    CHECK(PasserBuffsOver(policy, PasserView(5000), 0, 30000) == 1);   // once: the cooldown holds the next one
}

void NoPassingBuffByDefaultInCombatBelowTheManaThresholdOrOnCooldown()
{
    PolicyOptions off = BuffOptions();
    off.PassingBuff.Enabled = false;
    BotPolicy offPolicy(off);
    CHECK(PasserBuffsOver(offPolicy, PasserView(5000), 0, 30000) == 0);

    BotPolicy fighting(BuffOptions());
    WorldTickSnapshotView combat = PasserView(5000);
    combat.bots[0].in_combat = 1;
    CHECK(PasserBuffsOver(fighting, combat, 0, 30000) == 0);

    BotPolicy low(BuffOptions());
    CHECK(PasserBuffsOver(low, PasserView(3500), 0, 30000) == 0);       // 70 percent, under 80

    // Enough percent, but the rotation's reserve (Wrath 2000 x 1 cast) plus the buff (200) does not fit.
    PolicyOptions tight = BuffOptions();
    tight.PassingBuff.ManaAbovePct = 0;
    BotPolicy reserve(tight);
    CHECK(PasserBuffsOver(reserve, PasserView(2100), 0, 30000) == 0);
    BotPolicy enough(tight);
    CHECK(PasserBuffsOver(enough, PasserView(2300), 0, 3000) == 1);

    // The cooldown: after the first buff no second until it ends.
    BotPolicy policy(BuffOptions());
    CHECK(PasserBuffsOver(policy, PasserView(5000), 0, 9000) == 1);
    CHECK(PasserBuffsOver(policy, PasserView(5000), 12000, 300000) == 0);
    CHECK(PasserBuffsOver(policy, PasserView(5000), 603000, 615000) == 1);

    // A hostile creature in view: no buff.
    BotPolicy hostile(BuffOptions());
    WorldTickSnapshotView threat = PasserView(5000);
    threat.bots[0].nearby_count = 2;
    threat.nearby[1] = Unit(8, 5, 25.0f, true);
    CHECK(PasserBuffsOver(hostile, threat, 0, 30000) == 0);
}

void ABotAtAHubStaysForTheDowntimeThenLeaves()
{
    BotPolicy policy(AfkOptions(5));
    WorldTickSnapshotView view = OnMap(1);
    policy.Decide(view, 0);
    CHECK(WalkTo(AfterMount(policy, view, 30000), 10) == 500.0f);    // walking to hub 1
    view.bots[0].pos_x = 495.0f;                                     // arrived
    CHECK(WalkTo(policy.Decide(view, 31000), 10) < 0.0f);
    CHECK(WalkTo(policy.Decide(view, 61000), 10) < 0.0f);            // the usual interval has passed ...
    CHECK(WalkTo(policy.Decide(view, 31000 + 299000), 10) < 0.0f);   // ... but the downtime (5 min) has not
    CHECK(WalkTo(AfterMount(policy, view, 31000 + 301000), 10) == 900.0f); // then it mounts and walks on to the next hub
}

void TheNumberOfIdleBotsPerZoneIsCapped()
{
    BotPolicy policy(AfkOptions(1));
    WorldTickSnapshotView view = PartyView({ 10, 11 });
    for (BotSnapshot& bot : view.bots)
        bot.map_id = 1;
    policy.Decide(view, 0);
    policy.Decide(view, 30000);
    policy.Decide(view, 30001);
    for (BotSnapshot& bot : view.bots)
        bot.pos_x = 495.0f;
    policy.Decide(view, 31000);
    auto const later = policy.Decide(view, 62000);
    CHECK((WalkTo(later, 10) == 900.0f) != (WalkTo(later, 11) == 900.0f));   // one walks on, the other stays: one idle bot per map
}

void NoTownAfkByDefault()
{
    PolicyOptions options = AfkOptions(5);
    options.TownAfk.Enabled = false;
    BotPolicy policy(options);
    WorldTickSnapshotView view = OnMap(1);
    policy.Decide(view, 0);
    CHECK(WalkTo(AfterMount(policy, view, 30000), 10) == 500.0f);
    view.bots[0].pos_x = 495.0f;
    policy.Decide(view, 31000);
    CHECK(WalkTo(policy.Decide(view, 61000), 10) == 900.0f);
}

void RenewKeepsUpOnTheFightingTankEveryFifteenSeconds()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = LeveledDungeonView(40);
    view.bots[2].in_combat = 1;                          // the tank (bot 12) fights, at full health
    view.bots[2].threat_target_guid = MakeUnitRef(8, 1, 7);
    CHECK(CastsSpell(policy.Decide(view, 1000), 10, 139, PlayerOf(12)));
    CHECK(!CastsSpell(policy.Decide(view, 8000), 10, 139, PlayerOf(12)));      // it still runs
    CHECK(CastsSpell(policy.Decide(view, 17000), 10, 139, PlayerOf(12)));      // run out: again
    // Not before level 8, and not while the tank is out of the fight.
    BotPolicy low = DungeonPolicy();
    WorldTickSnapshotView young = LeveledDungeonView(6);
    young.bots[2].in_combat = 1;
    CHECK(!CastsSpell(low.Decide(young, 1000), 10, 139, PlayerOf(12)));
    BotPolicy calm = DungeonPolicy();
    CHECK(!CastsSpell(calm.Decide(LeveledDungeonView(40), 1000), 10, 139, PlayerOf(12)));
}

// HealWaitTankHold: no Renew on a full-health tank whose target is on someone else (ZG 2026-11-13: tank_loose reason=healer 5 s after the pull).
void RenewWaitsUntilTheTankHoldsItsTarget()
{
    auto const policyWith = [](bool wait)
    {
        PolicyOptions options = AggressiveOptions();
        options.PartySize = 5;
        options.PartyCount = 1;
        options.ClassOf = WarriorLeads;
        options.HealWaitTankHold = wait;
        BotPolicy policy(options);
        policy.SetRouteActive(true);
        return policy;
    };
    std::uint64_t const mob = MakeUnitRef(8, 1, 7);
    auto const viewWith = [mob](std::uint64_t victim, std::uint64_t top, bool inView)
    {
        WorldTickSnapshotView view = LeveledDungeonView(40);
        view.bots[2].in_combat = 1;                      // the tank (bot 12) fights, at full health
        view.bots[2].threat_target_guid = mob;
        if (inView)
        {
            NearbyUnit unit = Hostile(7, 20, 100);
            unit.guid = mob;
            unit.victim_ref = victim;
            unit.threat_top_ref = top;
            AddNearby(view, 0, unit);                    // in the priest's (bot 10) view
        }
        return view;
    };
    BotPolicy onPriest = policyWith(true);
    CHECK(!CastsSpell(onPriest.Decide(viewWith(PlayerOf(10), 0, true), 1000), 10, 139, PlayerOf(12)));   // the mob hits the priest: no Renew
    BotPolicy held = policyWith(true);
    CHECK(CastsSpell(held.Decide(viewWith(PlayerOf(12), 0, true), 1000), 10, 139, PlayerOf(12)));        // the mob hits the tank: Renew
    BotPolicy topTank = policyWith(true);
    CHECK(CastsSpell(topTank.Decide(viewWith(PlayerOf(10), PlayerOf(12), true), 1000), 10, 139, PlayerOf(12)));   // threat top wins over the victim
    BotPolicy topOther = policyWith(true);
    CHECK(!CastsSpell(topOther.Decide(viewWith(PlayerOf(12), PlayerOf(11), true), 1000), 10, 139, PlayerOf(12)));
    BotPolicy unseen = policyWith(true);
    CHECK(CastsSpell(unseen.Decide(viewWith(0, 0, false), 1000), 10, 139, PlayerOf(12)));                 // not in view: as before
    BotPolicy off = policyWith(false);
    CHECK(CastsSpell(off.Decide(viewWith(PlayerOf(10), 0, true), 1000), 10, 139, PlayerOf(12)));         // switch off: as before
}

void BindingHealWhenThePriestAndTheTankAreBothHurt()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = LeveledDungeonView(40);
    view.bots[0].health = 500;                           // the priest at 50%
    view.bots[2].health = 500;                           // the tank at 50%, fighting
    view.bots[2].in_combat = 1;
    view.bots[2].threat_target_guid = MakeUnitRef(8, 1, 7);
    CHECK(CastsSpell(policy.Decide(view, 1000), 10, 1240770, PlayerOf(12)));
    // The priest unhurt: a plain heal of the tank. Below the spell's level: a plain heal as well.
    BotPolicy fine = DungeonPolicy();
    WorldTickSnapshotView healthy = LeveledDungeonView(40);
    healthy.bots[2].health = 500;
    healthy.bots[2].in_combat = 1;
    healthy.bots[2].threat_target_guid = MakeUnitRef(8, 1, 7);
    CHECK(!CastsSpell(fine.Decide(healthy, 1000), 10, 1240770, PlayerOf(12)) && CastsSpell(DungeonPolicy().Decide(healthy, 1000), 10, 2054, PlayerOf(12)));
    BotPolicy young = DungeonPolicy();
    WorldTickSnapshotView below = LeveledDungeonView(30);
    below.bots[0].health = 500;
    below.bots[2].health = 500;
    below.bots[2].in_combat = 1;
    below.bots[2].threat_target_guid = MakeUnitRef(8, 1, 7);
    CHECK(!CastsSpell(young.Decide(below, 1000), 10, 1240770, PlayerOf(12)));
}

void PrayerOfHealingOnlyWhenMostOfThePartyIsLow()
{
    BotPolicy policy = DungeonPolicy();
    WorldTickSnapshotView view = LeveledDungeonView(40);
    view.bots[1].health = 400;                           // three members under half
    view.bots[3].health = 400;
    view.bots[4].health = 400;
    CHECK(CastsSpell(policy.Decide(view, 1000), 10, 596));
    BotPolicy two = DungeonPolicy();
    WorldTickSnapshotView fewer = LeveledDungeonView(40);
    fewer.bots[1].health = 400;
    fewer.bots[3].health = 400;
    CHECK(!CastsSpell(two.Decide(fewer, 1000), 10, 596));
    BotPolicy young = DungeonPolicy();
    WorldTickSnapshotView below = LeveledDungeonView(28);
    below.bots[1].health = 400;
    below.bots[3].health = 400;
    below.bots[4].health = 400;
    CHECK(!CastsSpell(young.Decide(below, 1000), 10, 596));
}

} // namespace

void RunPolicyTestsPart7()
{
    RenewKeepsUpOnTheFightingTankEveryFifteenSeconds();
    RenewWaitsUntilTheTankHoldsItsTarget();
    BindingHealWhenThePriestAndTheTankAreBothHurt();
    PrayerOfHealingOnlyWhenMostOfThePartyIsLow();
    ShortOfAmmoWalksToTheAmmoVendor();
    ALongWalkGoesToTheFlightMasterThenFlies();
    ARefusedFlightMakesTheBotWalk();
    ABotWaitingForAFlightFidgetsWhenIdle();
    NoFidgetByDefaultInCombatOrWithAnEnemyNear();
    ABotBuffsAPassingPlayerOutOfCombat();
    NoPassingBuffByDefaultInCombatBelowTheManaThresholdOrOnCooldown();
    ABotAtAHubStaysForTheDowntimeThenLeaves();
    TheNumberOfIdleBotsPerZoneIsCapped();
    NoTownAfkByDefault();
    FlightsAreOffByDefaultAndNotForPartyMembers();
    ABotIsSafeToLogOutOnlyWhenNothingIsGoingOn();
    AWarriorWithoutPotionsOrBandagesBuysThemAtTheGoodsVendor();
    AFarGoodsVendorIsWalkedTo();
    AHurtFightingBotDrinksAHealthPotionOnce();
    ARestingBotWithoutFoodBandagesItself();
    AManaUserDrinksAManaPotionInAFight();
    ABotMissingAReagentBuysItAtTheReagentVendor();
    NoSuppliesTripWhenTheFeatureIsOff();
    NoTripWithoutMoneyOrBagRoom();
    BuysAmmoAtTheVendor();
    AManaUserWithoutFoodOrDrinkBuysBoth();
    AFailedTripBacksOffAndASuccessfulOneDoesNot();
    WornGearGoesToTheRepairmanBeforeTheSupplyVendor();
    PartyMembersAndHeldBotsDoNotShop();
    ARestingHurtBotEatsAndDrinks();
    AHunterWithoutAmmunitionFightsInMelee();
    LeashedTargetIsNotPulledAgain();
    PetIsSentAtTheTargetAndCalledBack();
    DeadPetIsRevivedFromTheSnapshot();
    HealthyPetOutIsLeftAlone();
    HurtPetIsMendedFromTheSnapshot();
    SelfBuffIsReadFromTheAura();
    VendorCarriesItsGuid();
    TheRouteKeepsWaitingWhileTheHealerDrinks();
    AHeldHealerLogsConsumeCheckAndDrinks();
    TwoDruidsDoNotBothCastThePartyBuff();
    AFailingBuffBacksOffLongerEachTime();
    HubTimeoutTriesTheNextHubSoonerThanAnArrival();
}
