// SPDX-License-Identifier: GPL-2.0-or-later
// Tests of the survival system (2026-11-19, branch feature/survival-20261119, doc/survival.md): the shared danger map (danger_map.h), the fight-risk estimate and steps
// (fight_risk.h), and the policy's SafeRevive, FightRiskRetreat and DangerMap parts. Positions from the sim arm C read-out (2026-11-19): the Raven Hill graveyard in
// Duskwood (-10606, 294) with the Skeletal Horror (entry 202, level ~24) and the level 8-10 bots 235 and 66 dying at (-10600, 200..360); the four Skyborne killers on
// map 2991 (251707 at 2300,1100 and 254626 at 2700,1100 for bot 43; 256092 at 3200,1100 for bot 13; 251245 has no spot in the read-out, 3090,1390 is assumed).
#include "policy_test_support.h"
#include "bot_settings.h"
#include "danger_map.h"
#include "fight_risk.h"

#include <cstdio>
#include <sstream>

namespace {

constexpr std::uint32_t kEastern = 0;
constexpr std::uint32_t kSkyborne = 2991;
constexpr float kRavenX = -10606.0f;   // the Duskwood graveyard at Raven Hill
constexpr float kRavenY = 294.0f;
constexpr float kGoldX = -9339.0f;     // a low graveyard in Elwynn (bot 66 stood at -9472,320 in the level table)
constexpr float kGoldY = 172.0f;

NearbyUnit Mob(float x, float y, std::uint32_t level, std::uint32_t entry, std::uint64_t counter = 7)
{
    NearbyUnit unit;
    unit.guid = MakeUnitRef(8, entry, counter);
    unit.pos_x = x;
    unit.pos_y = y;
    unit.health = 100;
    unit.max_health = 100;
    unit.is_hostile = true;
    unit.level = level;
    return unit;
}

WorldTickSnapshotView At(std::uint32_t map, float x, float y, std::uint32_t level, std::vector<NearbyUnit> nearby = {}, std::uint64_t threat = 0)
{
    WorldTickSnapshotView view = View(1, threat, std::move(nearby));
    view.bots[0].map_id = map;
    view.bots[0].pos_x = x;
    view.bots[0].pos_y = y;
    view.bots[0].level = level;
    return view;
}

void DieAt(BotPolicy& policy, std::uint32_t map, float x, float y, std::uint32_t level, NearbyUnit const& killer, std::uint64_t at)
{
    policy.Decide(At(map, x, y, level), at);
    WorldTickSnapshotView dead = At(map, x, y, level, { killer }, killer.guid);
    BotSnapshot& bot = dead.bots[0];
    bot.life_state = 1;
    bot.health = 0;
    bot.corpse_valid = 1;
    bot.corpse_map = map;
    bot.corpse_x = x;
    bot.corpse_y = y;
    policy.Decide(dead, at + 1000);
}

std::vector<BotAction> GhostAt(BotPolicy& policy, std::uint32_t map, float x, float y, float corpseX, float corpseY, std::uint32_t level, std::uint64_t at,
                               std::vector<NearbyUnit> nearby = {})
{
    WorldTickSnapshotView ghost = At(map, x, y, level, std::move(nearby));
    BotSnapshot& bot = ghost.bots[0];
    bot.life_state = 2;
    bot.health = 1;
    bot.corpse_valid = 1;
    bot.corpse_map = map;
    bot.corpse_x = corpseX;
    bot.corpse_y = corpseY;
    return policy.Decide(ghost, at);
}

bool Has(std::vector<BotAction> const& actions, ActionType type, std::uint32_t spell = 0xFFFFFFFFu)
{
    for (BotAction const& action : actions)
        if (action.type == type && (spell == 0xFFFFFFFFu || action.spell_id == spell))
            return true;
    return false;
}

std::size_t Count(std::vector<std::string> const& events, char const* text)
{
    std::size_t n = 0;
    for (std::string const& event : events)
        n += event.find(text) != std::string::npos ? 1 : 0;
    return n;
}

PolicyOptions SurvivalOptions(std::vector<std::string>* events)
{
    PolicyOptions options = AggressiveOptions();
    options.WanderWhenIdle = false;
    options.SideOf = [](std::uint64_t) -> std::uint8_t { return 1; };
    options.OnEvent = [events](std::string const& text) { events->push_back(text); };
    options.DeathAreaCount = 0;   // the death area's own escapes and walk blocks off: the survival parts alone
    return options;
}

// ---- the danger map (pure) ----

void BandsAreFiveLevelsWide()
{
    CHECK(danger_map::BandOf(0) == 0 && danger_map::BandOf(1) == 0 && danger_map::BandOf(5) == 0);
    CHECK(danger_map::BandOf(6) == 1 && danger_map::BandOf(10) == 1 && danger_map::BandOf(11) == 2 && danger_map::BandOf(60) == 11);
    CHECK(danger_map::BandName(1) == "6-10" && danger_map::BandName(0) == "1-5");
}

// Bots 235 and 66 at Raven Hill: level 8 deaths to entry 202. The sixth fresh death makes the cell dangerous for band 6-10 (6 per hour), once.
void RavenHillTurnsHotForItsBandAndBelow()
{
    danger_map::Map map(danger_map::Options{ true, 7200000, 6 });
    std::optional<danger_map::HotCell> hot;
    std::size_t lines = 0;
    for (int i = 0; i < 9; ++i)
    {
        std::optional<danger_map::HotCell> const now = map.NoteDeath(kEastern, -10600.0f, 290.0f + float(i % 3) * 5.0f, 8, 202, 1000 + std::uint64_t(i) * 30000);
        if (now)
        {
            hot = now;
            ++lines;
            CHECK(i == 6);   // the seventh death (the first ones lost a little weight in the 3 minutes)
        }
    }
    CHECK(lines == 1 && hot && hot->Band == 1 && hot->TopEntry == 202 && hot->TopShare == 100 && hot->Deaths == 7);
    std::uint64_t const now = 1000 + 9 * 30000;
    CHECK(map.Dangerous(kEastern, kRavenX, kRavenY, 8, now));             // the graveyard, 10 yards off
    CHECK(map.Dangerous(kEastern, kRavenX, kRavenY, 3, now));             // a lower band: the place kills level 8, so level 3 too
    CHECK(!map.Dangerous(kEastern, kRavenX, kRavenY, 12, now));           // a higher band did not die here
    CHECK(!map.Dangerous(kEastern, kRavenX, kRavenY, 0, now));            // level unknown: never judged
    CHECK(!map.Dangerous(kEastern, -10600.0f, 500.0f, 8, now));           // 200 yards off
    CHECK(!map.Dangerous(kSkyborne, -10600.0f, 290.0f, 8, now));          // another map
    CHECK(map.TopKiller(kEastern, kRavenX, kRavenY) == 202);
    // The weight falls over the 2 h decay: after an hour about half, after two hours nothing.
    float const fresh = map.Rate(kEastern, -10600.0f, 290.0f, 8, now);
    float const later = map.Rate(kEastern, -10600.0f, 290.0f, 8, now + 3600000);
    CHECK(fresh >= 8.0f && later > fresh * 0.4f && later < fresh * 0.6f);
    CHECK(map.Rate(kEastern, -10600.0f, 290.0f, 8, now + 7200000) == 0.0f && !map.Dangerous(kEastern, kRavenX, kRavenY, 8, now + 7200000));
    std::string const fields = map.TakeFields(now);
    CHECK(fields.find(" danger_cells=") == 0 && fields.find("danger_hot=1") != std::string::npos && fields.find("danger_deaths=9") != std::string::npos);
    CHECK(fields.find("danger_top=0:-10580,300:202:") != std::string::npos);
    map.TakeFields(now + 7200000);
    CHECK(map.Cells() == 0);   // pruned once decayed
}

// The four Skyborne killers of arm C (level 9 rogues 13, 43, 190): each spot turns hot for band 6-10, only on map 2991.
void TheSkyborneKillersAreFourHotCells()
{
    danger_map::Map map(danger_map::Options{ true, 7200000, 6 });
    struct Killer
    {
        std::uint32_t Entry;
        float X, Y;
    };
    Killer const killers[] = { { 251707, 2300.0f, 1100.0f }, { 254626, 2700.0f, 1100.0f }, { 256092, 3200.0f, 1100.0f }, { 251245, 3090.0f, 1390.0f } };
    std::size_t hotLines = 0;
    std::uint64_t at = 1000;
    for (Killer const& killer : killers)
        for (int i = 0; i < 7; ++i)
            hotLines += map.NoteDeath(kSkyborne, killer.X + float(i), killer.Y, 9, killer.Entry, at += 20000) ? 1 : 0;
    CHECK(hotLines == 4);
    for (Killer const& killer : killers)
    {
        CHECK(map.Dangerous(kSkyborne, killer.X, killer.Y, 9, at) && map.Dangerous(kSkyborne, killer.X, killer.Y, 4, at));
        CHECK(!map.Dangerous(kSkyborne, killer.X, killer.Y, 13, at) && !map.Dangerous(kEastern, killer.X, killer.Y, 9, at));
        CHECK(map.TopKiller(kSkyborne, killer.X, killer.Y) == killer.Entry);
    }
    // A walk along y = 1100 from x = 2000: the first dangerous point is at the 251707 spot; the first 60 yards are not judged (the bot may walk out).
    auto const hit = map.FirstDangerous(kSkyborne, 2000.0f, 1100.0f, 3300.0f, 1100.0f, 9, at, 60.0f);
    CHECK(hit && hit->X >= 2240.0f && hit->X <= 2300.0f);
    CHECK(!map.FirstDangerous(kSkyborne, 2300.0f, 1100.0f, 2300.0f, 1150.0f, 9, at, 60.0f));       // a walk inside the first 60 yards
    CHECK(!map.FirstDangerous(kSkyborne, 2000.0f, 1700.0f, 3300.0f, 1700.0f, 9, at, 60.0f));       // a walk well north of them
    map.NoteSkip("grind");
    map.NoteSkip("grind");
    map.NoteSkip("quest");
    std::string const fields = map.TakeFields(at);
    CHECK(fields.find("danger_hot=4") != std::string::npos && fields.find("danger_skips=grind:2,quest:1") != std::string::npos);
    CHECK(fields.find(":251707:") != std::string::npos || fields.find(":254626:") != std::string::npos || fields.find(":256092:") != std::string::npos ||
          fields.find(":251245:") != std::string::npos);
    CHECK(map.TakeFields(at).find("danger_skips=none") != std::string::npos);   // the skip counts start again
}

void TheDangerMapStaysBounded()
{
    danger_map::Map map(danger_map::Options{ true, 7200000, 6 });
    for (int i = 0; i < 100; ++i)
        map.NoteDeath(kEastern, 0.0f, 0.0f, 8, 202, 1000 + std::uint64_t(i));
    CHECK(map.Cells() == 1 && map.Rate(kEastern, 0.0f, 0.0f, 8, 2000) <= float(danger_map::kDeathsPerCell) + 0.1f);
    danger_map::Map off(danger_map::Options{ true, 7200000, 0 });
    for (int i = 0; i < 10; ++i)
        CHECK(!off.NoteDeath(kEastern, 0.0f, 0.0f, 8, 202, 1000));
    CHECK(!off.Dangerous(kEastern, 0.0f, 0.0f, 8, 1000));                   // threshold 0: nothing is ever dangerous
}

// ---- the fight-risk estimate and steps (pure) ----

void TheEstimateComparesTimeToDieWithTimeToKill()
{
    fight_risk::Options options;
    options.Enabled = true;
    // Losing: the bot loses 50 health a second, its target 5.
    fight_risk::Watch losing;
    for (std::uint64_t s = 0; s <= 8; ++s)
        losing.Note(10000 + s * 1000, 1000 - std::uint32_t(s) * 50, 77, 1000 - std::uint32_t(s) * 5);
    CHECK(!losing.Judge(options, 10000, 100, 0).Known);                     // too young a fight
    fight_risk::Estimate const lose = losing.Judge(options, 18000, 60, 0);
    CHECK(lose.Known && lose.Losing && lose.InDps > 49.0f && lose.InDps < 51.0f && lose.OutDps > 4.0f && lose.OutDps < 6.0f);
    CHECK(lose.TimeToDieS < 13.0f && lose.TimeToKillS > 150.0f);
    CHECK(!losing.Judge(options, 18000, 75, 0).Losing);                     // above 70 % health: never
    // The big hits of the damage events count when they are more than the health loss (heals hide it).
    fight_risk::Watch healed;
    for (std::uint64_t s = 0; s <= 6; ++s)
        healed.Note(10000 + s * 1000, 600, 77, 1000 - std::uint32_t(s) * 5);
    CHECK(!healed.Judge(options, 16000, 60, 0).Losing);
    CHECK(healed.Judge(options, 16000, 60, 1200).Losing && healed.Judge(options, 16000, 60, 1200).InDps == 200.0f);
    // Winning: the target loses 100 a second.
    fight_risk::Watch winning;
    for (std::uint64_t s = 0; s <= 6; ++s)
        winning.Note(10000 + s * 1000, 1000 - std::uint32_t(s) * 50, 77, 1000 - std::uint32_t(s) * 100);
    CHECK(!winning.Judge(options, 16000, 70, 0).Losing);
    // The margin: 400 % leaves earlier (a closer fight counts as lost), 50 % later.
    fight_risk::Watch close;
    for (std::uint64_t s = 0; s <= 6; ++s)
        close.Note(10000 + s * 1000, 1000 - std::uint32_t(s) * 50, 77, 1000 - std::uint32_t(s) * 60);
    CHECK(!close.Judge(options, 16000, 70, 0).Losing);                      // ttd 14 s vs ttk 11 s
    options.MarginPct = 400;
    CHECK(close.Judge(options, 16000, 70, 0).Losing);
    options.MarginPct = 50;
    CHECK(!close.Judge(options, 16000, 70, 0).Losing);
}

void TheStepsGoPotionBandageWalkTeleport()
{
    using fight_risk::Step;
    fight_risk::Facts facts;
    CHECK(fight_risk::Next(facts) == Step::kNone);
    facts.Losing = true;
    facts.HasPotion = true;
    facts.PotionReady = true;
    facts.HasBandage = true;
    CHECK(fight_risk::Next(facts) == Step::kPotion);
    facts.PotionReady = false;
    facts.HitRecently = true;
    CHECK(fight_risk::Next(facts) == Step::kDisengage);                    // a hit would break the bandage
    facts.HitRecently = false;
    CHECK(fight_risk::Next(facts) == Step::kBandage);
    facts.Bandaged = true;
    CHECK(fight_risk::Next(facts) == Step::kDisengage);
    facts.Disengaging = true;
    facts.DisengageForMs = fight_risk::kDisengageMs;
    CHECK(fight_risk::Next(facts) == Step::kDisengage);                    // the teleport is not allowed
    facts.TeleportAllowed = true;
    CHECK(fight_risk::Next(facts) == Step::kDisengage);                    // ... nor ready
    facts.TeleportReady = true;
    CHECK(fight_risk::Next(facts) == Step::kTeleport);
    facts.DisengageForMs = fight_risk::kDisengageMs - 1;
    CHECK(fight_risk::Next(facts) == Step::kDisengage);                    // not walked away long enough
    CHECK(std::string(fight_risk::StepName(Step::kTeleport)) == "teleport");
}

// ---- SafeRevive (policy) ----

// The bot learns the Elwynn graveyard first (a death there, its ghost appears at it).
void LearnGoldshire(BotPolicy& policy, std::uint32_t level)
{
    DieAt(policy, kEastern, -9400.0f, 200.0f, level, Mob(-9395.0f, 200.0f, 5, 299, 3), 1000);
    GhostAt(policy, kEastern, kGoldX, kGoldY, -9400.0f, 200.0f, level, 3000);
    policy.Decide(At(kEastern, kGoldX, kGoldY, level), 4000);
}

// Raven Hill: a level 8 ghost appears at the graveyard with the Skeletal Horror (level 24) standing 8 yards off. With SafeRevive it never reclaims or asks the spirit
// healer there; it is moved as a ghost to the Elwynn graveyard and revives there. Off: it asks the healer beside the Horror (arm C).
void RavenHillGhostRevivesElsewhere()
{
    for (bool on : { true, false })
    {
        std::vector<std::string> events;
        PolicyOptions options = SurvivalOptions(&events);
        options.SafeRevive.Enabled = on;
        options.LevelSafe.Graveyard = true;   // the graveyards are remembered (the module default)
        BotPolicy policy(options);
        LearnGoldshire(policy, 8);
        NearbyUnit const horror = Mob(-10613.0f, 297.0f, 24, 202);
        DieAt(policy, kEastern, -10600.0f, 300.0f, 8, horror, 100000);
        bool revivedBeside = false;
        bool movedAway = false;
        for (std::uint64_t at = 102000; at < 160000; at += 1000)
        {
            std::vector<BotAction> const turn = GhostAt(policy, kEastern, kRavenX, kRavenY, -10600.0f, 300.0f, 8, at, { horror });
            revivedBeside = revivedBeside || Has(turn, ActionType::kResurrect, 2) || Has(turn, ActionType::kReclaimCorpse);
            std::optional<BotAction> const teleport = TeleportOf(turn, kBot);
            movedAway = movedAway || (teleport && teleport->dest_x == kGoldX && teleport->dest_y == kGoldY && teleport->spell_id == kEastern);
        }
        CHECK(revivedBeside == !on);
        CHECK(movedAway == on);
        if (!on)
        {
            CHECK(!LoggedWith(events, "safe_revive:"));
            continue;
        }
        CHECK(LoggedWith(events, "safe_revive: bot=42 level=8 map=0 stage=held what=spirit_healer_near_corpse reason=hostile at=-10606,294 entry=202 hostile_level=24"));
        CHECK(LoggedWith(events, "safe_revive: bot=42 level=8 map=0 stage=yard from=-10606,294 to=-9339,172"));
        CHECK(policy.UnsafeYardsKnown() == 1);   // Raven Hill is unsafe for level 8 a while
        // At the Elwynn graveyard, nothing high near: the spirit healer.
        CHECK(Has(GhostAt(policy, kEastern, kGoldX, kGoldY, -10600.0f, 300.0f, 8, 161000), ActionType::kResurrect, 2));
    }
}

// A hostile only 2 levels above the ghost holds nothing (SafeReviveLevelMargin 3).
void ANearLevelHostileHoldsNothing()
{
    std::vector<std::string> events;
    PolicyOptions options = SurvivalOptions(&events);
    options.SafeRevive.Enabled = true;
    BotPolicy policy(options);
    NearbyUnit const near = Mob(-10613.0f, 297.0f, 24, 202);
    DieAt(policy, kEastern, -10600.0f, 300.0f, 22, near, 100000);
    bool asked = false;
    for (std::uint64_t at = 102000; at < 160000 && !asked; at += 1000)
    {
        std::vector<BotAction> const turn = GhostAt(policy, kEastern, kRavenX, kRavenY, -10600.0f, 300.0f, 22, at, { near });
        asked = Has(turn, ActionType::kResurrect, 2) || Has(turn, ActionType::kReclaimCorpse);
    }
    CHECK(asked && !LoggedWith(events, "safe_revive:"));
}

// No other graveyard known: the ghost waits (no healer, no reclaim) and gives up after 10 minutes rather than staying a ghost for good.
void WithNoOtherGraveyardTheGhostWaitsThenGivesUp()
{
    std::vector<std::string> events;
    PolicyOptions options = SurvivalOptions(&events);
    options.SafeRevive.Enabled = true;
    BotPolicy policy(options);
    NearbyUnit const horror = Mob(-10613.0f, 297.0f, 24, 202);
    DieAt(policy, kEastern, -10600.0f, 300.0f, 8, horror, 100000);
    bool revived = false;
    std::uint64_t at = 102000;
    for (; at < 102000 + 700000 && !revived; at += 5000)
        revived = Has(GhostAt(policy, kEastern, kRavenX, kRavenY, -10600.0f, 300.0f, 8, at, { horror }), ActionType::kResurrect, 2);
    CHECK(LoggedWith(events, "safe_revive: bot=42 level=8 map=0 stage=wait"));
    CHECK(revived && LoggedWith(events, "stage=give_up"));
    CHECK(at > 102000 + 600000);   // not before the 10 minute wait ran out
}

// The danger map holds a revive too: seven level 8 deaths to the Horror make Raven Hill dangerous for band 6-10 (a danger_cell line), so a later ghost there with no
// hostile in view is sent to the Elwynn graveyard all the same.
void ADangerousGraveyardIsPassedOver()
{
    std::vector<std::string> events;
    PolicyOptions options = SurvivalOptions(&events);
    options.SafeRevive.Enabled = true;
    options.LevelSafe.Graveyard = true;
    BotPolicy policy(options);
    LearnGoldshire(policy, 8);
    NearbyUnit const horror = Mob(-10613.0f, 297.0f, 24, 202);
    for (std::uint64_t i = 0; i < 7; ++i)
        DieAt(policy, kEastern, -10600.0f, 290.0f, 8, horror, 100000 + i * 20000);
    CHECK(Count(events, "danger_cell: map=0 cell=-10580,300 band=6-10") == 1 && LoggedWith(events, "top_entry=202 top_share=100"));
    bool movedAway = false;
    bool asked = false;
    for (std::uint64_t at = 300000; at < 330000; at += 1000)
    {
        std::vector<BotAction> const turn = GhostAt(policy, kEastern, kRavenX, kRavenY, -10600.0f, 290.0f, 8, at);
        std::optional<BotAction> const teleport = TeleportOf(turn, kBot);
        movedAway = movedAway || (teleport && teleport->dest_x == kGoldX);
        asked = asked || (Has(turn, ActionType::kResurrect, 2) && !movedAway);
    }
    // Held at the corpse first (the reclaim), then sent to the Elwynn graveyard.
    CHECK(LoggedWith(events, "safe_revive: bot=42 level=8 map=0 stage=held what=reclaim reason=danger at=-10600,290 deaths_h="));
    CHECK(LoggedWith(events, "safe_revive: bot=42 level=8 map=0 stage=yard from=-10600,290 to=-9339,172"));
    CHECK(movedAway && !asked);
}

// A ghost at its corpse with a high hostile beside it never reclaims: it turns to the spirit healer of a safe graveyard (ghost_fallback reason=safe_revive).
void AHeldReclaimTurnsToTheGraveyard()
{
    std::vector<std::string> events;
    PolicyOptions options = SurvivalOptions(&events);
    options.SafeRevive.Enabled = true;
    options.SpiritHealerAfterMs = 0;        // the healer only through SafeRevive here
    options.SpiritHealerOnRepeat = false;
    BotPolicy policy(options);
    NearbyUnit const horror = Mob(-10595.0f, 300.0f, 24, 202);
    DieAt(policy, kEastern, -10600.0f, 300.0f, 8, horror, 100000);
    bool reclaimed = false;
    for (std::uint64_t at = 102000; at < 102000 + 200000; at += 1000)
        reclaimed = reclaimed || Has(GhostAt(policy, kEastern, -10600.0f, 300.0f, -10600.0f, 300.0f, 8, at, { horror }), ActionType::kReclaimCorpse);
    CHECK(!reclaimed);
    CHECK(LoggedWith(events, "stage=held what=reclaim reason=hostile") && LoggedWith(events, "ghost_fallback: bot=42 reason=safe_revive"));
}

// ---- DangerMap (policy) ----

std::shared_ptr<GrindData> LineSpots()
{
    std::vector<std::string> warnings;
    std::istringstream spots("spot 0 0 0 0 1 6 0 10\n"
                             "spot 0 100 0 0 2 6 0 10\n"
                             "spot 0 300 0 0 1 6 0 10\n"
                             "spot 0 400 0 0 3 6 0 10\n"
                             "spot 0 2000 0 0 24 26 0 10\n"
                             "spot 0 2100 0 0 23 27 0 10\n");
    return std::make_shared<GrindData>(ParseGrindData(spots, warnings));
}

// The level-safe escape takes the nearest grind spot for the bot's level (x = 400); with DangerMap on and bots of its band dying there, the next one (x = 300).
void ADangerousGrindSpotIsSkipped()
{
    for (bool on : { true, false })
    {
        std::vector<std::string> events;
        PolicyOptions options = SurvivalOptions(&events);
        options.Grind = LineSpots();
        options.LevelSafe.Escape = true;
        options.DangerMap.Enabled = on;
        options.DeathsTopEveryMs = 60000;
        BotPolicy policy(options);
        policy.TakeDeathsTopLine(1000);
        for (std::uint64_t i = 0; i < 7; ++i)
            DieAt(policy, kEastern, 400.0f, 0.0f, 6, Mob(405.0f, 0.0f, 9, 1388, 10 + i), 2000 + i * 5000);
        // Revived and carried off to the high zone; the level-safe escape comes once the death loop escape's cooldown is over.
        for (std::uint64_t at = 40000; at < 400000 && !LoggedWith(events, "level_unsafe_escape: bot=42 level=6 map=0 from=2000,0"); at += 1000)
            policy.Decide(At(kEastern, 2000.0f, 0.0f, 6), at);
        CHECK(LoggedWith(events, on ? "place_min=23 spot=2 to=300,0" : "place_min=23 spot=3 to=400,0"));
        std::optional<std::string> const line = policy.TakeDeathsTopLine(400000);
        CHECK(line && (line->find("danger_skips=grind:") != std::string::npos) == on);
        CHECK(LoggedWith(events, "danger_cell: map=0 cell=420,20 band=6-10") == on);
    }
}

constexpr char kQuests[] = "start test\n"
                           "anchor 0 100 0 0\n"
                           "quest 2 giver 300 0 0 ender 300 0 0\n";

void AQuestWalkIntoADangerousCellIsDropped()
{
    for (bool on : { true, false })
    {
        std::vector<std::string> events;
        PolicyOptions options = SurvivalOptions(&events);
        std::istringstream in(kQuests);
        std::vector<std::string> warnings;
        options.Quests = std::make_shared<QuestData>(ParseQuestData(in, warnings));
        options.DangerMap.Enabled = on;
        options.SafeRevive.Enabled = true;   // the map is kept either way; only DangerMap skips
        BotPolicy policy(options);
        for (std::uint64_t i = 0; i < 7; ++i)
            DieAt(policy, kEastern, 300.0f, 0.0f, 6, Mob(305.0f, 0.0f, 9, 1388, 10 + i), 2000 + i * 5000);
        bool walked = false;
        for (std::uint64_t at = 40000; at < 48000; at += 1000)
            for (BotAction const& action : policy.Decide(At(kEastern, 0.0f, 0.0f, 6), at))
                walked = walked || (action.type == ActionType::kMoveTo && action.dest_x > 250.0f);
        CHECK(walked == !on);
        CHECK(LoggedWith(events, "quest_walk_blocked: bot=42 map=0 quest=2 reason=danger kind=giver level=6") == on);
    }
}

// ---- FightRiskRetreat (policy) ----

std::shared_ptr<GrindData> SkySpots()
{
    std::vector<std::string> warnings;
    std::istringstream spots("spot 2991 3000 1100 0 8 10 0 10\n"
                             "spot 2991 3200 1500 0 8 10 0 10\n"
                             "spot 0 3000 1250 0 8 10 0 10\n");   // another map at the same place: never taken
    return std::make_shared<GrindData>(ParseGrindData(spots, warnings));
}

struct FightRun
{
    std::vector<std::vector<BotAction>> Turns;   // one per second, from 10 s
};

// A level 9 Skyborne bot (map 2991) stood safe at (3000, 1050), then fights 251707 at (3010, 1250): it loses 50 health a second, the target 5.
FightRun Fight(BotPolicy& policy, std::uint64_t groupLeader = 0, std::uint32_t potions = 2, std::uint64_t untilMs = 27000, std::uint32_t targetLoss = 5)
{
    WorldTickSnapshotView safe = At(kSkyborne, 3000.0f, 1050.0f, 9);
    safe.bots[0].group_leader_guid = groupLeader;
    policy.Decide(safe, 1000);
    FightRun run;
    NearbyUnit mob = Mob(3015.0f, 1250.0f, 12, 251707);
    mob.health = 1000;
    mob.max_health = 1000;
    for (std::uint64_t at = 10000; at <= untilMs; at += 1000)
    {
        std::uint32_t const s = std::uint32_t((at - 10000) / 1000);
        mob.health = 1000 - s * targetLoss;
        WorldTickSnapshotView view = At(kSkyborne, 3010.0f, 1250.0f, 9, { mob }, mob.guid);
        BotSnapshot& bot = view.bots[0];
        bot.in_combat = 1;
        bot.health = 1000 - s * 50;
        bot.group_leader_guid = groupLeader;
        bot.health_potion_count = potions;
        run.Turns.push_back(policy.Decide(view, at));
    }
    return run;
}

void ALosingSoloBotDrinksWalksAwayThenTeleports()
{
    std::vector<std::string> events;
    PolicyOptions options = SurvivalOptions(&events);
    options.Grind = SkySpots();
    options.FightRisk.Enabled = true;
    options.FightRisk.Teleport = true;
    BotPolicy policy(options);
    FightRun const run = Fight(policy);
    // 70 % health at 16 s: the potion; at 17 s the walk away to the safe point; 8 s later the rescue teleport, on map 2991.
    CHECK(Has(run.Turns[6], ActionType::kConsume, 3) && !Has(run.Turns[5], ActionType::kConsume, 3));
    CHECK(LoggedWith(events, "fight_retreat: bot=42 level=9 map=2991 stage=potion hp=70 in_dps=50 out_dps=5"));
    bool walked = false;
    for (BotAction const& action : run.Turns[7])
        walked = walked || (action.type == ActionType::kMoveTo && action.dest_x == 3000.0f && action.dest_y == 1050.0f);
    CHECK(walked);
    CHECK(LoggedWith(events, "stage=disengage") && LoggedWith(events, "to=3000,1050 safe_point=1"));
    std::optional<BotAction> teleport;
    std::size_t teleportAt = 0;
    for (std::size_t i = 0; i < run.Turns.size() && !teleport; ++i)
        if ((teleport = TeleportOf(run.Turns[i], kBot)))
            teleportAt = i;
    CHECK(teleport && teleportAt == 15 && teleport->spell_id == kSkyborne && teleport->dest_x == 3000.0f && teleport->dest_y == 1100.0f);
    CHECK(LoggedWith(events, "stage=teleport") && LoggedWith(events, "spot=0 to=3000,1100 dist=150"));
    CHECK(Count(events, "stage=teleport hp") == 1);
    // The core refused it (Bot.Actions.TeleportInCombat off): logged once, the walk goes on.
    NearbyUnit mob = Mob(3015.0f, 1250.0f, 12, 251707);
    mob.health = 900;
    mob.max_health = 1000;
    WorldTickSnapshotView refused = At(kSkyborne, 3010.0f, 1250.0f, 9, { mob }, mob.guid);
    refused.bots[0].in_combat = 1;
    refused.bots[0].health = 140;
    refused.bots[0].last_action_type = std::uint8_t(ActionType::kTeleport);
    refused.bots[0].last_action_status = BotActionStatus::kRefused;
    refused.bots[0].last_action_sequence = 99;
    policy.Decide(refused, 28000);
    policy.Decide(refused, 29000);
    CHECK(Count(events, "stage=teleport_refused") == 1);
    // Out of combat: the retreat ends.
    policy.Decide(At(kSkyborne, 3000.0f, 1100.0f, 9), 30000);
    CHECK(LoggedWith(events, "stage=end") && LoggedWith(events, "result=out_of_combat last=teleport"));
}

void NoTeleportWithoutTheKeyNorInAGroupNorWhenWinning()
{
    {
        std::vector<std::string> events;
        PolicyOptions options = SurvivalOptions(&events);
        options.Grind = SkySpots();
        options.FightRisk.Enabled = true;   // Teleport off
        BotPolicy policy(options);
        FightRun const run = Fight(policy, 0, 0);
        bool teleported = false;
        for (auto const& turn : run.Turns)
            teleported = teleported || TeleportOf(turn, kBot).has_value();
        CHECK(!teleported && LoggedWith(events, "stage=disengage") && !LoggedWith(events, "stage=potion"));   // no potion carried
    }
    {
        std::vector<std::string> events;
        PolicyOptions options = SurvivalOptions(&events);
        options.Grind = SkySpots();
        options.FightRisk.Enabled = true;
        options.FightRisk.Teleport = true;
        BotPolicy policy(options);
        Fight(policy, 77);   // in a group: never
        CHECK(!LoggedWith(events, "fight_retreat:"));
    }
    {
        std::vector<std::string> events;
        PolicyOptions options = SurvivalOptions(&events);
        options.Grind = SkySpots();
        options.FightRisk.Enabled = true;
        options.FightRisk.Teleport = true;
        BotPolicy policy(options);
        Fight(policy, 0, 2, 19000, 100);   // the target loses 100 a second: winning
        CHECK(!LoggedWith(events, "fight_retreat:"));
    }
    {
        std::vector<std::string> events;
        PolicyOptions options = SurvivalOptions(&events);
        options.Grind = SkySpots();
        BotPolicy policy(options);   // the key off: today's behaviour
        Fight(policy);
        CHECK(!LoggedWith(events, "fight_retreat:"));
    }
}

// A bot that dies while retreating says so (the sim counts saved vs not).
void ADeathDuringARetreatIsLogged()
{
    std::vector<std::string> events;
    PolicyOptions options = SurvivalOptions(&events);
    options.Grind = SkySpots();
    options.FightRisk.Enabled = true;
    BotPolicy policy(options);
    Fight(policy, 0, 0, 20000);
    WorldTickSnapshotView dead = At(kSkyborne, 3010.0f, 1250.0f, 9);
    dead.bots[0].life_state = 1;
    dead.bots[0].health = 0;
    dead.bots[0].corpse_valid = 1;
    dead.bots[0].corpse_map = kSkyborne;
    policy.Decide(dead, 21000);
    CHECK(LoggedWith(events, "fight_retreat: bot=42 level=9 map=2991 stage=died last=disengage entry=251707"));
}

void TheKeysAreOffByDefault()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(!defaults.SafeRevive && defaults.SafeReviveLevelMargin == 3 && !defaults.FightRiskRetreat && defaults.FightRiskMarginPct == 100 && !defaults.FightRiskTeleport);
    CHECK(!defaults.DangerMap && defaults.DangerMapDeathsPerHour == 6 && defaults.DangerMapDecayMin == 120 && warnings.empty());
    BotSettings const set = ParseBotSettings({ { "SafeRevive", "1" }, { "SafeReviveLevelMargin", "5" }, { "FightRiskRetreat", "1" }, { "FightRiskMarginPct", "150" },
                                               { "FightRiskTeleport", "1" }, { "DangerMap", "1" }, { "DangerMapDeathsPerHour", "10" }, { "DangerMapDecayMin", "60" } },
                                             warnings);
    CHECK(set.SafeRevive && set.SafeReviveLevelMargin == 5 && set.FightRiskRetreat && set.FightRiskMarginPct == 150 && set.FightRiskTeleport && set.DangerMap &&
          set.DangerMapDeathsPerHour == 10 && set.DangerMapDecayMin == 60 && warnings.empty());
    BotSettings const bad = ParseBotSettings({ { "SafeReviveLevelMargin", "0" }, { "FightRiskMarginPct", "10" }, { "DangerMapDeathsPerHour", "0" }, { "DangerMapDecayMin", "5" } },
                                             warnings);
    CHECK(bad.SafeReviveLevelMargin == 3 && bad.FightRiskMarginPct == 100 && bad.DangerMapDeathsPerHour == 6 && bad.DangerMapDecayMin == 120 && warnings.size() == 4);
    PolicyOptions const options;
    CHECK(!options.SafeRevive.Enabled && !options.FightRisk.Enabled && !options.FightRisk.Teleport && !options.DangerMap.Enabled);
}

// ---- ArmSplit (doc/sim-arm-d2.md) ----

// The death and ghost views of DieAt / GhostAt for another bot guid (43 is arm B with the split on).
WorldTickSnapshotView As(WorldTickSnapshotView view, std::uint64_t guid)
{
    view.bots[0].guid = guid;
    return view;
}

void DieAs(BotPolicy& policy, std::uint64_t guid, std::uint32_t map, float x, float y, std::uint32_t level, NearbyUnit const& killer, std::uint64_t at)
{
    policy.Decide(As(At(map, x, y, level), guid), at);
    WorldTickSnapshotView dead = As(At(map, x, y, level, { killer }, killer.guid), guid);
    BotSnapshot& bot = dead.bots[0];
    bot.life_state = 1;
    bot.health = 0;
    bot.corpse_valid = 1;
    bot.corpse_map = map;
    bot.corpse_x = x;
    bot.corpse_y = y;
    policy.Decide(dead, at + 1000);
}

std::vector<BotAction> GhostAs(BotPolicy& policy, std::uint64_t guid, std::uint32_t map, float x, float y, float corpseX, float corpseY, std::uint32_t level,
                               std::uint64_t at, std::vector<NearbyUnit> nearby = {})
{
    WorldTickSnapshotView ghost = As(At(map, x, y, level, std::move(nearby)), guid);
    BotSnapshot& bot = ghost.bots[0];
    bot.life_state = 2;
    bot.health = 1;
    bot.corpse_valid = 1;
    bot.corpse_map = map;
    bot.corpse_x = corpseX;
    bot.corpse_y = corpseY;
    return policy.Decide(ghost, at);
}

// SafeRevive only in arm B: the arm B ghost (guid 43) is held at Raven Hill and moved to Elwynn, the arm A ghost (guid 42) asks the healer beside the Horror.
void SafeReviveIsPerArm()
{
    for (std::uint64_t guid : { std::uint64_t(42), std::uint64_t(43) })
    {
        std::vector<std::string> events;
        PolicyOptions a = SurvivalOptions(&events);
        a.LevelSafe.Graveyard = true;
        PolicyOptions b = a;
        b.SafeRevive.Enabled = true;
        BotPolicy policy(a);
        policy.SetArmB(b);
        DieAs(policy, guid, kEastern, -9400.0f, 200.0f, 8, Mob(-9395.0f, 200.0f, 5, 299, 3), 1000);
        GhostAs(policy, guid, kEastern, kGoldX, kGoldY, -9400.0f, 200.0f, 8, 3000);
        policy.Decide(As(At(kEastern, kGoldX, kGoldY, 8), guid), 4000);
        NearbyUnit const horror = Mob(-10613.0f, 297.0f, 24, 202);
        DieAs(policy, guid, kEastern, -10600.0f, 300.0f, 8, horror, 100000);
        bool revivedBeside = false;
        for (std::uint64_t at = 102000; at < 160000; at += 1000)
        {
            std::vector<BotAction> const turn = GhostAs(policy, guid, kEastern, kRavenX, kRavenY, -10600.0f, 300.0f, 8, at, { horror });
            revivedBeside = revivedBeside || Has(turn, ActionType::kResurrect, 2) || Has(turn, ActionType::kReclaimCorpse);
        }
        bool const armB = guid == 43;
        CHECK(revivedBeside == !armB);
        CHECK(LoggedWith(events, "safe_revive: bot=43 level=8 map=0 stage=held") == armB);
        CHECK(!LoggedWith(events, "safe_revive: bot=42"));
    }
}

// DangerMap only in arm B: the map still takes the deaths of the arm A bots (as it would with the key on for everyone); without the split and the key off, nothing.
void TheDangerMapTakesBothArmsDeaths()
{
    for (bool split : { true, false })
    {
        std::vector<std::string> events;
        PolicyOptions a = SurvivalOptions(&events);
        BotPolicy policy(a);
        if (split)
        {
            PolicyOptions b = a;
            b.DangerMap.Enabled = true;
            b.DangerMap.DeathsPerHour = 2;
            policy.SetArmB(b);
        }
        NearbyUnit const horror = Mob(-10613.0f, 297.0f, 24, 202);
        DieAs(policy, 42, kEastern, -10600.0f, 300.0f, 8, horror, 100000);
        CHECK((policy.DangerMapView().Cells() > 0) == split);
        // Arm B alone uses the map: its DangerMapDeathsPerHour is the map's.
        CHECK(policy.DangerMapView().Settings().DeathsPerHour == (split ? 2u : 6u));
    }
}

} // namespace

int main()
{
    BandsAreFiveLevelsWide();
    RavenHillTurnsHotForItsBandAndBelow();
    TheSkyborneKillersAreFourHotCells();
    TheDangerMapStaysBounded();
    TheEstimateComparesTimeToDieWithTimeToKill();
    TheStepsGoPotionBandageWalkTeleport();
    RavenHillGhostRevivesElsewhere();
    ANearLevelHostileHoldsNothing();
    WithNoOtherGraveyardTheGhostWaitsThenGivesUp();
    ADangerousGraveyardIsPassedOver();
    AHeldReclaimTurnsToTheGraveyard();
    ADangerousGrindSpotIsSkipped();
    AQuestWalkIntoADangerousCellIsDropped();
    ALosingSoloBotDrinksWalksAwayThenTeleports();
    NoTeleportWithoutTheKeyNorInAGroupNorWhenWinning();
    ADeathDuringARetreatIsLogged();
    TheKeysAreOffByDefault();
    SafeReviveIsPerArm();
    TheDangerMapTakesBothArmsDeaths();
    if (failures == 0)
        std::printf("survival_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
