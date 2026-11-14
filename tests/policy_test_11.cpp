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
    PersonalityIsOffByDefault();
    ASlowBotPausesBeforeAPullThenPulls();
    AWaryBotLeavesAFullPackAtMostThreeTimesThenPulls();
    ABoldBotMakingAMistakePullsAnotherMob();
    AMistakeNeverPicksAMobThePullGuardsRefuse();
    ADefensiveBotWalksOnPastACreatureThatAttacksNobody();
    ACasterWithNoSpellAtItsLevelMelees();
    AMemberDoesNotAssistOnACreatureFarAboveItsLevel();
    TheLevelGapRule();
    AGhostWithNoCorpseTakesTheSpiritHealer();
    ARefusedSpiritHealerLogsGhostStuck();
    AGhostIsNeverAGhostLongerThanGhostMax();
    ThreeDeathsAtOneSpotMakeTheBotAvoidIt();
}
