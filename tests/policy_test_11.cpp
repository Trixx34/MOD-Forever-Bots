// SPDX-License-Identifier: GPL-2.0-or-later
// Policy tests, part 11: leveling deaths (prod 2026-11-14: 7441 deaths against 25 level-ups, 56 of 150 online bots ghosts). The idle rule of a bot that starts no fight
// of its own (IdleEnemyAttackersOnly), a caster with no spell (CasterMeleeWithoutSpell), the level-gap guard (EngageMaxLevelAbove, EngageEliteSolo), the ghost fallback
// (GhostFallbackMs, GhostMaxMs) and the death area (DeathAreaCount).
#include "policy_test_support.h"
#include "bot_policy_internal.h"

namespace {

std::shared_ptr<GrindData> FarSpot()
{
    std::vector<std::string> warnings;
    std::istringstream spots("spot 0 5000 0 0 1 10 0\n");
    return std::make_shared<GrindData>(ParseGrindData(spots, warnings));
}

bool MovesTo(std::vector<BotAction> const& actions, std::uint64_t bot, float x)
{
    for (BotAction const& action : actions)
        if (action.bot_guid == bot && action.type == ActionType::kMoveTo && action.dest_x == x)
            return true;
    return false;
}

bool AskedSpiritHealer(std::vector<BotAction> const& actions, std::uint64_t bot)
{
    for (BotAction const& action : actions)
        if (action.type == ActionType::kResurrect && action.bot_guid == bot && action.spell_id == 2)
            return true;
    return false;
}

// A defensive bot (AggressiveStart off) with a creature beside it that attacks nobody walks on to its grind spot; the old rule (any live creature in view) kept it
// standing for good (the sim's 'Galorogar'). A creature attacking its pet still holds it.
void ADefensiveBotWalksOnPastACreatureThatAttacksNobody()
{
    auto const walks = [](bool attackersOnly, std::uint64_t victim)
    {
        PolicyOptions options;   // the defensive default
        options.Grind = FarSpot();
        options.IdleEnemyAttackersOnly = attackersOnly;
        BotPolicy policy(options);
        bool walked = false;
        for (std::uint64_t now = 1000; now <= 120000 && !walked; now += 5000)
        {
            NearbyUnit boar = Hostile(7, 10.0f, 100);
            boar.level = 6;
            boar.victim_ref = victim;
            WorldTickSnapshotView view = View(1, 0, { boar });
            view.bots[0].level = 6;
            view.bots[0].pet_guid = MakeUnitRef(8, 99, 1);
            walked = MovesTo(policy.Decide(view, now), kBot, 5000.0f);
        }
        return walked;
    };
    CHECK(walks(true, 0));                              // nobody attacked: it walks on
    CHECK(!walks(false, 0));                            // the old rule: it stands
    CHECK(!walks(true, MakeUnitRef(8, 99, 1)));         // its pet is attacked: it stays
}

// A level 1 shaman whose only damage row starts at level 8 (Lightning Bolt rank 2 in the rotation data) has nothing to cast: a creature 8 yards off is fought in melee,
// not kited back to the 12-yard stand-off of a caster (the core's attack order then runs it in again, and no damage is ever done; the 2026-11-14 sim: shamans averaged
// level 1.2 after two hours). With CasterMeleeWithoutSpell off, or at level 10 (Lightning Bolt learned), it kites as before.
void ACasterWithNoSpellAtItsLevelMelees()
{
    auto const decide = [](bool melee, std::uint32_t level)
    {
        std::vector<std::string> warnings;
        std::istringstream rows("class 7\nrow 529 level 8 always target range 0 30\n");
        PolicyOptions options = AggressiveOptions();
        options.Rotations = std::make_shared<rotation::RotationData>(rotation::ParseRotationData(rows, warnings));
        options.ClassOf = [](std::uint64_t) { return 7u; };
        options.CasterMeleeWithoutSpell = melee;
        BotPolicy policy(options);
        NearbyUnit boar = Hostile(7, 8.0f, 100);
        boar.level = level;
        WorldTickSnapshotView view = View(1, 0, { boar });
        view.bots[0].level = level;
        view.bots[0].power_type = 0;
        view.bots[0].power = 100;
        view.bots[0].max_power = 100;
        std::vector<BotAction> const actions = policy.Decide(view, 1000);
        bool kites = false;
        for (BotAction const& action : actions)
            kites = kites || (action.bot_guid == kBot && action.type == ActionType::kMoveTo && action.dest_x < 0.0f);
        return (HasAction(actions, kBot, ActionType::kAttackTarget, boar.guid) || HasAction(actions, kBot, ActionType::kFollow, boar.guid)) && !kites;
    };
    CHECK(decide(true, 1));       // no spell at level 1: in to melee
    CHECK(!decide(false, 1));     // the old stand-off
    CHECK(!decide(true, 10));     // level 10 has Lightning Bolt: it stays at range
}

// A level 10 shaman whose Lightning Bolt came back "not known" (60 s retry) stays in melee until the retry has run out. It used to leave melee 30 s in, then stand at
// range casting nothing (the cast loop skips the spell until its retry ends) for the other 30 s.
void ACasterWithNotKnownSpellStaysInMeleeUntilRetry()
{
    auto const kites = [](std::uint64_t nowMs)
    {
        std::vector<std::string> warnings;
        std::istringstream rows("class 7\nrow 529 level 8 always target range 0 30\n");
        PolicyOptions options = AggressiveOptions();
        options.Rotations = std::make_shared<rotation::RotationData>(rotation::ParseRotationData(rows, warnings));
        options.ClassOf = [](std::uint64_t) { return 7u; };
        BotPolicy policy(options);
        NearbyUnit boar = Hostile(7, 8.0f, 100);
        boar.level = 10;
        WorldTickSnapshotView view = View(1, 0, { boar });
        view.bots[0].level = 10;
        view.bots[0].power_type = 0;
        view.bots[0].power = 100;
        view.bots[0].max_power = 100;
        view.bots[0].last_action_sequence = 1;
        view.bots[0].last_action_type = static_cast<std::uint8_t>(ActionType::kCastSpell);
        view.bots[0].last_action_spell = 529;
        view.bots[0].last_action_status = BotActionStatus::kNotKnown;
        policy.Decide(view, 1000);                                   // the answer is heard: retry at 61000
        view.bots[0].last_action_status = BotActionStatus::kNone;
        std::vector<BotAction> const actions = policy.Decide(view, nowMs);
        bool backs = false;
        for (BotAction const& action : actions)
            backs = backs || (action.bot_guid == kBot && action.type == ActionType::kMoveTo && action.dest_x < 0.0f);
        return backs;
    };
    CHECK(!kites(5000));          // just after the answer: melee, no kiting back to the stand-off
    CHECK(!kites(45000));         // 44 s later (retry 16 s away): still melee, not the 30 s flip
    CHECK(kites(62000));        // the retry has run out: the spell is tried again from range
}

// A party member does not take up its leader's target when that creature is far above its level (out of combat, no route); a creature already in a fight is helped.
void AMemberDoesNotAssistOnACreatureFarAboveItsLevel()
{
    auto const attacks = [](std::uint32_t maxAbove, std::uint64_t victim)
    {
        PolicyOptions options = DeadOptions(nullptr, AllWarriors);
        options.EngageMaxLevelAbove = maxAbove;
        BotPolicy policy(options);
        WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
        NearbyUnit strong = Hostile(7, 10.0f, 100);
        strong.level = 20;
        strong.victim_ref = victim;
        for (BotSnapshot& bot : v.bots)
            bot.level = 5;
        v.bots[0].threat_target_guid = strong.guid;        // the leader (10, all warriors) has it as its target, not in combat yet
        for (std::size_t i = 0; i < v.bots.size(); ++i)
            AddNearby(v, i, strong);
        std::vector<BotAction> const acts = policy.Decide(v, 1000);
        return HasAction(acts, 13, ActionType::kAttackTarget, strong.guid) || HasAction(acts, 13, ActionType::kFollow, strong.guid);
    };
    CHECK(!attacks(5, 0));                               // 15 levels above: left alone
    CHECK(attacks(0, 0));                                // the guard off
    CHECK(attacks(5, MakeUnitRef(2, 0, 10)));            // it attacks the leader already: the member helps
}

// The level-gap rule: more than EngageMaxLevelAbove levels above the bot; an elite only for a solo bot, unless EngageEliteSolo.
void TheLevelGapRule()
{
    BotSnapshot bot;
    bot.level = 10;
    NearbyUnit elite = Hostile(7, 10.0f, 100);
    elite.level = 10;
    elite.elite = 1;
    CHECK(bot_policy_detail::EngageTooStrong(bot, elite, 5, true, false));
    CHECK(!bot_policy_detail::EngageTooStrong(bot, elite, 5, true, true));
    CHECK(!bot_policy_detail::EngageTooStrong(bot, elite, 5, false, false));   // in a party
    NearbyUnit high = Hostile(8, 10.0f, 100);
    high.level = 16;
    CHECK(bot_policy_detail::EngageTooStrong(bot, high, 5, false, false));
    high.level = 15;
    CHECK(!bot_policy_detail::EngageTooStrong(bot, high, 5, false, false));
    CHECK(!bot_policy_detail::EngageTooStrong(bot, high, 0, true, false));
}

// A ghost with no corpse in the snapshot used to stand for good (corpse_run_failed reason=no_corpse). GhostFallbackMs later it takes the spirit healer; a ghost that
// wandered from the graveyard is put back there first. A corpse that shows up again restarts the run.
void AGhostWithNoCorpseTakesTheSpiritHealer()
{
    std::vector<std::string> events;
    PolicyOptions options = AggressiveOptions();
    options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
    BotPolicy policy(options);
    auto const ghost = [](float x, bool corpse)
    {
        WorldTickSnapshotView v = View(1, 0, {});
        Ghost(v.bots[0], x, 0, 300.0f);
        v.bots[0].corpse_valid = corpse ? 1 : 0;
        return v;
    };
    CHECK(!AskedSpiritHealer(policy.Decide(ghost(0.0f, false), 1000), kBot));
    CHECK(Logged(events, "corpse_run_failed:"));
    CHECK(!AskedSpiritHealer(policy.Decide(ghost(0.0f, false), 20000), kBot));     // 19 s: still waiting
    CHECK(AskedSpiritHealer(policy.Decide(ghost(0.0f, false), 32000), kBot));      // 31 s
    CHECK(Logged(events, "ghost_fallback:"));
    // Walked 200 yards off (the core moved it): back to the graveyard first.
    std::optional<BotAction> const back = TeleportOf(policy.Decide(ghost(200.0f, false), 36000), kBot);
    CHECK(back.has_value() && back->dest_x == 0.0f);

    // The corpse comes back before the fallback: the run starts again.
    std::vector<std::string> events2;
    PolicyOptions options2 = AggressiveOptions();
    options2.OnEvent = [&events2](std::string const& text) { events2.push_back(text); };
    BotPolicy again(options2);
    again.Decide(ghost(0.0f, false), 1000);
    std::vector<BotAction> const resumed = again.Decide(ghost(0.0f, true), 5000);
    CHECK(Logged(events2, "corpse_run_resume:"));
    CHECK(MovesTo(resumed, kBot, 300.0f));

    // GhostFallbackMs = 0: the old behaviour, the ghost stands.
    PolicyOptions off = AggressiveOptions();
    off.GhostFallbackMs = 0;
    off.GhostMaxMs = 0;
    BotPolicy never(off);
    never.Decide(ghost(0.0f, false), 1000);
    CHECK(!AskedSpiritHealer(never.Decide(ghost(0.0f, false), 700000), kBot));
}

// A refused spirit healer (an older core, the key off) logs ghost_stuck and does not ask again for a while.
void ARefusedSpiritHealerLogsGhostStuck()
{
    std::vector<std::string> events;
    PolicyOptions options = AggressiveOptions();
    options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
    BotPolicy policy(options);
    auto ghost = [](std::uint32_t seq, BotActionStatus status)
    {
        WorldTickSnapshotView v = View(1, 0, {});
        Ghost(v.bots[0], 0.0f, 0, 300.0f);
        v.bots[0].corpse_valid = 0;
        v.bots[0].last_action_type = std::uint8_t(ActionType::kResurrect);
        v.bots[0].last_action_spell = 2;
        v.bots[0].last_action_sequence = seq;
        v.bots[0].last_action_status = status;
        return v;
    };
    policy.Decide(ghost(0, BotActionStatus::kNone), 1000);
    CHECK(AskedSpiritHealer(policy.Decide(ghost(0, BotActionStatus::kNone), 32000), kBot));
    CHECK(!AskedSpiritHealer(policy.Decide(ghost(1, BotActionStatus::kRefused), 36000), kBot));
    CHECK(Logged(events, "ghost_stuck:"));
    CHECK(!AskedSpiritHealer(policy.Decide(ghost(1, BotActionStatus::kRefused), 100000), kBot));   // within the two minutes of the refusal
    CHECK(AskedSpiritHealer(policy.Decide(ghost(1, BotActionStatus::kRefused), 160000), kBot));    // after them: asked again
}

// A ghost outside a dungeon party is never a ghost longer than GhostMaxMs: then it takes the spirit healer, even with its corpse run still going on.
void AGhostIsNeverAGhostLongerThanGhostMax()
{
    PolicyOptions options = AggressiveOptions();
    options.CorpseRunTeleportMs = 0;
    options.CorpseRunTimeoutMs = 36000000;
    options.GhostMaxMs = 600000;
    BotPolicy policy(options);
    auto const ghost = []
    {
        WorldTickSnapshotView v = View(1, 0, {});
        Ghost(v.bots[0], 0.0f, 0, 3000.0f);   // a corpse it never reaches
        return v;
    };
    CHECK(!AskedSpiritHealer(policy.Decide(ghost(), 1000), kBot));
    CHECK(!AskedSpiritHealer(policy.Decide(ghost(), 590000), kBot));
    CHECK(AskedSpiritHealer(policy.Decide(ghost(), 602000), kBot));
}

// Three deaths at one spot within the window: the bot avoids the area. Its ghost takes the spirit healer at once (no reclaim beside its killer), and alive inside the
// area it is put at a grind spot for its level away from it, with a walk there in case the teleport is refused.
void ThreeDeathsAtOneSpotMakeTheBotAvoidIt()
{
    std::vector<std::string> events;
    PolicyOptions options = AggressiveOptions();
    options.Grind = FarSpot();
    options.OnEvent = [&events](std::string const& text) { events.push_back(text); };
    BotPolicy policy(options);
    std::uint64_t now = 1000;
    for (int death = 0; death < 3; ++death)
    {
        WorldTickSnapshotView dead = View(1, 0, {});
        dead.bots[0].level = 6;
        Corpse(dead.bots[0], 10.0f + float(death));
        policy.Decide(dead, now);
        now += 200000;   // far enough apart that the death loop escape (three deaths in seven minutes) does not fire
        if (death < 2)
        {
            CHECK(!Logged(events, "death_area:"));
            WorldTickSnapshotView up = View(1, 0, {});
            up.bots[0].level = 6;
            up.bots[0].pos_x = 10.0f;
            policy.Decide(up, now);
            now += 1000;
        }
    }
    CHECK(Logged(events, "death_area:"));
    // The ghost, its corpse in the area: the spirit healer, not the reclaim.
    WorldTickSnapshotView ghost = View(1, 0, {});
    ghost.bots[0].level = 6;
    Ghost(ghost.bots[0], 12.0f, 0, 12.0f);
    std::vector<BotAction> const ghostTurn = policy.Decide(ghost, now);
    CHECK(AskedSpiritHealer(ghostTurn, kBot));
    CHECK(!HasAction(ghostTurn, kBot, ActionType::kReclaimCorpse));
    // Alive inside the area: away to the far grind spot.
    now += 5000;
    WorldTickSnapshotView up = View(1, 0, {});
    up.bots[0].level = 6;
    up.bots[0].pos_x = 20.0f;
    std::optional<BotAction> const away = TeleportOf(policy.Decide(up, now), kBot);
    CHECK(away.has_value() && away->dest_x == 5000.0f);
    CHECK(Logged(events, "death_area_escape:"));
    // The teleport was refused: the bot walks there.
    WorldTickSnapshotView still = View(1, 0, {});
    still.bots[0].level = 6;
    still.bots[0].pos_x = 20.0f;
    CHECK(MovesTo(policy.Decide(still, now + 1000), kBot, 5000.0f));

    // DeathAreaCount = 0: no area.
    std::vector<std::string> offEvents;
    PolicyOptions off = AggressiveOptions();
    off.DeathAreaCount = 0;
    off.OnEvent = [&offEvents](std::string const& text) { offEvents.push_back(text); };
    BotPolicy noArea(off);
    for (int death = 0; death < 3; ++death)
    {
        WorldTickSnapshotView dead = View(1, 0, {});
        Corpse(dead.bots[0], 10.0f);
        noArea.Decide(dead, 1000 + std::uint64_t(death) * 200000);
        WorldTickSnapshotView back = View(1, 0, {});
        noArea.Decide(back, 2000 + std::uint64_t(death) * 200000);
    }
    CHECK(!Logged(offEvents, "death_area:"));
}

// ---- RaidPullSingle and RaidGhostFightHoldMaxMs (Onyxia fix 3) ----

// The pull_single row for the test creatures (HostileAt: entry 1) on the drift map 36.
std::shared_ptr<raid_rules::Table const> PullSingleRules(char const* text = "pull_single|36|1|30|test Warder\n")
{
    std::vector<std::string> errors;
    auto table = std::make_shared<raid_rules::Table>(raid_rules::Parse(text, errors));
    CHECK(errors.empty());
    return table;
}

NearbyUnit OtherEntryAt(std::uint64_t counter, float x, float y)
{
    NearbyUnit unit = HostileAt(counter, x, y);
    unit.guid = MakeUnitRef(8, 2, counter);
    return unit;
}

// The route leader (12) does not pull either of two listed creatures 15 yards apart and holds its walk (pull_single_hold); 60 s on it pulls as before. One alone is
// pulled at once; without the key, or without a row, the pair is pulled as before.
void PullSingleHoldsThePairAndPullsOneAlone()
{
    struct Result { bool pulled = false; bool walked = false; };
    auto const run = [](bool key, char const* rules, float secondX, std::uint64_t until, std::vector<std::string>* events)
    {
        PolicyOptions options = DriftOptions(0, 0, events);
        options.RaidPullSingle = key;
        options.RaidRules = PullSingleRules(rules);
        BotPolicy policy(options);
        policy.SetRouteActive(true);
        Result result;
        for (std::uint64_t now = 1000; now <= until; now += 1000)
        {
            WorldTickSnapshotView view = DriftView();
            AddNearby(view, 2, HostileAt(7, 20.0f, 0.0f));
            AddNearby(view, 2, HostileAt(8, secondX, 0.0f));
            std::vector<BotAction> const actions = policy.Decide(view, now);
            for (std::uint64_t counter : { 7u, 8u })
                result.pulled = result.pulled || HasAction(actions, 12, ActionType::kFollow, MakeUnitRef(8, 1, counter)) ||
                                HasAction(actions, 12, ActionType::kAttackTarget, MakeUnitRef(8, 1, counter));
            result.walked = result.walked || LeaderWalk(actions) == 100.0f;
        }
        return result;
    };
    char const* const row = "pull_single|36|1|30|test Warder\n";
    std::vector<std::string> events;
    Result const held = run(true, row, 35.0f, 30000, &events);
    CHECK(!held.pulled && !held.walked);
    CHECK(Logged(events, "pull_single_hold: bot=12 entry=1 apart=15"));
    CHECK(run(true, row, 35.0f, 70000, nullptr).pulled);                        // past the 60 s cap
    CHECK(run(true, row, 80.0f, 5000, nullptr).pulled);                         // 60 yards apart: one alone, pulled at once
    CHECK(run(false, row, 35.0f, 5000, nullptr).pulled);                        // key off
    CHECK(run(true, "pull_single|36|2|30|another entry\n", 35.0f, 5000, nullptr).pulled);   // no row for entry 1
    CHECK(run(true, "pull_single|249|1|30|another map\n", 35.0f, 5000, nullptr).pulled);    // no row for the map
}

// In a fight a bot with no target of its own does not pick a listed creature that fights nobody (a second Warder walking past), even when it is the weaker; it takes
// the one that fights the party. A creature of another entry, or the key off, is picked as before.
void PullSingleInAFightTakesOnlyWhatFights()
{
    auto const picks = [](bool key, NearbyUnit idle)
    {
        PolicyOptions options = DriftOptions(0, 0);
        options.RaidPullSingle = key;
        options.RaidRules = PullSingleRules();
        BotPolicy policy(options);
        policy.SetRouteActive(true);
        policy.Decide(DriftView(), 1000);
        WorldTickSnapshotView view = DriftView();
        view.bots[0].in_combat = 1;
        view.bots[2].in_combat = 1;
        idle.health = 15;                                                         // hurt: the weakest pick, but it fights nobody
        NearbyUnit fighting = HostileAt(8, 3.0f, 0.0f);
        fighting.victim_ref = PlayerOf(12);
        AddNearby(view, 0, idle);
        AddNearby(view, 0, fighting);
        std::vector<BotAction> const actions = policy.Decide(view, 1500);
        return HasAction(actions, 10, ActionType::kFollow, idle.guid) || HasAction(actions, 10, ActionType::kAttackTarget, idle.guid);
    };
    CHECK(picks(false, HostileAt(7, 6.0f, 0.0f)));
    CHECK(!picks(true, HostileAt(7, 6.0f, 0.0f)));
    CHECK(picks(true, OtherEntryAt(7, 6.0f, 0.0f)));                              // entry 2 has no row
}

// RaidGhostFightHoldMaxMs: on a map with a pull_single row a ghost of a fighting party is not put at the entrance (held, reason party_fight), and goes to its leader
// once the fight is over; at most the key's time per death. Without the row or with the key at 0 it goes to the entrance during the fight as before.
void AGhostWaitsOutThePartysFightOnAPullSingleMap()
{
    auto const build = [](bool fighting)
    {
        WorldTickSnapshotView v = PartyView({ 10, 11, 12, 13, 14 });
        for (BotSnapshot& bot : v.bots)
        {
            bot.map_id = 36;
            bot.pos_x = 200.0f;
        }
        Ghost(v.bots[3], 0.0f, 36, 190.0f);
        v.bots[3].map_id = 0;                                                      // the ghost is outside, at the graveyard
        v.bots[1].in_combat = fighting ? 1 : 0;
        return v;
    };
    auto const optionsOf = [](std::vector<std::string>* events, std::uint64_t maxMs, char const* rules)
    {
        PolicyOptions options = DeadOptions(events, AllWarriors);
        options.RaidGhostFightHoldMaxMs = maxMs;
        options.RaidRules = PullSingleRules(rules);
        PolicyOptions::DungeonRoute dungeon;
        dungeon.HasEntry = true;
        dungeon.Entry = Point{ 0.0f, 0.0f, 0.0f };
        options.DungeonsByMap[36] = dungeon;
        return options;
    };
    char const* const row = "pull_single|36|1|30|test Warder\n";
    std::vector<std::string> events;
    BotPolicy policy(optionsOf(&events, 300000, row));
    policy.SetRouteActive(true);
    CHECK(!TeleportOf(policy.Decide(build(true), 1000), 13).has_value());
    CHECK(Logged(events, "ghost_teleport_hold: bot=13 reason=party_fight dest=entrance map=36"));
    CHECK(!TeleportOf(policy.Decide(build(true), 200000), 13).has_value());      // past the entrance wait: still held, no corpse run either
    std::optional<BotAction> const quiet = TeleportOf(policy.Decide(build(false), 201000), 13);
    CHECK(quiet.has_value() && quiet->spell_id == 36 && quiet->dest_x == 200.0f);  // the fight is over: to the leader
    // the maximum: 300 s after the first held decision the ghost is no longer held; the entrance wait is long over, so it starts its corpse run as before
    std::vector<std::string> expiryEvents;
    BotPolicy expiring(optionsOf(&expiryEvents, 300000, row));
    expiring.SetRouteActive(true);
    CHECK(!TeleportOf(expiring.Decide(build(true), 1000), 13).has_value());
    CHECK(!Logged(expiryEvents, "corpse_run_start: bot=13"));
    expiring.Decide(build(true), 302000);
    CHECK(Logged(expiryEvents, "corpse_run_start: bot=13"));
    // no pull_single row for the map, or the key at 0: the entrance teleport during the fight, as before
    BotPolicy otherMap(optionsOf(nullptr, 300000, "pull_single|249|1|30|another map\n"));
    otherMap.SetRouteActive(true);
    std::optional<BotAction> const entrance = TeleportOf(otherMap.Decide(build(true), 1000), 13);
    CHECK(entrance.has_value() && entrance->dest_x == 0.0f);
    BotPolicy off(optionsOf(nullptr, 0, row));
    off.SetRouteActive(true);
    CHECK(TeleportOf(off.Decide(build(true), 1000), 13).has_value());
// Personality (personality.h): off by default, a solo pull is varied by a reaction pause, a pack cap for wary bots, and a small mistake.
PolicyOptions PersonalityOptions(std::uint64_t seed)
{
    PolicyOptions options = AggressiveOptions();
    options.RandomSeed = seed;
    options.Personality.Enabled = true;
    options.Personality.ReactionMaxMs = 0;
    options.Personality.CautionStrength = 0;
    options.Personality.MistakeMaxPct = 0;
    return options;
}

bool Pulls(std::vector<BotAction> const& actions, std::uint64_t unit)
{
    return HasAction(actions, 10, ActionType::kAttackTarget, unit) || HasAction(actions, 10, ActionType::kFollow, unit);
}

void PersonalityIsOffByDefault()
{
    PolicyOptions options = AggressiveOptions();
    options.Personality.ReactionMaxMs = 10000;
    options.Personality.CautionStrength = 100;
    options.Personality.MistakeMaxPct = 30;
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, Hostile(7, 3.0f, 100));
    AddNearby(view, 0, Hostile(8, 3.5f, 100));
    AddNearby(view, 0, Hostile(9, 4.0f, 100));
    BotPolicy policy(options);
    CHECK(Pulls(policy.Decide(view, 0), MakeUnitRef(8, 1, 7)));
}

void ASlowBotPausesBeforeAPullThenPulls()
{
    std::uint64_t seed = 1;
    std::uint64_t delay = 0;
    for (; seed < 5000; ++seed)
    {
        personality::Settings s;
        s.Enabled = true;
        s.ReactionMaxMs = 10000;
        delay = personality::ReactionDelayMs(s, personality::TraitsFor(seed, 10), seed, 10, MakeUnitRef(8, 1, 7));
        if (delay >= 3000)
            break;
    }
    CHECK(delay >= 3000);
    PolicyOptions options = PersonalityOptions(seed);
    options.Personality.ReactionMaxMs = 10000;
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, Hostile(7, 3.0f, 100));
    CHECK(!Pulls(policy.Decide(view, 1000), MakeUnitRef(8, 1, 7)));
    CHECK(!Pulls(policy.Decide(view, 1000 + delay - 500), MakeUnitRef(8, 1, 7)));
    CHECK(Pulls(policy.Decide(view, 1000 + delay + 1), MakeUnitRef(8, 1, 7)));
}

void AWaryBotLeavesAFullPackAtMostThreeTimesThenPulls()
{
    std::uint64_t seed = 1;
    for (; seed < 5000; ++seed)
        if (personality::TraitsFor(seed, 10).Caution >= 90)
            break;
    PolicyOptions options = PersonalityOptions(seed);
    options.Personality.CautionStrength = 50;
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, Hostile(7, 3.0f, 100));
    AddNearby(view, 0, Hostile(8, 3.5f, 100));
    AddNearby(view, 0, Hostile(9, 4.0f, 100));
    bool pulled = false;
    for (std::uint64_t tick = 0; tick < 3; ++tick)
        for (std::uint64_t unit = 7; unit <= 9; ++unit)
            pulled = pulled || Pulls(policy.Decide(view, tick * 100), MakeUnitRef(8, 1, unit));
    CHECK(!pulled);
    CHECK(Pulls(policy.Decide(view, 16000), MakeUnitRef(8, 1, 7)) || Pulls(policy.Decide(view, 16100), MakeUnitRef(8, 1, 8)) ||
          Pulls(policy.Decide(view, 16200), MakeUnitRef(8, 1, 9)));
}

void ABoldBotMakingAMistakePullsAnotherMob()
{
    std::uint64_t seed = 1;
    for (; seed < 100000; ++seed)
    {
        personality::Settings s;
        s.Enabled = true;
        s.MistakeMaxPct = 30;
        if (personality::MakesMistake(s, personality::TraitsFor(seed, 10), seed, 10, MakeUnitRef(8, 1, 7)))
            break;
    }
    PolicyOptions options = PersonalityOptions(seed);
    options.Personality.MistakeMaxPct = 30;
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10 });
    AddNearby(view, 0, Hostile(7, 3.0f, 100));
    AddNearby(view, 0, Hostile(8, 4.0f, 100));
    auto const actions = policy.Decide(view, 0);
    CHECK(Pulls(actions, MakeUnitRef(8, 1, 8)));
    CHECK(!Pulls(actions, MakeUnitRef(8, 1, 7)));
}

void AMistakeNeverPicksAMobThePullGuardsRefuse()
{
    std::uint64_t seed = 1;
    for (; seed < 100000; ++seed)
    {
        personality::Settings s;
        s.Enabled = true;
        s.MistakeMaxPct = 30;
        if (personality::MakesMistake(s, personality::TraitsFor(seed, 10), seed, 10, MakeUnitRef(8, 1, 7)))
            break;
    }
    PolicyOptions options = PersonalityOptions(seed);
    options.Personality.MistakeMaxPct = 30;
    BotPolicy policy(options);
    WorldTickSnapshotView view = PartyView({ 10 });
    view.bots[0].level = 1;
    AddNearby(view, 0, Hostile(7, 3.0f, 100));
    NearbyUnit strong = Hostile(8, 4.0f, 100);
    strong.level = 9;
    AddNearby(view, 0, strong);
    auto const actions = policy.Decide(view, 0);
    CHECK(!Pulls(actions, MakeUnitRef(8, 1, 8)));
}

} // namespace

void RunPolicyTestsPart11()
{
    PullSingleHoldsThePairAndPullsOneAlone();
    PullSingleInAFightTakesOnlyWhatFights();
    AGhostWaitsOutThePartysFightOnAPullSingleMap();
    PersonalityIsOffByDefault();
    ASlowBotPausesBeforeAPullThenPulls();
    AWaryBotLeavesAFullPackAtMostThreeTimesThenPulls();
    ABoldBotMakingAMistakePullsAnotherMob();
    AMistakeNeverPicksAMobThePullGuardsRefuse();
    ADefensiveBotWalksOnPastACreatureThatAttacksNobody();
    ACasterWithNoSpellAtItsLevelMelees();
    ACasterWithNotKnownSpellStaysInMeleeUntilRetry();
    AMemberDoesNotAssistOnACreatureFarAboveItsLevel();
    TheLevelGapRule();
    AGhostWithNoCorpseTakesTheSpiritHealer();
    ARefusedSpiritHealerLogsGhostStuck();
    AGhostIsNeverAGhostLongerThanGhostMax();
    ThreeDeathsAtOneSpotMakeTheBotAvoidIt();
}
