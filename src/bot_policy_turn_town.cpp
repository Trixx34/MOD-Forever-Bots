// SPDX-License-Identifier: GPL-2.0-or-later
// BotPolicy::Decide, a bot's turn: vendors and supplies, wandering, rest, training, parties, loot, flights, mounts and the move it sends (the steps DecideBot runs in order; bot_policy.cpp).
#include "bot_policy_internal.h"

void BotPolicy::TurnShopping(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& lowHealth = turn.lowHealth;
    auto const& partyMember = turn.partyMember;
    auto& hasTarget = turn.hasTarget;
    auto& wanted = turn.wanted;
    auto& moveWhy = turn.moveWhy;
    auto& idle = turn.idle;

    // Services and travel (M6). An idle bot due a vendor visit walks to the nearest vendor on its map, unless it is a
    // party member (it follows its leader). The visit is sent once the bot is in reach. A walk longer than
    // kTravelTimeoutMs is dropped and the visit waits for its next interval. A walking bot is not idle, so it neither
    // sits nor trains on the way.
    bool const urgentService = state.ServiceUrgent && !hasTarget && bot.in_combat == 0 && !lowHealth && !state.InFight;

    // Supplies (layout 15, supply_policy.h). A trip made earlier is judged once the core has counted again: more pieces than before is a
    // success; otherwise the vendor had nothing the bot could buy (or the bot never reached it) and the next trip waits longer.
    turn.supplyFacts = FactsOf(bot);
    auto const& supplyFacts = turn.supplyFacts;
    if (state.SupplyCheckAtMs != 0 && nowMs >= state.SupplyCheckAtMs)
    {
        state.SupplyCheckAtMs = 0;
        bool const improved = supply::Improved(state.SupplyAsked, state.SupplyBefore, supplyFacts);
        state.SupplyFailures = improved ? 0 : state.SupplyFailures + 1;
        if (!improved)
            state.NextSupplyMs = nowMs + supply::BackoffMs(_options.Supplies, state.SupplyFailures);
        if (_options.OnEvent)
            _options.OnEvent(std::string(improved ? "supply_trip_done" : "supply_trip_failed") + ": bot=" + std::to_string(bot.guid) +
                             " ammo=" + std::to_string(supplyFacts.AmmoCount) + " food=" + std::to_string(supplyFacts.FoodCount) +
                             " drink=" + std::to_string(supplyFacts.DrinkCount) + " hpot=" + std::to_string(supplyFacts.HealthPotions) +
                             " mpot=" + std::to_string(supplyFacts.ManaPotions) + " bandage=" + std::to_string(supplyFacts.Bandages) +
                             " failures=" + std::to_string(state.SupplyFailures));
    }
    // A party member follows its leader, and a bot held by a chat order stays where it was told: neither goes shopping.
    supply::Needs const supplyNeeds = (partyMember || state.ChatStay || state.ChatFollow != 0) ? supply::Needs{}
                                                                                                : supply::Assess(_options.Supplies, supplyFacts);
    supply::Stop const supplyStop = supply::NextStop(supplyNeeds);
    bool const supplyDue = supplyStop != supply::Stop::None && nowMs >= state.NextSupplyMs && state.SupplyCheckAtMs == 0;
    bool const serviceDue = _options.ServiceEveryMs != 0 && nowMs >= state.NextServiceMs;
    bool const shopping = (idle || urgentService) && !state.ChatStay && (serviceDue || supplyDue);
    if (!shopping && state.Traveling)
        state.Traveling = false; // a walk to a vendor cut short (a fight, an order) starts its timer again next time

    // Services and travel (M6). An idle bot due a vendor visit walks to the nearest vendor on its map, unless it is a
    // party member (it follows its leader). The visit is sent once the bot is in reach. A walk longer than
    // kTravelTimeoutMs is dropped and the visit waits for its next interval. A walking bot is not idle, so it neither
    // sits nor trains on the way. A bot short of ammunition, food or drink walks to a vendor that sells it instead (supplies).
    if (shopping)
    {
        // The nearest vendor on the bot's map, found by the core (layout 6); the nearest repairman instead when the gear is worn
        // (layout 9). None on the map, or a party member: no walk.
        bool const needRepair = bot.repair_valid != 0 && bot.durability_pct < kRepairBelowPct;
        // Worn gear first: the repairman is the stop then, and the supplies wait for the next trip (a few minutes).
        bool const supplyStopNow = supplyDue && !needRepair;
        bool const ammoStop = supplyStopNow && supplyStop == supply::Stop::Ammo;
        bool const goodsStop = supplyStopNow && supplyStop == supply::Stop::Goods;
        bool const reagentStop = supplyStopNow && supplyStop == supply::Stop::Reagent;
        Point const vendor = supplyStopNow ? (ammoStop      ? Point{ bot.ammo_vendor_x, bot.ammo_vendor_y, bot.ammo_vendor_z }
                                              : goodsStop   ? Point{ bot.goods_vendor_x, bot.goods_vendor_y, bot.goods_vendor_z }
                                              : reagentStop ? Point{ bot.reagent_vendor_x, bot.reagent_vendor_y, bot.reagent_vendor_z }
                                                            : Point{ bot.food_vendor_x, bot.food_vendor_y, bot.food_vendor_z })
                             : needRepair  ? Point{ bot.repair_x, bot.repair_y, bot.repair_z }
                                           : Point{ bot.vendor_x, bot.vendor_y, bot.vendor_z };
        bool const vendorExists = supplyStopNow || needRepair || bot.vendor_valid != 0;
        bool const hasVendor = !partyMember && vendorExists;
        bool const inReach = vendorExists && DistanceTo(bot, vendor) <= kVendorReachYards;
        if (hasVendor && !inReach)
        {
            if (!state.Traveling)
            {
                state.Traveling = true;
                state.TravelStartMs = nowMs;
                state.SupplyTrip = supplyStopNow;
            }
            if (nowMs - state.TravelStartMs <= kTravelTimeoutMs)
            {
                wanted = Decision{ ActionType::kMoveTo, 0, vendor };
                moveWhy = "vendor";
                idle = false;
            }
            else
            {
                state.Traveling = false;
                state.ServiceUrgent = false;
                if (state.SupplyTrip)
                {
                    // The walk never got there: a trip that bought nothing.
                    state.SupplyTrip = false;
                    state.NextSupplyMs = nowMs + supply::BackoffMs(_options.Supplies, ++state.SupplyFailures);
                }
                else
                    state.NextServiceMs = nowMs + _options.ServiceEveryMs;
            }
        }
        else if (inReach || !vendorExists)
        {
            // In reach of a vendor, or none on the map at all: the visit is tried where the bot stands and the core refuses it when
            // no NPC is in range. What is left is a party member with a vendor it may not walk to, and it is sent nothing: it follows
            // its leader, and an action it will have refused still spent a slot of the tick's action budget (kRequestedActions is 2 a
            // bot, and PublishActions drops the whole tick when the list runs over) and pushed the next visit a whole ServiceEveryMs
            // out. At the 1500-bot ceiling that is 1200 refused actions on one tick, over the budget the core granted.
            state.Traveling = false;
            state.SupplyTrip = false;
            if (state.ServiceUrgent)
            {
                state.ServiceUrgent = false;
                _quests.NoteServiced(state.Quest); // the bags are emptier: the refused hand-ins are tried again
            }
            BotAction vendor;
            vendor.bot_guid = bot.guid;
            vendor.type = ActionType::kVendor;
            vendor.spell_id = kServiceSell | kServiceRepair | kServiceEquip;
            // Nearly full bags (113 of 300 soak bots had every slot taken by cloth, meat and spare bags and could not turn in a quest): the visit also
            // sells the white junk, not only the grey items.
            if (bot.bag_free_slots <= kBagsLowSlots)
                vendor.spell_id |= kServiceSellJunk;
            // Layout 13: the NPC's guid, not just its position. An older core leaves it 0 and the core resolves the
            // nearest vendor of the map instead.
            vendor.target_guid = needRepair ? bot.repair_guid : bot.vendor_guid;
            if (supplyStopNow)
            {
                // Layout 15: buy what is short. The core picks the items from this vendor's stock and buys up to the counts given here.
                supply::Request const request = supply::RequestFor(_options.Supplies, supplyNeeds);
                vendor.target_guid = ammoStop ? bot.ammo_vendor_guid : goodsStop ? bot.goods_vendor_guid
                                   : reagentStop ? bot.reagent_vendor_guid : bot.food_vendor_guid;
                if (request.Ammo != 0)
                    vendor.spell_id |= kServiceBuyAmmo;
                if (request.Food != 0)
                    vendor.spell_id |= kServiceBuyFood;
                if (request.Drink != 0)
                    vendor.spell_id |= kServiceBuyDrink;
                if (request.HealthPotion)
                    vendor.spell_id |= kServiceBuyHealthPotion;
                if (request.ManaPotion)
                    vendor.spell_id |= kServiceBuyManaPotion;
                if (request.Bandage)
                    vendor.spell_id |= kServiceBuyBandage;
                if (request.Reagent)
                    vendor.spell_id |= kServiceBuyReagent;
                vendor.dest_x = float(request.Ammo);
                vendor.dest_y = float(request.Food);
                vendor.dest_z = float(request.Drink);
                state.NextSupplyMs = nowMs + _options.Supplies.TripEveryMs;
                state.SupplyCheckAtMs = nowMs + kSupplyJudgeAfterMs;
                state.SupplyAsked = supplyNeeds;
                state.SupplyBefore = supplyFacts;
                if (_options.OnEvent)
                    _options.OnEvent("supply_trip: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " ammo=" +
                                     std::to_string(supplyFacts.AmmoCount) + " food=" + std::to_string(supplyFacts.FoodCount) + " drink=" +
                                     std::to_string(supplyFacts.DrinkCount) + " hpot=" + std::to_string(supplyFacts.HealthPotions) + " mpot=" +
                                     std::to_string(supplyFacts.ManaPotions) + " bandage=" + std::to_string(supplyFacts.Bandages) +
                                     " copper=" + std::to_string(bot.copper));
            }
            actions.push_back(vendor);
            if (!supplyStopNow || serviceDue)
                state.NextServiceMs = nowMs + _options.ServiceEveryMs;
        }
    }
}

void BotPolicy::TurnWander(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& stagingHold = turn.stagingHold;
    auto const& partyMember = turn.partyMember;
    auto& wanted = turn.wanted;
    auto& moveWhy = turn.moveWhy;
    auto& idle = turn.idle;

    // Town AFK (TownAfk, off by default) ends when the stay is over or the bot is no longer idle (a fight, an order): it frees its place on the map.
    if (state.AfkUntilMs != 0 && (!idle || nowMs >= state.AfkUntilMs || bot.in_combat != 0))
    {
        state.AfkUntilMs = 0;
        _afkZones.End(bot.guid);
    }

    // Hubs (M7): a bot that has been idle for kWanderIdleMs walks to the next hub of its map, and waits there until the
    // next walk. A hurt bot rests instead, and a party member follows its leader. Anything that comes near stops the walk.
    // Mana (the soak's hunter spent most of its time at a third of its mana, because only health made a bot sit): an idle mana user
    // below kRestManaBelowPct sits and drinks until kRestManaUntilPct, at most kManaRestMaxMs at a time (and not again for a while
    // when that was not enough: mana that does not come back is not waited for). Anything else the bot does ends the rest.
    turn.manaUser = bot.power_type == 0 && bot.max_power > 0;
    auto const& manaUser = turn.manaUser;
    turn.manaResting = false;
    auto& manaResting = turn.manaResting;
    if (idle && manaUser)
    {
        if (state.ManaRestSinceMs == 0 && nowMs >= state.ManaRestBlockedUntilMs &&
            std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * kRestManaBelowPct)
            state.ManaRestSinceMs = nowMs;
        if (state.ManaRestSinceMs != 0)
        {
            if (std::uint64_t(bot.power) * 100 >= std::uint64_t(bot.max_power) * kRestManaUntilPct)
                state.ManaRestSinceMs = 0;
            else if (nowMs - state.ManaRestSinceMs > kManaRestMaxMs)
            {
                state.ManaRestSinceMs = 0;
                state.ManaRestBlockedUntilMs = nowMs + kManaRestBlockMs;
            }
            else
                manaResting = true;
        }
    }
    else
        state.ManaRestSinceMs = 0;
    turn.healthy = !manaResting && (bot.max_health == 0 || std::uint64_t(bot.health) * 100 >= std::uint64_t(bot.max_health) * state.RestPct);
    auto const& healthy = turn.healthy;
    auto const hubs = _options.HubsByMap.find(bot.map_id);
    turn.grindMode = _options.Grind && !_options.Grind->Spots.empty() && bot.level != 0;
    auto const& grindMode = turn.grindMode;
    // Walking to the anvil, forge or fire a recipe needs (SDK 0.18, after kNeedsFocus): nothing in view and healthy, the bot goes there; arrived, it asks the core again; a walk
    // with no route, or too long, is over and the skill waits half an hour.
    // Walking to a gathering node (GatherWalk, after the core answered that nothing is in reach): healthy and idle, the bot goes there; within reach of it, the
    // next kGather asks for the node. A walk with no route, or too long, is over and that line waits a minute and a half.
    if (state.GatherWalking && _options.GatherNodes && state.GatherWalkIndex < _options.GatherNodes->size() && idle && healthy)
    {
        gather::NodePoint const& node = (*_options.GatherNodes)[state.GatherWalkIndex];
        bool const arrived = node.Map == bot.map_id && std::hypot(node.X - bot.pos_x, node.Y - bot.pos_y) <= kGatherReachYards;
        bool const blocked = state.WalkBlockedAtMs != 0 && state.WalkBlockedAtMs >= state.GatherWalkStartMs;
        if (arrived || blocked || nowMs - state.GatherWalkStartMs > kGatherWalkTimeoutMs)
        {
            state.GatherWalking = false;
            state.GatherRetryAtMs[state.GatherWalkLine] = arrived ? nowMs : nowMs + kGatherNothingMs;
        }
        else
        {
            wanted = Decision{ ActionType::kMoveTo, 0, Point{ node.X, node.Y, node.Z } };
            moveWhy = "gather_walk";
            idle = false;
        }
    }
    if (state.CraftWalking && _options.Foci && state.CraftWalkIndex < _options.Foci->size() && idle && healthy)
    {
        crafting::FocusPoint const& point = (*_options.Foci)[state.CraftWalkIndex];
        bool const arrived = crafting::InReach(point, bot.map_id, bot.pos_x, bot.pos_y);
        bool const blocked = state.WalkBlockedAtMs != 0 && state.WalkBlockedAtMs >= state.CraftWalkStartMs;
        if (arrived || blocked || nowMs - state.CraftWalkStartMs > kCraftWalkTimeoutMs)
        {
            state.CraftWalking = false;
            state.CraftRetryAtMs[state.CraftWalkLine] = arrived ? nowMs : nowMs + kCraftWalkFailMs;
        }
        else
        {
            wanted = Decision{ ActionType::kMoveTo, 0, Point{ point.X, point.Y, point.Z } };
            moveWhy = "point_walk";
            idle = false;
        }
    }
    else if (idle && healthy && _options.WanderWhenIdle && grindMode && nowMs >= state.GrindRetryAtMs && !partyMember && !stagingHold && !state.ChatStay && state.ChatFollow == 0)
    {
        // Leveling: after a quiet spell the bot walks to a grind spot that suits its level (nearest first, not the ones it just left), and
        // stays to fight what lives there until it sees nothing for a while again.
        if (state.Wandering)
        {
            bool const arrived = DistanceTo(bot, state.GrindTarget) <= kHubReachYards;
            bool const timedOut = nowMs - state.WanderStartMs > kWanderTimeoutMs || (state.WalkBlockedAtMs != 0 && state.WalkBlockedAtMs >= state.WanderStartMs); // no route: the walk ends, not re-sent every tick
            if (arrived || timedOut)
            {
                state.Wandering = false;
                state.IdleSinceMs = nowMs;
            }
            else
            {
                wanted = Decision{ ActionType::kMoveTo, 0, state.GrindTarget };
                moveWhy = "state.GrindTarget";
                idle = false;
            }
        }
        else if (nowMs - state.IdleSinceMs >= WanderIdleFor(turn))
        {
            std::vector<std::size_t> const spots = GrindSpotsForBot(state, bot, nowMs);
            if (!spots.empty())
            {
                // Not always the nearest spot: with randomness on the bot takes one of the nearest few, and stands somewhere around it, not on the
                // spot's exact point, so the bots of a level do not pile up on one place.
                std::size_t pick = 0;
                if (_options.RandomPct > 0 && spots.size() > 1)
                    pick = bot_random::Below(bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltGrindPick, state.WanderCycles),
                                             std::uint32_t(std::min<std::size_t>(spots.size(), kGrindPickAmong)));
                GrindSpot const& spot = _options.Grind->Spots[spots[pick]];
                state.GrindTarget = Point{ spot.X, spot.Y, spot.Z };
                state.GrindSpotIndex = spots[pick];
                if (_options.RandomPct > 0)
                {
                    double const angle = bot_random::Unit(bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltJitter, std::uint64_t(state.WanderCycles) * 2)) * 6.283185307;
                    double const radius = kStandJitterYards * std::sqrt(bot_random::Unit(bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltJitter, std::uint64_t(state.WanderCycles) * 2 + 1)));
                    state.GrindTarget.X += float(std::cos(angle) * radius);
                    state.GrindTarget.Y += float(std::sin(angle) * radius);
                }
                ++state.WanderCycles;
                state.WanderIdleMs = 0; // the next walk draws its own idle time
                state.GrindVisited.push_back(spots[pick]);
                if (state.GrindVisited.size() > kGrindMemory)
                    state.GrindVisited.erase(state.GrindVisited.begin());
                state.Wandering = true;
                state.WanderStartMs = nowMs;
                wanted = Decision{ ActionType::kMoveTo, 0, state.GrindTarget };
                moveWhy = "state.GrindTarget";
                idle = false;
                if (_options.OnEvent)
                    _options.OnEvent("grind_spot: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) +
                                     " spot=" + std::to_string(spots[pick]) + " levels=" + std::to_string(spot.MinLevel) + "-" + std::to_string(spot.MaxLevel));
            }
            else
            {
                // Nothing on this map for this level (a start zone the spot file does not cover, such as the new races' island): the bot
                // explores on foot for a minute, as a bot without grind data does, and then looks again. Without this it stood at its
                // start for the whole run.
                state.IdleSinceMs = nowMs;
                state.GrindRetryAtMs = nowMs + kGrindRetryMs;
            }
        }
    }
    else if (idle && healthy && _options.WanderWhenIdle && !partyMember && !state.ChatStay && state.ChatFollow == 0 && hubs != _options.HubsByMap.end() && !hubs->second.empty() &&
             state.HubFailures < kHubFailuresMax)
    {
        std::vector<Point> const& list = hubs->second;
        if (state.Wandering)
        {
            Point const& hub = list[state.NextHub % list.size()];
            bool const arrived = DistanceTo(bot, hub) <= kHubReachYards;
            bool const timedOut = nowMs - state.WanderStartMs > kWanderTimeoutMs || (state.WalkBlockedAtMs != 0 && state.WalkBlockedAtMs >= state.WanderStartMs); // no route: the walk ends, not re-sent every tick
            if (arrived || timedOut)
            {
                // Next time, the next hub. An arrival waits a full idle interval, as before. A timeout does not: a hub in rock or in
                // another zone must not cost the bot 30 s of standing every time it goes round the list, so it walks on after a short
                // pause. Walks that end without arriving are counted: after kHubFailuresMax in a row the bot explores on foot instead.
                state.Wandering = false;
                state.HubFailures = arrived ? 0 : state.HubFailures + 1;
                state.NextHub = (state.NextHub + 1) % list.size();
                state.IdleSinceMs = arrived ? nowMs : nowMs - (kWanderIdleMs - kHubRetryIdleMs);
                // Town AFK: now and then a bot that reached a hub stays for 5 to 20 minutes (it stands; no new order is sent), unless the map already has its quota of
                // staying bots. The stay only holds back the next walk; any fight or order ends it.
                if (arrived && _options.TownAfk.Enabled && bot.in_combat == 0 && !turn.enemyNear &&
                    realism::AfkRoll(_options.TownAfk, _options.RandomSeed, bot.guid, state.AfkCounter))
                {
                    std::uint64_t const stay = realism::AfkDurationMs(_options.TownAfk, _options.RandomSeed, bot.guid, state.AfkCounter);
                    if (_afkZones.TryStart(bot.map_id, bot.guid, nowMs, nowMs + stay, _options.TownAfk.ZoneMax))
                    {
                        state.AfkUntilMs = nowMs + stay;
                        if (_options.OnEvent)
                            _options.OnEvent("town_afk: bot=" + std::to_string(bot.guid) + " map=" + std::to_string(bot.map_id) + " minutes=" + std::to_string(stay / 60000));
                    }
                }
                ++state.AfkCounter;
            }
            else
            {
                wanted = Decision{ ActionType::kMoveTo, 0, hub };
                moveWhy = "hub";
                idle = false;
            }
        }
        else if (nowMs - state.IdleSinceMs >= WanderIdleFor(turn) && nowMs >= state.AfkUntilMs)
        {
            ++state.WanderCycles;
            state.WanderIdleMs = 0; // the next walk draws its own idle time
            state.Wandering = true;
            state.WanderStartMs = nowMs;
            wanted = Decision{ ActionType::kMoveTo, 0, list[state.NextHub % list.size()] };
            moveWhy = "list[state.NextHub % list.size()]";
            idle = false;
        }
    }

    // Exploring on foot: a bot with no grind spot and no hub it can reach (a map with neither, or hubs it never got to) would stand where it
    // began and never meet a creature, as 45 bots did for eight hours in the soak. After a short quiet it takes a walk of 50 to 100 yards in a
    // turning direction, and after a few walks tries its hubs again.
    else if (idle && healthy && _options.WanderWhenIdle && (!grindMode || nowMs < state.GrindRetryAtMs) && !partyMember && !state.ChatStay && state.ChatFollow == 0)
    {
        // Starts a walk to a new random point; scale shortens it (a retry after a refused point). The point is at the bot's own height, so
        // the core often has no route to it (open ground the navmesh does not cover): a refused point is retried nearer, not after the idle pause.
        auto const startExploreWalk = [&](float scale) {
            float const angle = float((bot.guid * 2654435761ull) % 628) * 0.01f + float(state.ExploreStep) * 2.399963f;
            float radius = (50.0f + 25.0f * float(state.ExploreStep % 3)) * scale;
            if (_options.RandomPct > 0)
                radius = float(bot_random::Vary(std::uint64_t(radius), _options.RandomPct,
                                                bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltJitter, 0x200000000ull + state.ExploreStep)));
            ++state.ExploreStep;
            state.ExploreTarget = Point{ bot.pos_x + std::cos(angle) * radius, bot.pos_y + std::sin(angle) * radius, bot.pos_z };
            state.Wandering = true;
            state.WanderStartMs = nowMs;
            wanted = Decision{ ActionType::kMoveTo, 0, state.ExploreTarget };
            moveWhy = "state.ExploreTarget";
            idle = false;
        };
        if (state.Wandering)
        {
            bool const arrived = DistanceTo(bot, state.ExploreTarget) <= kExploreReachYards;
            bool const refused = !arrived && state.WalkBlockedAtMs != 0 && state.WalkBlockedAtMs >= state.WanderStartMs;
            bool const timedOut = nowMs - state.WanderStartMs > kExploreTimeoutMs || refused;
            if (refused && state.ExploreRefusals < kExploreRetryRefusals)
            {
                ++state.ExploreRefusals;
                state.ExploreScale *= 0.5f;
                startExploreWalk(state.ExploreScale);
            }
            else if (arrived || timedOut)
            {
                state.Wandering = false;
                state.IdleSinceMs = nowMs; // the next walk starts after kExploreIdleMs
                state.ExploreRefusals = 0;
                state.ExploreScale = 1.0f;
                if (state.ExploreStep % 8 == 0)
                    state.HubFailures = 0; // try the hubs again now and then
            }
            else
            {
                wanted = Decision{ ActionType::kMoveTo, 0, state.ExploreTarget };
                moveWhy = "state.ExploreTarget";
                idle = false;
            }
        }
        else if (nowMs - state.IdleSinceMs >= bot_random::Vary(kExploreIdleMs, std::min<std::uint32_t>(_options.RandomPct * 2, 90),
                                                               bot_random::Draw(_options.RandomSeed, bot.guid, bot_random::kSaltIdle, 0x100000000ull + state.ExploreStep)))
        {
            state.ExploreRefusals = 0;
            state.ExploreScale = 1.0f;
            startExploreWalk(1.0f);
        }
    }
}

void BotPolicy::TurnRestAndConsume(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& nearby = turn.nearby;
    auto const& partyMember = turn.partyMember;
    auto& questStep = turn.questStep;
    auto& routeBusy = turn.routeBusy;
    auto& dungeonHold = turn.dungeonHold;
    auto& hasTarget = turn.hasTarget;
    auto& wanted = turn.wanted;
    auto& enemyNear = turn.enemyNear;
    auto& idle = turn.idle;
    auto const& supplyFacts = turn.supplyFacts;
    auto const& manaUser = turn.manaUser;
    auto& manaResting = turn.manaResting;
    auto const& healthy = turn.healthy;
    auto const& grindMode = turn.grindMode;

    // A bot that stood on the same spot for a whole check interval, not fighting, says why in one event line (stuck_idle). The 73 Undead bots
    // of the soak did this for eight hours and nothing in the logs said what held them; this line shows which condition did.
    if (_options.OnEvent && nowMs - state.DiagAtMs >= kStuckCheckMs)
    {
        float const moved = std::hypot(bot.pos_x - state.DiagX, bot.pos_y - state.DiagY);
        if (state.DiagAtMs != 0 && moved < 5.0f && bot.in_combat == 0 && !state.Sitting)
        {
            std::size_t hostiles = 0;
            for (NearbyUnit const& unit : nearby)
                if (IsLiveMob(unit))
                    ++hostiles;
            _options.OnEvent("stuck_idle: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " map=" + std::to_string(bot.map_id) +
                             " idle=" + std::to_string(idle) + " hasTarget=" + std::to_string(hasTarget) + " enemyNear=" + std::to_string(enemyNear) +
                             " wanted=" + std::to_string(int(wanted.Type)) + " quest=" + std::to_string(int(questStep.Kind)) + " route=" + std::to_string(routeBusy) +
                             " healthy=" + std::to_string(healthy) + " mana=" + std::to_string(manaResting) + " grind=" + std::to_string(grindMode) +
                             " wandering=" + std::to_string(state.Wandering) + " traveling=" + std::to_string(state.Traveling) + " party=" + std::to_string(partyMember) +
                             " stay=" + std::to_string(state.ChatStay) + " liveHostiles=" + std::to_string(hostiles) + " nearby=" + std::to_string(nearby.size()) +
                             " idleForS=" + std::to_string(state.Idling ? (nowMs - state.IdleSinceMs) / 1000 : 0) +
                             " pos=" + std::to_string(int(bot.pos_x)) + "," + std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)) +
                             " in_combat=" + std::to_string(bot.in_combat) + " route_index=" + std::to_string(state.RouteIndex) +
                             " tank_lead=" + std::to_string(state.TankLead) + " event_active=" + std::to_string(state.Events && state.Events->Active()));
        }
        state.DiagAtMs = nowMs;
        state.DiagX = bot.pos_x;
        state.DiagY = bot.pos_y;
    }

    // Rest: sit down while idle and hurt, and stand before anything moves or fights. The stand goes out before the
    // movement below, in the same action set, so the bot never moves sitting.
    // Idle means no fight and no player to follow, with no live hostile anywhere near: a sitting bot is easy prey.
    // Creatures only: a hostile player that is not attacking the bot does not stop it from resting (its threat target
    // already does, through hasTarget).
    bool const hurtEnough = bot.max_health > 0 && std::uint64_t(bot.health) * 100 < std::uint64_t(bot.max_health) * state.RestPct;
    bool const stillHurt = bot.max_health > 0 && bot.health < bot.max_health;
    // Waiting for the party (dungeon): sits to recover health, and mana for a mana user, even with creatures in view.
    bool const holdIdle = dungeonHold && !hasTarget && wanted.Type == ActionType::kStop && !routeBusy;
    // Dungeon drink: any mana user in a dungeon party drinks at kDungeonDrinkBelowPct and drinks up to kDungeonDrinkUpToPct.
    // This overrides the normal rest share so the pull does not go out with anyone OOM. The owner's rule of 2026-10-23:
    // a pull should not start with the healer (or any mana user) OOM.
    bool const dungeonDrink = holdIdle && manaUser && IsRouteActive(bot.guid) &&
                              std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * kDungeonDrinkBelowPct;
    bool const manaLow = holdIdle && manaUser &&
                         std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * (state.Sitting ? kPartyReadySitHealthPct : (_options.TacticRest ? kPartyReadyPowerPct : kPartyReadyPowerOldPct));
    // The Rest tactic: the party waits for every member to reach the pull share (90 %), so a member below it sits to get there (it only sat below its own rest share).
    bool const healthLow = holdIdle && _options.TacticRest && bot.max_health > 0 &&
                           std::uint64_t(bot.health) * 100 < std::uint64_t(bot.max_health) * kPartyReadyHealthPct;
    // A dungeon mana user drinks up to the dungeon drink cap; others use the normal manaLow threshold.
    bool const drinkCap = dungeonDrink ? kDungeonDrinkUpToPct : (state.Sitting ? kPartyReadySitHealthPct : (_options.TacticRest ? kPartyReadyPowerPct : kPartyReadyPowerOldPct));
    bool const manaStillLow = state.Sitting && dungeonDrink &&
                              std::uint64_t(bot.power) * 100 < std::uint64_t(bot.max_power) * drinkCap;
    bool const sitWanted = (idle || holdIdle) && (state.Sitting ? (stillHurt || manaLow || manaStillLow || manaResting) : (hurtEnough || healthLow || manaLow || manaResting)) &&
                           nowMs >= state.RestRetryAtMs;
    if (sitWanted != state.Sitting)
    {
        BotAction rest;
        rest.bot_guid = bot.guid;
        rest.type = ActionType::kRest;
        rest.spell_id = sitWanted ? 1 : 0;
        actions.push_back(rest);
        state.Sitting = sitWanted;
    }

    // Eat or drink while resting (layout 15): a bot sat down because it is hurt or low on mana uses food or drink it carries, which restores
    // far faster than sitting alone. Not while moving or fighting, and not again until the last piece has done its work.
    if (_options.Supplies.Enabled && state.Sitting && sitWanted && bot.moving == 0 && bot.in_combat == 0 && nowMs >= state.NextConsumeMs)
    {
        std::uint32_t const healthPct = bot.max_health > 0 ? std::uint32_t(std::uint64_t(bot.health) * 100 / bot.max_health) : 100;
        std::uint32_t const manaPct = manaUser && bot.max_power > 0 ? std::uint32_t(std::uint64_t(bot.power) * 100 / bot.max_power) : 100;
        std::uint32_t what = supply::ConsumeNow(_options.Supplies, supplyFacts, healthPct, manaPct);
        if (what == 0)
            what = supply::BandageNow(_options.Supplies, supplyFacts, healthPct); // layout 16: no food to eat, so a bandage
        if (what != 0)
        {
            BotAction eat;
            eat.bot_guid = bot.guid;
            eat.type = ActionType::kConsume;
            eat.spell_id = what;
            actions.push_back(eat);
            state.NextConsumeMs = nowMs + (what == 5 ? _options.Supplies.BandageEveryMs : _options.Supplies.ConsumeEveryMs);
        }
    }

    // Potions (layout 16): a bot in a fight that is badly hurt, or a mana user nearly out of mana, drinks one. It costs no global cooldown and
    // no standing still, so it goes alongside whatever the bot does; the game's shared potion cooldown is why it is not asked for sooner.
    if (_options.Supplies.Enabled && _options.Supplies.Potions && bot.in_combat != 0 && nowMs >= state.NextPotionMs)
    {
        std::uint32_t const healthPct = bot.max_health > 0 ? std::uint32_t(std::uint64_t(bot.health) * 100 / bot.max_health) : 100;
        std::uint32_t const manaPct = manaUser && bot.max_power > 0 ? std::uint32_t(std::uint64_t(bot.power) * 100 / bot.max_power) : 100;
        if (std::uint32_t const which = supply::PotionNow(_options.Supplies, supplyFacts, healthPct, manaPct))
        {
            BotAction potion;
            potion.bot_guid = bot.guid;
            potion.type = ActionType::kConsume;
            potion.spell_id = which;
            actions.push_back(potion);
            state.NextPotionMs = nowMs + _options.Supplies.PotionEveryMs;
        }
    }
}

void BotPolicy::TurnTrainAndParty(DecideTick& tick, BotTurn& turn)
{
    WorldTickSnapshotView const& view = tick.view;
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    auto const& indexOf = tick.indexOf;
    auto& membersOf = tick.membersOf;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& partyLeader = turn.partyLeader;
    auto const& partyLeaderBot = turn.partyLeaderBot;
    auto const& partyMember = turn.partyMember;
    auto const& leaderRef = turn.leaderRef;
    auto& idle = turn.idle;

    bool const levelUp = bot.level != 0 && bot.level != state.LastTrainLevel;
    // A sitting bot (resting, or drinking) is not available for a trainer check, exactly as a walking bot is not: it stands first.
    if (idle && !state.Sitting && _options.TrainEveryMs != 0 && (levelUp || nowMs >= state.NextTrainMs) &&
        realism::ReadReady(_options.ReadDelay, state.TrainRead, kTrainReadKey, _options.RandomSeed, bot.guid, nowMs))
    {
        realism::Close(state.TrainRead);
        BotAction train;
        train.bot_guid = bot.guid;
        train.type = ActionType::kTrain;
        actions.push_back(train);
        state.LastTrainLevel = bot.level;
        state.NextTrainMs = nowMs + _options.TrainEveryMs;
    }

    // A player's party invite is accepted (AcceptInvites, from the chat command switch): the player then commands the bot in chat. An invite
    // from one of this module's bots is not a player's: the party code above answers those (a second accept here was refused as "no pending
    // invite", and a leader even tried to accept the party it leads).
    // With WhisperReplies a busy bot (dead, in a fight, in a dungeon, travelling) does not accept: it whispers the inviter why (once per
    // player and bot, the limiter), and the invite times out in the core.
    if (_options.AcceptInvites && bot.group_invite_from != 0 && !tick.IsOurBot(bot.group_invite_from) && nowMs >= state.InviteAcceptAtMs &&
        forever_module::UnitRefType(bot.group_invite_from) == kPlayerType)
    {
        BusyKind const busy = _options.WhisperReplies ? BusyOf(bot) : BusyKind::None;
        if (busy != BusyKind::None)
        {
            std::uint64_t const player = bot.group_invite_from & forever_module::kUnitRefCounterMask;
            if (_whispers.Allow(player, bot.guid, nowMs))
                _whisperReplies.push_back({ bot.guid, player, ClampReply(BusyLine(bot.guid, busy)) });
            state.InviteAcceptAtMs = nowMs + kAcceptInviteRetryMs;
        }
        else if (bot.in_combat == 0)
        {
            BotAction accept;
            accept.bot_guid = bot.guid;
            accept.type = ActionType::kGroup;
            accept.spell_id = kGroupAccept;
            actions.push_back(accept);
            state.InviteAcceptAtMs = nowMs + kAcceptInviteRetryMs;
        }
    }

    // Party actions (M8), out of combat. The leader invites each member that is not in its party yet, and invites a
    // member again only after kInviteRetryMs. A member accepts its leader's invite, and leaves a party that is not its
    // leader's. Each is sent once; a refused one is retried after kPartyRetryMs.
    if (_options.PartySize != 0 && bot.in_combat == 0 && nowMs >= state.PartyRetryAtMs)
    {
        if (partyLeaderBot)
        {
            // A leader that sits in another bot's party (old parties come back from the saved game), or holds an invite to one, leaves it first:
            // a member cannot invite, and a pending invite on it would be refused for the others.
            bool const foreign = (bot.group_role != 0 && bot.group_leader_guid != leaderRef && tick.IsOurBot(bot.group_leader_guid)) ||
                                 (bot.group_role == 0 && bot.group_invite_from != leaderRef && tick.IsOurBot(bot.group_invite_from));
            // (a leader that has just invited holds a pending invite to its own party: that one is not foreign, leaving would drop the party)
            if (foreign)
            {
                BotAction leave;
                leave.bot_guid = bot.guid;
                leave.type = ActionType::kGroup;
                leave.spell_id = kGroupLeave;
                actions.push_back(leave);
                state.PartyRetryAtMs = nowMs + kPartyRetryMs;
            }
            std::vector<std::uint64_t> const none;
            for (std::uint64_t member : foreign ? none : membersOf[bot.guid])
            {
                auto const at = indexOf.find(member);
                if (at == indexOf.end())
                    continue; // not online
                BotSnapshot const& other = view.bots[at->second];
                if (other.group_leader_guid == leaderRef || other.group_invite_from == leaderRef)
                    continue; // in this party already, or invited and waiting for the answer
                if ((other.group_role != 0 && other.group_leader_guid != leaderRef) ||
                    (other.group_role == 0 && other.group_invite_from != 0 && other.group_invite_from != leaderRef))
                    continue; // in another party, or invited to one: it leaves or declines on its own turn, then it is invited
                auto const invited = state.InvitedAtMs.find(member);
                if (invited != state.InvitedAtMs.end() && nowMs < invited->second + kInviteRetryMs)
                    continue;

                BotAction invite;
                invite.bot_guid = bot.guid;
                invite.type = ActionType::kGroup;
                invite.spell_id = kGroupInvite;
                invite.target_guid = PlayerRef(member);
                actions.push_back(invite);
                state.InvitedAtMs[member] = nowMs;
            }
            // 40-man raid support (doc 108): this leader's roster is a raid (PolicyOptions::Raid). ConvertToRaid works at
            // any party size (Group.cpp), so it is asked for as soon as this bot leads one, well before the 6th member
            // would otherwise be refused by the 5-member party cap; repeated at kRaidConvertRetryMs since the core gives
            // no snapshot signal of success and the ask is a harmless no-op once already a raid. Each confirmed member
            // (group_leader_guid already this leader's) is then put in the subgroup its position in the roster implies --
            // 5 per subgroup, leader first -- so an 8x5 roster (ScenarioRoster order) lands one tank/healer/dps group per
            // subgroup, the same split a 5-man scenario's roster gives a single party.
            if (_options.Raid)
            {
                if (!foreign && nowMs >= state.RaidConvertRetryAtMs)
                {
                    BotAction convert;
                    convert.bot_guid = bot.guid;
                    convert.type = ActionType::kGroup;
                    convert.spell_id = kGroupConvertRaid;
                    actions.push_back(convert);
                    state.RaidConvertRetryAtMs = nowMs + kRaidConvertRetryMs;
                }
                std::vector<std::uint64_t> const& roster = membersOf[bot.guid];
                for (std::size_t m = 0; m < roster.size(); ++m)
                {
                    std::uint64_t const member = roster[m];
                    auto const at = indexOf.find(member);
                    if (at == indexOf.end())
                        continue; // not online
                    BotSnapshot const& other = view.bots[at->second];
                    if (other.group_leader_guid != leaderRef)
                        continue; // not a confirmed member of this raid yet; assigned once it has joined
                    // The leader itself holds subgroup 0's first slot (not counted in `roster`), so a member's 1-based
                    // position (m + 1) cut by kRaidSubgroupSize gives its subgroup.
                    std::uint8_t const subgroup = static_cast<std::uint8_t>((m + 1) / kRaidSubgroupSize);
                    auto const sentAt = state.RaidAssignRetryAtMs.find(member);
                    auto const sentWhat = state.RaidSubgroupSent.find(member);
                    bool const alreadySent = sentWhat != state.RaidSubgroupSent.end() && sentWhat->second == subgroup &&
                                              sentAt != state.RaidAssignRetryAtMs.end() && nowMs < sentAt->second;
                    if (alreadySent)
                        continue;
                    BotAction assign;
                    assign.bot_guid = bot.guid;
                    assign.type = ActionType::kGroup;
                    assign.spell_id = kGroupAssignSubgroup;
                    assign.target_guid = PlayerRef(member);
                    assign.dest_x = float(subgroup);
                    actions.push_back(assign);
                    state.RaidSubgroupSent[member] = subgroup;
                    state.RaidAssignRetryAtMs[member] = nowMs + kRaidAssignRetryMs;
                }
            }
        }
        else if (partyMember)
        {
            BotAction party;
            party.bot_guid = bot.guid;
            party.type = ActionType::kGroup;
            if (bot.group_role == 0 && bot.group_invite_from == leaderRef)
                party.spell_id = kGroupAccept;
            else if (bot.group_role == 0 && tick.IsOurBot(bot.group_invite_from))
                party.spell_id = kGroupLeave; // declines an invite from another bot leader (an invite from a human is left to the player's own accept)
            else if (bot.group_role != 0 && bot.group_leader_guid != leaderRef)
                party.spell_id = kGroupLeave;
            if (party.spell_id != 0)
            {
                actions.push_back(party);
                state.PartyRetryAtMs = nowMs + kPartyRetryMs;
            }
        }
        else if (partyLeader == 0 && bot.group_role != 0 && tick.IsOurBot(bot.group_leader_guid) && !(_options.IsAlt && _options.IsAlt(bot.guid)))
        {
            // Not one of the bots chosen for the parties (PartyPercent), but still in a bot party left from an earlier run: it leaves, and plays on
            // its own. A bot in a human's party, and a player's alt, are not touched.
            BotAction leave;
            leave.bot_guid = bot.guid;
            leave.type = ActionType::kGroup;
            leave.spell_id = kGroupLeave;
            actions.push_back(leave);
            state.PartyRetryAtMs = nowMs + kPartyRetryMs;
        }
    }
}

void BotPolicy::TurnLootTaxiMount(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto& nearby = turn.nearby;
    auto const& lowHealth = turn.lowHealth;
    auto const& partyLeaderBot = turn.partyLeaderBot;
    auto const& partyMember = turn.partyMember;
    PolicyOptions::ScenarioRoute const& scenario = *turn.scenario;
    auto const& returningToDungeon = turn.returningToDungeon;
    auto& wanted = turn.wanted;
    auto& moveWhy = turn.moveWhy;

    // Loot (M7): a creature the bot fought, now dead and close, once the bot is out of combat. One loot per corpse; the core
    // decides whether the bot may take anything from it.
    if (bot.in_combat == 0 && !lowHealth)
    {
        for (NearbyUnit const& unit : nearby)
        {
            if (unit.health != 0 || forever_module::UnitRefType(unit.guid) != 8 || state.LootedUntilMs.count(unit.guid))
                continue;
            if (!state.FoughtAtMs.count(unit.guid) || rotation::DistanceYards3d(bot, unit) > kLootRangeYards)
                continue;
            // A groupmate already took this corpse (the core loots all of it for the group): nothing is left for this bot.
            std::uint64_t const lootGroup = bot.group_leader_guid != 0 ? bot.group_leader_guid : bot.guid;
            if (auto const claim = _lootClaims.find(unit.guid); claim != _lootClaims.end() && nowMs < claim->second.UntilMs)
            {
                if (claim->second.Group == lootGroup)
                {
                    state.FoughtAtMs.erase(unit.guid);
                    continue;
                }
            }
            else if (_lootClaims.size() > 512)
                for (auto it = _lootClaims.begin(); it != _lootClaims.end();)
                    it = nowMs >= it->second.UntilMs ? _lootClaims.erase(it) : std::next(it);
            _lootClaims[unit.guid] = LootClaim{ lootGroup, nowMs + kLootWindowMs };

            BotAction loot;
            loot.bot_guid = bot.guid;
            loot.type = ActionType::kLoot;
            loot.target_guid = unit.guid;
            actions.push_back(loot);
            state.LootedUntilMs[unit.guid] = nowMs + kLootWindowMs;
            state.FoughtAtMs.erase(unit.guid);
            break; // one corpse per tick
        }
    }

    if (IsResurrectSpell(bot.casting_spell) && bot.cast_remaining_ms != 0 && state.Last)
        wanted = *state.Last; // a resurrect cast is not broken by a new movement order

    // Flight points (layout 17): a bot on a flight takes no walking orders; a long walk of a bot that is on its own is replaced by a walk to the nearest
    // flight master and a flight from it (taxi_policy.h). The core refuses a flight that would not bring the bot closer, and the bot then walks.
    bool fidgetWait = false;
    if (_options.Taxi.Enabled && wanted.Type == ActionType::kMoveTo && !partyMember && !partyLeaderBot)
    {
        taxi::Facts facts;
        facts.X = bot.pos_x;
        facts.Y = bot.pos_y;
        facts.InFlight = bot.in_flight != 0;
        facts.InCombat = bot.in_combat != 0;
        facts.Alive = bot.life_state == 0;
        facts.MasterValid = bot.taxi_master_valid != 0;
        facts.MasterX = bot.taxi_master_x;
        facts.MasterY = bot.taxi_master_y;
        switch (taxi::Decide(_options.Taxi, facts, wanted.Dest.X, wanted.Dest.Y, nowMs, state.TaxiBlockedUntilMs, state.TaxiWaitUntilMs))
        {
            case taxi::Move::Walk:
                break;
            case taxi::Move::WalkToMaster:
                wanted.Dest = Point{ bot.taxi_master_x, bot.taxi_master_y, bot.taxi_master_z };
                break;
            case taxi::Move::Fly:
            {
                BotAction fly;
                fly.bot_guid = bot.guid;
                fly.type = ActionType::kTaxi;
                fly.target_guid = bot.taxi_master_guid;
                fly.spell_id = bot.map_id;
                fly.dest_x = wanted.Dest.X;
                fly.dest_y = wanted.Dest.Y;
                fly.dest_z = wanted.Dest.Z;
                actions.push_back(fly);
                state.TaxiWaitUntilMs = nowMs + _options.Taxi.WaitMs;
                state.FidgetState = realism::FidgetState{};
                state.TaxiFlight = taxi::Flight{ nowMs, 0, wanted.Dest.X, wanted.Dest.Y, bot.taxi_master_guid };
                if (_options.TaxiPlaces)
                    state.TaxiFlight.Place = taxi::PlaceName(*_options.TaxiPlaces, bot.map_id, wanted.Dest.X, wanted.Dest.Y);
                if (_options.OnEvent)
                    _options.OnEvent(taxi::OrderLine(bot.guid, bot.level, bot.map_id, int(std::hypot(bot.pos_x - wanted.Dest.X, bot.pos_y - wanted.Dest.Y)),
                                                     state.TaxiFlight));
                [[fallthrough]];
            }
            case taxi::Move::Wait:
                // Idle fidgeting (IdleFidget, off by default): decided at the end of this step, once the mount step has had its say.
                if (_options.Fidget.Enabled && state.TaxiFlight.OrderedMs != 0 && nowMs >= state.TaxiFlight.OrderedMs + _options.Fidget.AfterOrderMs &&
                    bot.in_flight == 0)
                    fidgetWait = true;
                if (state.Last)
                    wanted = *state.Last; // nothing new is sent while the flight starts or runs
                else
                    wanted = Decision{};
                break;
        }
    }
    else if (bot.in_flight != 0 && state.Last)
        wanted = *state.Last; // on a flight with nothing to walk to: the stop order would be a new order too

    // Back to the dungeon: whatever the rest of the policy wanted, a bot of a dungeon party outside the instance walks to the instance
    // door — the ghost's route, and the destination the scenario's rescue teleport uses (which the core refuses in combat, so the walk
    // is also what makes the rescue possible).
    if (returningToDungeon)
    {
        wanted = Decision{ ActionType::kMoveTo, 0, scenario.PortalPoint };
        moveWhy = "return_to_instance";
    }

    // Mount for long travel, dismount when the walk is short, combat starts, or the bot arrives.
    if (bot.in_combat == 0 && bot.life_state == 0 && bot.casting_spell == 0 && nowMs >= state.MountRetryMs)
    {
        if (wanted.Type == ActionType::kMoveTo && !state.Mounted && bot.level >= _options.MountMinLevel)
        {
            float const destDist = DistanceTo(bot, wanted.Dest);
            if (destDist > kMountMinDistanceYards)
            {
                // Stop first and mount last: the core keeps only the last action's answer, and with the stop after the mount its refusal was never seen.
                BotAction stop;
                stop.bot_guid = bot.guid;
                stop.type = ActionType::kStop;
                actions.push_back(stop);
                BotAction mount;
                mount.bot_guid = bot.guid;
                mount.type = ActionType::kMount;
                mount.spell_id = 1; // the core interprets 1 as mount up
                actions.push_back(mount);
                state.Mounted = true;
                state.MountSentUp = true;
                state.MountRetryMs = nowMs + 5000;
                state.Last = Decision{ ActionType::kStop, 0 };
                wanted = Decision{ ActionType::kStop, 0 }; // stop moving while the mount cast completes (the stop is already sent: Last equals it, so no second one follows the mount)
            }
        }
        else if (state.Mounted &&
                 (wanted.Type != ActionType::kMoveTo ||
                  DistanceTo(bot, wanted.Dest) <= kMountDismountDistanceYards))
        {
            BotAction dismount;
            dismount.bot_guid = bot.guid;
            dismount.type = ActionType::kMount;
            dismount.spell_id = 0; // 0 = dismount
            actions.push_back(dismount);
            state.Mounted = false;
            state.MountSentUp = false;
            state.MountRetryMs = nowMs + 5000;
        }
    }

    // Idle fidgeting while a flight is awaited: the bot stands or takes a short step now and then. Only out of combat, with no target, no hostile in view, no fight
    // under way or pending, and only when this turn sent the bot nothing else (the action budget of a bot is two).
    if (fidgetWait && actions.size() == turn.actionsAtStart && bot.in_combat == 0 && bot.life_state == 0 && bot.casting_spell == 0 && bot.moving == 0 &&
        !turn.hasTarget && !turn.enemyNear && !state.InFight && !lowHealth && !state.Mounted)
    {
        bool hostileNear = false;
        for (NearbyUnit const& unit : nearby)
            hostileNear = hostileNear || (unit.is_hostile && unit.health != 0);
        if (!hostileNear)
        {
            realism::Fidget const fidget = realism::NextFidget(_options.Fidget, state.FidgetState, _options.RandomSeed, bot.guid, nowMs);
            if (fidget == realism::Fidget::Stand)
            {
                BotAction stand;
                stand.bot_guid = bot.guid;
                stand.type = ActionType::kStop;
                actions.push_back(stand);
            }
            else if (fidget != realism::Fidget::None)
            {
                // A step of 1.5 yards; the turn goes the other way round, so the bot faces the other side.
                float const angle = float((bot.guid * 2654435761ull + state.FidgetState.Counter * 40503ull) % 628) * 0.01f + (fidget == realism::Fidget::Turn ? 3.14159f : 0.0f);
                BotAction step;
                step.bot_guid = bot.guid;
                step.type = ActionType::kMoveTo;
                step.dest_x = bot.pos_x + std::cos(angle) * 1.5f;
                step.dest_y = bot.pos_y + std::sin(angle) * 1.5f;
                step.dest_z = bot.pos_z;
                actions.push_back(step);
            }
        }
    }
}

void BotPolicy::TurnSendMove(DecideTick& tick, BotTurn& turn)
{
    std::uint64_t nowMs = tick.nowMs;
    std::vector<BotAction>& actions = tick.actions;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;
    auto const& partyLeader = turn.partyLeader;
    auto const& partyLeaderBot = turn.partyLeaderBot;
    PolicyOptions::ScenarioRoute const& scenario = *turn.scenario;
    auto& wanted = turn.wanted;
    auto& moveWhy = turn.moveWhy;
    auto& kiteRetry = turn.kiteRetry;
    auto& aoeWarder = turn.aoeWarder;
    auto& kiteEntry = turn.kiteEntry;

    // PathJitter: a plain walk (not a kite, a flee or a dungeon route) ends a little to one side of the target. Out of combat only.
    if (_options.PathJitter.Enabled && wanted.Type == ActionType::kMoveTo && bot.in_combat == 0 && !turn.hasTarget && !turn.enemyNear && partyLeader == 0 &&
        scenario.RouteMap == 0 && !turn.routeBusy && !turn.dungeonHold && !state.TankLead && _options.DungeonsByMap.count(bot.map_id) == 0 &&
        !kiteEntry && kiteRetry < 0 && !turn.healLosPoint && DistanceTo(bot, wanted.Dest) > 4.0f)
    {
        std::vector<path_jitter::Pt> unwalkable;
        if (_options.Grind)
            for (std::size_t const spot : _grindBlocks.Blocked(nowMs))
                if (spot < _options.Grind->Spots.size() && _options.Grind->Spots[spot].Map == bot.map_id)
                    unwalkable.push_back({ _options.Grind->Spots[spot].X, _options.Grind->Spots[spot].Y });
        path_jitter::Facts facts;
        facts.Walking = true;
        facts.Unwalkable = &unwalkable;
        facts.From = { bot.pos_x, bot.pos_y };
        path_jitter::Pt const moved = path_jitter::Apply(_options.PathJitter, facts, { wanted.Dest.X, wanted.Dest.Y }, _options.RandomSeed, bot.guid);
        wanted.Dest.X = moved.X;
        wanted.Dest.Y = moved.Y;
    }

    if (_options.OnEvent && scenario.RouteMap != 0 && partyLeader != 0 && (wanted.Type == ActionType::kMoveTo || (wanted.Type == ActionType::kFollow && partyLeaderBot)) &&
        nowMs - state.MoveWhyLoggedMs >= 5000 && (wanted.Type == ActionType::kFollow || std::hypot(wanted.Dest.X - state.MoveWhyDest.X, wanted.Dest.Y - state.MoveWhyDest.Y) > 8.0f))
    {
        state.MoveWhyLoggedMs = nowMs;
        state.MoveWhyDest = wanted.Dest;
        _options.OnEvent("move_why: bot=" + std::to_string(bot.guid) + " why=" + (*moveWhy ? moveWhy : "other") + " type=" + std::to_string(int(wanted.Type)) + " target=" + std::to_string(wanted.Target) + " to=" + std::to_string(int(wanted.Dest.X)) + "," +
                         std::to_string(int(wanted.Dest.Y)) + "," + std::to_string(int(wanted.Dest.Z)) + " from=" + std::to_string(int(bot.pos_x)) + "," +
                         std::to_string(int(bot.pos_y)) + "," + std::to_string(int(bot.pos_z)) + " leader=" + (partyLeaderBot ? "1" : "0") + " combat=" + std::to_string(bot.in_combat) +
                         " kite_retry=" + std::to_string(kiteRetry) + " kite_entry=" + (kiteEntry ? "1" : "0") +
                         (aoeWarder ? " warder=" + std::to_string(int(aoeWarder->pos_x)) + "," + std::to_string(int(aoeWarder->pos_y)) : std::string(" warder=-")));
    }
    // A move to a place the core found no route to a moment ago is not sent again yet.
    bool const blockedMove = wanted.Type == ActionType::kMoveTo && nowMs < state.NoRouteUntilMs &&
                             std::hypot(wanted.Dest.X - state.NoRouteDest.X, wanted.Dest.Y - state.NoRouteDest.Y) < kNoRouteSameYards;
    if (!blockedMove && (!state.Last || state.Last->Type != wanted.Type || state.Last->Target != wanted.Target ||
        state.Last->Dest.X != wanted.Dest.X || state.Last->Dest.Y != wanted.Dest.Y || state.Last->Dest.Z != wanted.Dest.Z))
    {
        BotAction action;
        action.bot_guid = bot.guid;
        action.type = wanted.Type;
        action.target_guid = wanted.Target;
        action.spell_id = wanted.Type == ActionType::kTeleport ? wanted.Map : 0;
        action.dest_x = wanted.Dest.X;
        action.dest_y = wanted.Dest.Y;
        action.dest_z = wanted.Dest.Z;
        actions.push_back(action);
        state.Last = wanted;
        if (wanted.Type == ActionType::kMoveTo)
            state.LastMove = wanted;
    }
}
