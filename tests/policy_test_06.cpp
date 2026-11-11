// SPDX-License-Identifier: GPL-2.0-or-later
// Policy tests, part 6 of 8 (split out of policy_test.cpp; the helpers are in policy_test_support.h).
#include "policy_test_support.h"

namespace {

// A ranged band is 23-29 yards: a caster already inside it holds, and one nearer than 23 kites to the middle (26).
void RangedBandHoldsInsideOnyxiaBand()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : guid == 11 ? 8u : 4u; };   // 10 the warrior tank, 11 a mage, 12 a rogue
    for (float const yards : { 25.0f, 20.0f })
    {
        BotPolicy policy(options);
        WorldTickSnapshotView view = PartyView({ 10, 11, 12 }, options.ClassOf);
        NearbyUnit boss = Hostile(2, yards, 100);
        boss.guid = MakeUnitRef(8, 10184, 2);
        for (std::size_t i = 0; i < 3; ++i)
        {
            view.bots[i].threat_target_guid = boss.guid;
            view.bots[i].in_combat = 1;
            AddNearby(view, i, boss);
        }
        float mageAt = -1.0f;
        // Run past the 3 s hold the raid's damage dealers keep after the tank's first pull of Onyxia (OnyxiaPullHoldsTheDamageDealers): the mage kites after it.
        for (std::uint64_t at = 1000; at < 5000; at += 100)
        {
            view.tick_number = at / 100;
            for (BotAction const& action : policy.Decide(view, at))
            {
                if (action.bot_guid == 11 && action.type == ActionType::kMoveTo && action.target_guid == 0)
                    mageAt = std::fabs(action.dest_x - boss.pos_x);
            }
        }
        if (yards > 23.0f)
            CHECK(mageAt < 0.0f);                       // inside the band: no move
        else
            CHECK(mageAt >= 25.0f && mageAt <= 27.0f);  // kites to the middle, 26
    }
}

// A melee band is 2-9 yards and never kites: a rogue at 5 yards holds no kite move.
void MeleeDoesNotKiteFromOnyxia()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : guid == 11 ? 8u : 4u; };   // 10 the warrior tank, 11 a mage, 12 a rogue
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 }, options.ClassOf);
    NearbyUnit boss = Hostile(2, 5.0f, 100);
    boss.guid = MakeUnitRef(8, 10184, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].threat_target_guid = boss.guid;
        view.bots[i].in_combat = 1;
        AddNearby(view, i, boss);
    }
    for (std::uint64_t at = 1000; at < 3000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
            CHECK(!(action.bot_guid == 12 && action.type == ActionType::kMoveTo && action.target_guid == 0));
    }
}

// The healer's band stand point skips a hazard: a hostile ground effect over the bearing-0 point (3 yards behind the bot, 23 from Onyxia)
// sends it to the next clear point on the same 23-yard arc, outside the hazard and its 1-yard margin.
void HealerBandStandSkipsAHazard()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.AvoidGroundEffects = true;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : guid == 11 ? 5u : 4u; };   // 10 the warrior tank, 11 a priest, 12 a rogue
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 }, options.ClassOf);
    NearbyUnit boss = Hostile(2, 20.0f, 100);
    boss.guid = MakeUnitRef(8, 10184, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].threat_target_guid = boss.guid;
        view.bots[i].in_combat = 1;
        AddNearby(view, i, boss);
    }
    view.bots[1].ground_effect_count = 1;                       // the hazard is on the priest (bot 11)
    view.bots[1].ground_effects[0] = forever_module::GroundEffect{};
    view.bots[1].ground_effects[0].spell_id = 11829;
    view.bots[1].ground_effects[0].x = -3.0f;
    view.bots[1].ground_effects[0].y = 0.0f;
    view.bots[1].ground_effects[0].radius = 1.5f;
    view.bots[1].ground_effects[0].kind = 1;
    view.bots[1].ground_effects[0].hostile = 1;
    bool found = false;
    for (BotAction const& move : policy.Decide(view, 1000))
    {
        if (move.bot_guid != 11 || move.type != ActionType::kMoveTo || move.target_guid != 0)
            continue;
        found = true;
        float const hx = move.dest_x + 3.0f, hy = move.dest_y;
        CHECK(std::sqrt(hx * hx + hy * hy) > 2.5f);             // outside the hazard (radius 1.5) and its 1-yard margin
        float const bx = move.dest_x - boss.pos_x, by = move.dest_y - boss.pos_y;   // the band is a distance, so measure the whole offset, not its x part
        CHECK(std::sqrt(bx * bx + by * by) >= 21.0f && std::sqrt(bx * bx + by * by) <= 25.0f);
    }
    CHECK(found);
}

// The warrior tank casts Berserker Rage on its own 30 s timer while Onyxia is up: over 40 s of a fight, two casts (at once, and 30 s on).
void OnyxiaTankBerserkerRageOnATimer()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : guid == 11 ? 4u : 5u; };   // 10 the warrior tank, 11 a rogue, 12 a priest
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 }, options.ClassOf);
    NearbyUnit boss = Hostile(2, 5.0f, 100);
    boss.guid = MakeUnitRef(8, 10184, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].threat_target_guid = boss.guid;
        view.bots[i].in_combat = 1;
        AddNearby(view, i, boss);
    }
    int rages = 0;
    for (std::uint64_t at = 1000; at < 41000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 10 && action.type == ActionType::kCastSpell && action.spell_id == 18499)
            {
                CHECK(action.target_guid == 0);   // on the tank itself
                ++rages;
            }
    }
    CHECK(rages == 2);
}

// Taunt (355) on Onyxia: the tank re-taunts when another party member is fighting her and the tank is not, once per 10 s. Not while the tank is on her.
void OnyxiaTankTauntsWhenTheRaidIsOnHer()
{
    for (bool tankOnHer : { false, true })
    {
        PolicyOptions options = AggressiveOptions();
        options.PartySize = 3;
        options.PartyCount = 1;
        options.WanderWhenIdle = false;
        options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : guid == 11 ? 4u : 5u; };   // 10 the warrior tank, 11 a rogue, 12 a priest
        BotPolicy policy(options);
        WorldTickSnapshotView view = PartyView({ 10, 11, 12 }, options.ClassOf);
        NearbyUnit boss = Hostile(2, 20.0f, 100);
        boss.guid = MakeUnitRef(8, 10184, 2);
        NearbyUnit other = Hostile(3, 5.0f, 100);
        other.guid = MakeUnitRef(8, 12129, 3);
        view.bots[0].threat_target_guid = tankOnHer ? boss.guid : other.guid;
        view.bots[1].threat_target_guid = boss.guid;
        view.bots[2].threat_target_guid = other.guid;
        for (std::size_t i = 0; i < 3; ++i)
        {
            view.bots[i].in_combat = 1;
            AddNearby(view, i, boss);
            AddNearby(view, i, other);
        }
        int taunts = 0;
        for (std::uint64_t at = 1000; at < 14000; at += 100)
        {
            view.tick_number = at / 100;
            for (BotAction const& action : policy.Decide(view, at))
                if (action.bot_guid == 10 && action.type == ActionType::kCastSpell && action.spell_id == 355)
                {
                    CHECK(action.target_guid == boss.guid);
                    ++taunts;
                }
        }
        // The rage at 1 s holds the global cooldown, so the first taunt comes at 2.5 s, the next at 12.5 s (10 s on); none while the tank is on Onyxia.
        CHECK(taunts == (tankOnHer ? 0 : 2));
    }
}

// Threat discipline: the tank's first pull of Onyxia holds the raid's damage dealers for 3 s. Before that the rogue (11) does not attack her; after, it does.
void OnyxiaPullHoldsTheDamageDealers()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : guid == 11 ? 4u : 5u; };   // 10 the warrior tank, 11 a rogue, 12 a priest
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 }, options.ClassOf);
    NearbyUnit boss = Hostile(2, 5.0f, 100);
    boss.guid = MakeUnitRef(8, 10184, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].threat_target_guid = boss.guid;
        view.bots[i].in_combat = 1;
        AddNearby(view, i, boss);
    }
    int rogueEarly = 0, rogueLate = 0, tankAttacks = 0;
    for (std::uint64_t at = 1000; at < 6000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
        {
            if (action.bot_guid == 10 && action.type == ActionType::kAttackTarget)
                ++tankAttacks;
            if (action.bot_guid == 11 && action.target_guid == boss.guid)
                (at < 4000 ? rogueEarly : rogueLate) += 1;        }
    }
    CHECK(tankAttacks > 0);
    CHECK(rogueEarly == 0);
    CHECK(rogueLate > 0);
}

// Phases and the landing (study items 2 and 3): at 40% Onyxia lands and her threat list resets, so the damage dealers hold until the tank has her again.
// The rogue (11) does not attack her while the tank (10) is off her, and does once the tank takes her at 3 s; the phase is read from her health.
void OnyxiaLandingHoldsDamageDealersUntilTankHasHer()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : guid == 11 ? 4u : 5u; };
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 }, options.ClassOf);
    NearbyUnit boss = Hostile(2, 5.0f, 35);   // landed: 35% of her health
    boss.guid = MakeUnitRef(8, 10184, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].in_combat = 1;
        AddNearby(view, i, boss);
    }
    int rogueEarly = 0, rogueLate = 0;
    for (std::uint64_t at = 1000; at < 9000; at += 100)
    {
        view.tick_number = at / 100;
        view.bots[0].threat_target_guid = at >= 3000 ? boss.guid : 0;   // the tank takes her again at 3 s
        view.bots[1].threat_target_guid = boss.guid;
        for (BotAction const& action : policy.Decide(view, at))
        {
            if (action.bot_guid == 11 && action.target_guid == boss.guid)
                (at < 3000 ? rogueEarly : rogueLate) += 1;
        }
    }
    CHECK(rogueEarly == 0);
    CHECK(rogueLate > 0);
}

// Onyxian Warder (12129): its Fire Nova hits everything within 20 yards, so a ranged raid member backs off to 21+ yards and only the tank melees it, every other member backs off, melee classes too.
void RaidCasterKitesOutOfWarderNova()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : guid == 11 ? 8u : 4u; };   // 10 the warrior tank, 11 a mage, 12 a rogue
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 });
    NearbyUnit warder = Hostile(2, 5.0f, 100);
    warder.guid = MakeUnitRef(8, 12129, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].threat_target_guid = warder.guid;
        view.bots[i].in_combat = 1;
        AddNearby(view, i, warder);
    }
    int tankKites = 0;
    float mageAt = -1.0f, rogueAt = -1.0f;
    for (std::uint64_t at = 1000; at < 3000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
        {
            bool const move = action.type == ActionType::kMoveTo && action.target_guid == 0;
            tankKites += action.bot_guid == 10 && move;
            if (action.bot_guid == 11 && move)
                mageAt = std::fabs(action.dest_x - warder.pos_x);
            if (action.bot_guid == 12 && move)
                rogueAt = std::fabs(action.dest_x - warder.pos_x);
        }
    }
    CHECK(mageAt >= 21.0f);
    CHECK(tankKites == 0);
    CHECK(rogueAt >= 21.0f);
}

// The Fire Nova hits every party member in reach, not only the ones fighting the Warder: a mage on another target backs off too, each bot on its own bearing.
void RaidMemberOnAnotherTargetKitesOutOfWarderNova()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : 8u; };   // 10 the warrior tank, 11 and 12 mages
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 });
    NearbyUnit warder = Hostile(2, 5.0f, 100);
    warder.guid = MakeUnitRef(8, 12129, 2);
    NearbyUnit other = Hostile(3, 30.0f, 100);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].threat_target_guid = i == 0 ? warder.guid : other.guid;
        view.bots[i].in_combat = 1;
        AddNearby(view, i, warder);
        AddNearby(view, i, other);
    }
    int tankKites = 0;
    float firstAt = -1.0f, secondAt = -1.0f;
    float firstY = 0.0f, secondY = 0.0f;
    for (std::uint64_t at = 1000; at < 3000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
        {
            bool const move = action.type == ActionType::kMoveTo && action.target_guid == 0;
            tankKites += action.bot_guid == 10 && move;
            if (action.bot_guid == 11 && move)
            {
                firstAt = std::hypot(action.dest_x - warder.pos_x, action.dest_y - warder.pos_y);
                firstY = action.dest_y;
            }
            if (action.bot_guid == 12 && move)
            {
                secondAt = std::hypot(action.dest_x - warder.pos_x, action.dest_y - warder.pos_y);
                secondY = action.dest_y;
            }
        }
    }
    CHECK(firstAt >= 24.0f);
    CHECK(secondAt >= 24.0f);
    CHECK(tankKites == 0);
    CHECK(firstY != secondY);   // not one shared point
}

// The 2026-10-29 pull: the tank pulled the Warder and the other bots stayed out of combat for 55 s, standing next to it. A non-tank bot that is not in combat kites
// as soon as a live Warder is inside the margin; the tank never does.
void NonTankOutOfCombatKitesWhenWarderIsWithinMargin()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : 8u; };   // 10 the warrior tank, 11 and 12 mages
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 });
    NearbyUnit warder = Hostile(2, 25.0f, 100);   // inside the 26 yard margin, outside the 24.5 yard nova
    warder.guid = MakeUnitRef(8, 12129, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].in_combat = 0;
        view.bots[i].threat_target_guid = 0;
        AddNearby(view, i, warder);
    }
    int tankKites = 0, mageKites = 0;
    for (std::uint64_t at = 1000; at < 3000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
        {
            bool const move = action.type == ActionType::kMoveTo && action.target_guid == 0;
            tankKites += action.bot_guid == 10 && move;
            if (action.bot_guid == 11 && move)
            {
                ++mageKites;
                CHECK(std::hypot(action.dest_x - warder.pos_x, action.dest_y - warder.pos_y) >= 25.5f);
            }
        }
    }
    CHECK(mageKites > 0);
    CHECK(tankKites == 0);
}

// The tank holds the Warder; a mage standing where the kite puts it (kAoeStandYards, 26) is inside its Fireball's usable reach (30 less the 2-yard slack) and casts it,
// without kiting further out. At the old 30-yard stand no 30-yard spell was ever cast on a Warder (2026-11-08, 126 fight_stalled at 28-31 yards, Warder at full health).
void RaidCasterAtTheWarderStandCastsOnIt()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : 8u; };   // 10 the warrior tank, 11 and 12 mages
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 }, options.ClassOf);
    NearbyUnit warder = Hostile(2, 26.0f, 100);
    warder.guid = MakeUnitRef(8, 12129, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].threat_target_guid = warder.guid;
        view.bots[i].in_combat = 1;
        AddNearby(view, i, warder);
    }
    view.bots[0].pos_x = 25.0f;   // the tank in melee
    int fireballs = 0, mageMoves = 0;
    for (std::uint64_t at = 1000; at < 6000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
        {
            if (action.bot_guid == 11 && action.type == ActionType::kCastSpell && action.spell_id == 133 && action.target_guid == warder.guid)
                ++fireballs;
            if (action.bot_guid == 11 && action.type == ActionType::kMoveTo)
                ++mageMoves;
        }
    }
    CHECK(fireballs > 0);
    CHECK(mageMoves == 0);
}

// A member whose own nearby list does not hold the Warder (in the sim it was full of the raid's pets and totems) still sees the one its tank fights, in the
// tank's list: it assists it from the stand, and it kites when the Warder is inside the nova, the same as with the Warder in its own list.
void RaidMemberSeesTheWarderInItsLeadersView()
{
    for (float warderX : { 26.0f, 10.0f })
    {
        std::vector<std::string> events;
        PolicyOptions options = AggressiveOptions();
        options.PartySize = 3;
        options.PartyCount = 1;
        options.WanderWhenIdle = false;
        options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : 8u; };   // 10 the warrior tank, 11 and 12 mages
        options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
        BotPolicy policy(options);
        WorldTickSnapshotView view = PartyView({ 10, 11, 12 }, options.ClassOf);
        NearbyUnit warder = Hostile(2, warderX, 100);
        warder.guid = MakeUnitRef(8, 12129, 2);
        view.bots[0].pos_x = warderX - 1.0f;   // the tank in melee
        view.bots[0].threat_target_guid = warder.guid;
        view.bots[0].in_combat = 1;
        AddNearby(view, 0, warder);           // only the tank's list holds it
        int fireballs = 0, kites = 0;
        for (std::uint64_t at = 1000; at < 6000; at += 100)
        {
            view.tick_number = at / 100;
            for (BotAction const& action : policy.Decide(view, at))
            {
                if (action.bot_guid == 11 && action.type == ActionType::kCastSpell && action.spell_id == 133 && action.target_guid == warder.guid)
                    ++fireballs;
                if (action.bot_guid == 11 && action.type == ActionType::kMoveTo && action.target_guid == 0)
                {
                    ++kites;
                    CHECK(std::hypot(action.dest_x - warder.pos_x, action.dest_y - warder.pos_y) >= 25.5f);
                }
            }
        }
        if (warderX == 26.0f)
        {
            CHECK(fireballs > 0);
            CHECK(kites == 0);
        }
        else
            CHECK(kites > 0);
        CHECK(Logged(events, "warder_unseen: bot=11"));
    }
}

// A dead Warder in the leader's view is logged once (warder_dead), the kill measure of the Onyxia runs.
void TheLeaderLogsADeadWarderOnce()
{
    std::vector<std::string> events;
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : 8u; };
    options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 }, options.ClassOf);
    NearbyUnit warder = Hostile(2, 5.0f, 0);   // a corpse: health 0
    warder.guid = MakeUnitRef(8, 12129, 2);
    AddNearby(view, 0, warder);
    for (std::uint64_t at = 1000; at < 3000; at += 100)
        policy.Decide(view, at);
    int lines = 0;
    for (std::string const& event : events)
        lines += event.rfind("warder_dead: leader=10", 0) == 0;
    CHECK(lines == 1);
}

// The kite point must be somewhere the core can path to. The module has no path query, so the first point is the retreat along the entry route (the line from the Warder
// toward the dungeon entry, 26 yards out); when the core refuses it, the next point is the circle round the Warder, a different place and not the refused one again.
void KiteRetreatsAlongTheEntryRouteAndFallsBackToTheCircle()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 3;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : 8u; };   // 10 the warrior tank, 11 and 12 mages
    PolicyOptions::DungeonRoute route;
    route.HasEntry = true;
    route.Entry = Point{ 5.0f, -100.0f, 0.0f };   // straight behind the Warder, which stands at (5, 0)
    options.DungeonsByMap[0] = route;
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12 });
    NearbyUnit warder = Hostile(2, 5.0f, 100);
    warder.guid = MakeUnitRef(8, 12129, 2);
    for (std::size_t i = 0; i < 3; ++i)
    {
        view.bots[i].in_combat = 1;
        AddNearby(view, i, warder);
    }
    auto kiteOf = [&](std::uint64_t at, bool& found) {
        Point dest{};
        found = false;
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
            if (action.bot_guid == 11 && action.type == ActionType::kMoveTo && action.target_guid == 0)
            {
                dest = Point{ action.dest_x, action.dest_y, action.dest_z };
                found = true;
            }
        return dest;
    };
    bool found = false;
    Point const retreat = kiteOf(1000, found);
    CHECK(found);
    float const length = std::hypot(retreat.X - warder.pos_x, retreat.Y - warder.pos_y);
    CHECK(length >= 25.0f && length <= 27.0f);
    CHECK((retreat.Y - warder.pos_y) / length < -0.9f);   // toward the entry, within a spread of 25 degrees

    view.bots[1].last_action_sequence = 1;
    view.bots[1].last_action_type = uint8_t(ActionType::kMoveTo);
    view.bots[1].last_action_status = BotActionStatus::kNoRoute;
    Point const fallback = kiteOf(2000, found);
    CHECK(found);
    CHECK(std::hypot(fallback.X - retreat.X, fallback.Y - retreat.Y) >= 12.0f);   // not the refused place
    CHECK(std::hypot(fallback.X - warder.pos_x, fallback.Y - warder.pos_y) >= 25.5f);

    // A second refusal: the third point is neither of the two refused ones (it used to go back to the first, 91,000 refusals in 15 minutes on 2026-10-30).
    view.bots[1].last_action_sequence = 2;
    Point const third = kiteOf(3000, found);
    CHECK(found);
    CHECK(std::hypot(third.X - retreat.X, third.Y - retreat.Y) >= 12.0f);
    CHECK(std::hypot(third.X - fallback.X, third.Y - fallback.Y) >= 12.0f);
}

// A party member on low health keeps fighting: its healer needs it to hold still, and a fleeing bot drags the pack onto the party.
void PartyMemberDoesNotFleeOnLowHealth()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 2;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : 5u; };   // 10 the warrior leads
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11 });
    NearbyUnit mob = Hostile(2, 3.0f, 100);
    for (std::size_t i = 0; i < 2; ++i)
    {
        view.bots[i].in_combat = 1;
        view.bots[i].threat_target_guid = MakeUnitRef(8, 1, 2);
        AddNearby(view, i, mob);
    }
    view.bots[0].health = 100;                              // the tank at 10 %
    int flees = 0;
    for (std::uint64_t at = 1000; at < 4000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
            flees += action.bot_guid == 10 && action.type == ActionType::kMoveTo;
    }
    CHECK(flees == 0);
    // Alone, the same bot still runs.
    BotPolicy alone = ClassPolicy(1);
    WorldTickSnapshotView solo = View(1, MakeUnitRef(8, 1, 2), { Hostile(2, 3.0f, 100) });
    solo.bots[0].in_combat = 1;
    solo.bots[0].health = 100;
    bool fled = false;
    for (BotAction const& action : alone.Decide(solo, 1000))
        fled = fled || action.type == ActionType::kMoveTo;
    CHECK(fled);
}

// A party member in a fight does not chase a target that has run far off (a fleeing creature): it stands with its party.
void PartyMemberDoesNotChaseAFleeingTarget()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 2;
    options.PartyCount = 1;
    options.WanderWhenIdle = false;
    options.ClassOf = [](std::uint64_t guid) { return guid == 10 ? 1u : 5u; };
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11 });
    NearbyUnit mob = Hostile(2, 70.0f, 20);                 // 70 yards off and running
    for (std::size_t i = 0; i < 2; ++i)
    {
        view.bots[i].in_combat = 1;
        view.bots[i].threat_target_guid = MakeUnitRef(8, 1, 2);
        AddNearby(view, i, mob);
    }
    int follows = 0;
    for (std::uint64_t at = 1000; at < 4000; at += 100)
    {
        view.tick_number = at / 100;
        for (BotAction const& action : policy.Decide(view, at))
            follows += action.bot_guid == 10 && action.type == ActionType::kFollow && action.target_guid == MakeUnitRef(8, 1, 2);
    }
    CHECK(follows == 0);
}

// A ranged bot at or beyond kiting distance attacks normally.
void RangedBotAttacksWhenBackAtRange()
{
    BotPolicy policy = ClassPolicy(8);
    WorldTickSnapshotView view = View(1, MakeUnitRef(8, 1, 2), { Hostile(2, 12.0f, 100) });
    view.bots[0].in_combat = 1;
    auto actions = policy.Decide(view, 1000);
    bool attacked = false;
    for (BotAction const& action : actions)
        attacked = attacked || (action.type == ActionType::kAttackTarget && action.target_guid == MakeUnitRef(8, 1, 2));
    CHECK(attacked);
}

// Below 30% health, in combat, threat close: flee away from the threat target.
void HurtBotFleesFromThreat()
{
    WorldTickSnapshotView view = Hurt(250, { Unit(8, 2, 10.0f, true) }, MakeUnitRef(8, 1, 9));
    view.bots[0].in_combat = 1;
    BotPolicy policy(AggressiveOptions());
    auto actions = policy.Decide(view, 1000);
    bool fled = false;
    for (BotAction const& action : actions)
        fled = fled || (action.type == ActionType::kMoveTo && action.target_guid == 0);
    CHECK(fled);
}

// Below 30% health, but the threat is farther than kFleeYards: stop, do not flee.
void HurtBotDoesNotFleeWhenThreatIsFar()
{
    WorldTickSnapshotView view = Hurt(250, { Unit(8, 2, 20.0f, true) }, MakeUnitRef(8, 1, 9));
    view.bots[0].in_combat = 1;
    BotPolicy policy(AggressiveOptions());
    auto actions = policy.Decide(view, 1000);
    for (BotAction const& action : actions)
        CHECK(action.type != ActionType::kMoveTo);
}

// Vendor 200 yards away: above the mount threshold, the bot mounts before walking.
void BotMountsForALongWalk()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 200.0f);
    BotPolicy policy = VendorPolicy();
    auto actions = policy.Decide(view, 0);
    bool mounted = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMount && action.spell_id == 1)
            mounted = true;
    CHECK(mounted);
}

// Riding is learned at level 40: below MountMinLevel the bot walks the long way on foot and never sends a mount.
void BotBelowTheMountLevelNeverAsksToMount()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 200.0f);
    view.bots[0].level = 30;
    PolicyOptions options = AggressiveOptions();
    options.ServiceEveryMs = 60000;
    options.MountMinLevel = 40;
    BotPolicy policy(options);
    bool walks = false;
    for (std::uint64_t at = 0; at < 3000; at += 100)
        for (BotAction const& action : policy.Decide(view, at))
        {
            CHECK(action.type != ActionType::kMount);
            walks = walks || action.type == ActionType::kMoveTo;
        }
    CHECK(walks);
    view.bots[0].level = 40;
    BotPolicy rider(options);
    bool mounts = false;
    for (BotAction const& action : rider.Decide(view, 0))
        mounts = mounts || (action.type == ActionType::kMount && action.spell_id == 1);
    CHECK(mounts);
}

// Vendor 50 yards away: below the mount threshold, the bot walks without mounting.
void BotDoesNotMountForAShortWalk()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 50.0f);
    BotPolicy policy = VendorPolicy();
    auto actions = policy.Decide(view, 0);
    for (BotAction const& action : actions)
        CHECK(action.type != ActionType::kMount);
    bool walks = false;
    for (BotAction const& action : actions)
        walks = walks || (action.type == ActionType::kMoveTo && action.dest_x == 50.0f);
    CHECK(walks);
}

// Already mounted with the vendor close: dismount.
void BotDismountsWhenClose()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 10.0f);
    view.bots[0].last_action_sequence = 1;
    view.bots[0].last_action_type = uint8_t(ActionType::kMount);
    view.bots[0].last_action_spell = 1;
    view.bots[0].last_action_status = BotActionStatus::kApplied;
    BotPolicy policy = VendorPolicy();
    auto actions = policy.Decide(view, 1000);
    bool dismounted = false;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMount && action.spell_id == 0)
            dismounted = true;
    CHECK(dismounted);
}

// In combat with a target: do not mount even if a vendor is far away.
void BotDoesNotMountInCombat()
{
    WorldTickSnapshotView view = PartyView({ 10 });
    SetVendor(view, 200.0f);
    view.bots[0].in_combat = 1;
    view.bots[0].threat_target_guid = MakeUnitRef(8, 1, 2);
    AddNearby(view, 0, Hostile(2, 5.0f, 100));
    BotPolicy policy = VendorPolicy();
    auto actions = policy.Decide(view, 0);
    for (BotAction const& action : actions)
        CHECK(action.type != ActionType::kMount);
}

// A hostile ten yards away that the bot is not fighting: below 30% health it runs, it does not stand and rest.
void LowHealthFleesFromAHostileItIsNotFighting()
{
    WorldTickSnapshotView view = Hurt(250, { Unit(8, 2, 10.0f, true) });
    BotPolicy policy(AggressiveOptions());
    auto actions = policy.Decide(view, 0);
    CHECK(actions.size() == 1);
    if (actions.size() == 1)
        CHECK(actions[0].type == ActionType::kMoveTo);
}

// A pull is the weakest creature that may be pulled, not the closest: finishing it before the next is more experience per hour.
void WeakestTargetIsPulledFirst()
{
    BotPolicy policy(AggressiveOptions());
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, Hostile(7, 20.0f, 20)); // hurt, far away
    AddNearby(view, 0, Hostile(8, 3.0f, 100)); // healthy, close
    auto const actions = policy.Decide(view, 0);
    CHECK(HasAction(actions, 10, ActionType::kFollow, MakeUnitRef(8, 1, 7)));
}

// Two that may equally well be pulled: the closest one. It is 3 yards away, so the bot is already in range and attacks rather than
// walking up (a target still out of range is kFollow: see WeakestTargetIsPulledFirst).
void ClosestBreaksTheTieBetweenEqualTargets()
{
    BotPolicy policy(AggressiveOptions());
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, Hostile(7, 12.0f, 100));
    AddNearby(view, 0, Hostile(8, 3.0f, 100));
    auto const actions = policy.Decide(view, 0);
    CHECK(HasAction(actions, 10, ActionType::kAttackTarget, MakeUnitRef(8, 1, 8)));
}

// Two rogues of one party, a target that is casting: one interrupts it and the other keeps building combo points.
void OnlyOneRogueInterruptsACastingTarget()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t) { return 4; }; // every bot a rogue
    BotPolicy policy(options);

    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 });
    NearbyUnit caster = Hostile(7, 3.0f, 100);
    caster.casting = 1;
    AddNearby(view, 0, caster);
    view.bots[0].casting_spell = 1766; // bot 10 already has Kick on it
    auto const actions = policy.Decide(view, 0);
    CHECK(!HasCast(actions, 1766)); // a second Kick lands on nothing
}

// One rogue with no party: it interrupts as before.
void RogueWithoutAPartyStillInterrupts()
{
    NearbyUnit caster = Hostile(2, 3.0f, 100);
    caster.casting = 1;
    auto const actions = ClassPolicy(4).Decide(View(1, 0, { caster }), 1000);
    CHECK(HasCast(actions, 1766));
}

// Two priests of one party, one member hurt: one heals it and the others look elsewhere while that heal is in flight.
void TwoHealersDoNotHealTheSameMember()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = [](std::uint64_t guid) { return guid == 12 ? 1u : 5u; }; // warrior leads, rest priests
    BotPolicy policy(options);
    policy.SetRouteActive(true);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 }, [](std::uint64_t guid) { return guid == 12 ? 1u : 5u; });
    // Pre-form the party: bot 12 leads, all are members.
    constexpr std::uint64_t kLeaderRef = (std::uint64_t(2) << 56) | 12; // HighGuid::Player = 2
    for (BotSnapshot& bot : view.bots)
    {
        if (bot.guid == 12)
        {
            bot.group_role = 2;
            bot.group_leader_guid = kLeaderRef; // leader's own GUID as unit reference
        }
        else
        {
            bot.group_role = 1;
            bot.group_leader_guid = kLeaderRef;
        }
    }
    view.bots[3].health = 300; // bot 13 is hurt
    auto const actions = policy.Decide(view, 0);
    int heals = 0;
    std::uint64_t healed = 0;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kCastSpell && action.spell_id == 2050)
        {
            ++heals;
            healed = action.target_guid;
        }
    CHECK(heals == 1);
    CHECK(healed == PlayerOf(13));
}

// The leader (the tank) is kept alive first while it holds a target, even when someone else is on less health.
void TankIsHealedBeforeTheLowestMember()
{
    PolicyOptions options = AggressiveOptions();
    options.PartySize = 5;
    options.PartyCount = 1;
    options.ClassOf = WarriorLeads; // bot 12 is the warrior and leads
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10, 11, 12, 13, 14 }, WarriorLeads);
    // Pre-form the party: bot 12 leads, others follow.
    for (BotSnapshot& bot : view.bots)
    {
        if (bot.guid == 12)
        {
            bot.group_role = 2;
            bot.group_leader_guid = (std::uint64_t(2) << 56) | 12; // full Player unit ref
        }
        else
        {
            bot.group_role = 1;
            bot.group_leader_guid = (std::uint64_t(2) << 56) | 12;
        }
    }
    view.bots[2].health = 600; // the leader: 60%, holding a target
    view.bots[3].health = 300; // a member on 30%
    view.bots[2].threat_target_guid = Hostile(7, 3.0f, 100).guid; // the tank is fighting
    AddNearby(view, 2, Hostile(7, 3.0f, 100));
    auto const actions = policy.Decide(view, 0);
    CHECK(CastsSpell(actions, 10, 2050, PlayerOf(12))); // the tank, not the member
}

// Out of a fight, on a dungeon route, the healer heals the leader up (below 90%): the leader walks on only at 80%, and cannot sit with creatures in view.
void TheRouteLeaderIsHealedUpOutOfAFight()
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
    }
    view.bots[2].health = 650;                                        // the leader at 65%, no target, nobody fighting
    CHECK(CastsSpell(policy.Decide(view, 0), 10, 2050, PlayerOf(12)));
    // Healthy enough (95%): no heal.
    BotPolicy healthy(options);
    healthy.SetRouteActive(true);
    view.bots[2].health = 950;
    CHECK(!CastsSpell(healthy.Decide(view, 0), 10, 2050, PlayerOf(12)));
}

// The route leader waits while a mana user of the party is dry (the healer walked into fights at 3% mana), and goes on after kRouteManaMaxMs.
void TheRouteWaitsForTheHealersMana()
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
    }
    for (BotSnapshot& bot : view.bots)
        if (bot.guid == 10)
        {
            bot.power_type = 0;
            bot.max_power = 1000;
            bot.power = 50; // the healer at 5%
        }
    auto const walks = [](std::vector<BotAction> const& actions, std::uint64_t guid)
    {
        for (BotAction const& action : actions)
            if (action.bot_guid == guid && action.type == ActionType::kMoveTo && action.dest_x == 100.0f) // the route point
                return true;
        return false;
    };
    CHECK(!walks(policy.Decide(view, 1000), 12));
    CHECK(!walks(policy.Decide(view, 30000), 12));
    CHECK(walks(policy.Decide(view, 100000), 12)); // given up after 35 s
    // Mana full: the leader walks at once.
    BotPolicy full(options);
    full.SetRouteActive(true);
    for (BotSnapshot& bot : view.bots)
        if (bot.guid == 10)
            bot.power = 1000;
    CHECK(walks(full.Decide(view, 0), 12));
}

} // namespace

void RunPolicyTestsPart6()
{
    RangedBandHoldsInsideOnyxiaBand();
    MeleeDoesNotKiteFromOnyxia();
    OnyxiaTankBerserkerRageOnATimer();
    OnyxiaTankTauntsWhenTheRaidIsOnHer();
    OnyxiaPullHoldsTheDamageDealers();
    OnyxiaLandingHoldsDamageDealersUntilTankHasHer();
    HealerBandStandSkipsAHazard();
    RaidCasterKitesOutOfWarderNova();
    RaidMemberOnAnotherTargetKitesOutOfWarderNova();
    NonTankOutOfCombatKitesWhenWarderIsWithinMargin();
    RaidCasterAtTheWarderStandCastsOnIt();
    RaidMemberSeesTheWarderInItsLeadersView();
    TheLeaderLogsADeadWarderOnce();
    KiteRetreatsAlongTheEntryRouteAndFallsBackToTheCircle();
    PartyMemberDoesNotFleeOnLowHealth();
    PartyMemberDoesNotChaseAFleeingTarget();
    RangedBotAttacksWhenBackAtRange();
    HurtBotFleesFromThreat();
    HurtBotDoesNotFleeWhenThreatIsFar();
    BotMountsForALongWalk();
    BotBelowTheMountLevelNeverAsksToMount();
    BotDoesNotMountForAShortWalk();
    BotDismountsWhenClose();
    BotDoesNotMountInCombat();
    LowHealthFleesFromAHostileItIsNotFighting();
    WeakestTargetIsPulledFirst();
    ClosestBreaksTheTieBetweenEqualTargets();
    OnlyOneRogueInterruptsACastingTarget();
    RogueWithoutAPartyStillInterrupts();
    TwoHealersDoNotHealTheSameMember();
    TankIsHealedBeforeTheLowestMember();
    TheRouteLeaderIsHealedUpOutOfAFight();
    TheRouteWaitsForTheHealersMana();
}
