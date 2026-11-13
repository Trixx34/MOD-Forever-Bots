// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for ParseBotSettings. No core needed.
#include "bot_settings.h"

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

void DefaultsWhenEmpty()
{
    std::vector<std::string> warnings;
    BotSettings const s = ParseBotSettings({}, warnings);
    CHECK(s.MaxBots == 1 && s.Race == 1 && s.Class == 1);
    CHECK(s.MountMinLevel == 40);                                  // classic riding is learned at 40
    CHECK(warnings.empty());
    std::vector<std::string> more;
    CHECK(ParseBotSettings({ { "MountMinLevel", "0" } }, more).MountMinLevel == 0);
}

void RaidRuleRowSwitches()
{
    std::vector<std::string> warnings;
    BotSettings const s = ParseBotSettings({}, warnings);
    CHECK(s.RaidSpread && s.RaidAvoidEnemy && s.RaidPurge && s.RaidMindControl);   // on by default: they act only through rows of RaidRulesFile
    BotSettings const off = ParseBotSettings({ { "RaidSpread", "0" }, { "RaidAvoidEnemy", "0" }, { "RaidPurge", "0" }, { "RaidMindControl", "0" } }, warnings);
    CHECK(!off.RaidSpread && !off.RaidAvoidEnemy && !off.RaidPurge && !off.RaidMindControl);
    CHECK(ParseBotSettings({ { "RaidPurge", "0" } }, warnings).RaidSpread);
    CHECK(warnings.empty());
}

void ReadsValidValues()
{
    std::vector<std::string> warnings;
    BotSettings const s = ParseBotSettings({ { "MaxBots", "4" }, { "Race", "2" }, { "Class", "8" } }, warnings);
    CHECK(s.MaxBots == 4 && s.Race == 2 && s.Class == 8);
    CHECK(warnings.empty());
}

void ChatSettings()
{
    std::vector<std::string> warnings;
    CHECK(ParseBotSettings({}, warnings).VendorSellGear && !ParseBotSettings({ { "VendorSellGear", "0" } }, warnings).VendorSellGear); // on by default
    // Protocol 28: the spirit healer rules (45 s, on a repeated killer) are on, the heal downrank is off
    BotSettings const spiritDefault = ParseBotSettings({}, warnings);
    CHECK(spiritDefault.SpiritHealerAfterMs == 45000 && spiritDefault.SpiritHealerOnRepeat && !spiritDefault.HealDownrank);
    BotSettings const spiritSet = ParseBotSettings({ { "SpiritHealerAfterMs", "0" }, { "SpiritHealerOnRepeat", "0" }, { "HealDownrank", "1" } }, warnings);
    CHECK(spiritSet.SpiritHealerAfterMs == 0 && !spiritSet.SpiritHealerOnRepeat && spiritSet.HealDownrank);
    BotSettings const threatDefault = ParseBotSettings({}, warnings);
    CHECK(threatDefault.ThreatHold && threatDefault.ThreatHoldPct == 90 && threatDefault.ThreatReleasePct == 70 && threatDefault.ThreatHoldMaxSec == 8 &&
          threatDefault.ThreatNearDeathPct == 15 && threatDefault.ThreatDropAbilities);
    BotSettings const threatSet = ParseBotSettings({ { "ThreatHold", "0" }, { "ThreatHoldPct", "80" }, { "ThreatReleasePct", "50" }, { "ThreatHoldMaxSec", "5" },
                                                     { "ThreatNearDeathPct", "25" }, { "ThreatDropAbilities", "0" } }, warnings);
    CHECK(!threatSet.ThreatHold && threatSet.ThreatHoldPct == 80 && threatSet.ThreatReleasePct == 50 && threatSet.ThreatHoldMaxSec == 5 && threatSet.ThreatNearDeathPct == 25 &&
          !threatSet.ThreatDropAbilities);
    std::vector<std::string> threatWarnings;
    BotSettings const threatBad = ParseBotSettings({ { "ThreatHoldPct", "60" }, { "ThreatReleasePct", "80" } }, threatWarnings);
    CHECK(threatBad.ThreatReleasePct == 59 && !threatWarnings.empty());   // a release mark at or over the hold mark is pulled under it
    BotSettings const off = ParseBotSettings({}, warnings);
    CHECK(!off.ChatCommands && !off.ChatOrders && !off.ChatControl && !off.QuestTeleport); // all off by default
    BotSettings const on = ParseBotSettings({ { "ChatCommands", "1" }, { "ChatOrders", "1" }, { "ChatControl", "1" }, { "QuestTeleport", "1" } }, warnings);
    CHECK(on.ChatCommands && on.ChatOrders && on.ChatControl && on.QuestTeleport && warnings.empty());
    CHECK(!off.QuestGroups.Enabled && off.QuestGroups.WaitMs == 600000 && off.QuestGroups.RangeYards == 150.0f); // group quests are off by default
    BotSettings const groups = ParseBotSettings({ { "QuestGroups", "1" }, { "QuestGroupWaitMinutes", "5" }, { "QuestGroupRangeYards", "80" }, { "QuestGroupGuildHelpers", "0" } }, warnings);
    CHECK(groups.QuestGroups.Enabled && groups.QuestGroups.WaitMs == 300000 && groups.QuestGroups.RangeYards == 80.0f && !groups.QuestGroups.GuildHelpers && warnings.empty());
}

void QuestListSettings()
{
    std::vector<std::string> warnings;
    BotSettings const off = ParseBotSettings({}, warnings);
    CHECK(off.QuestFileAlliance.empty() && off.QuestFileHorde.empty() && off.QuestFileAlliance60.empty() && off.QuestFileHorde60.empty());
    BotSettings const on = ParseBotSettings({ { "QuestFileAlliance", "a.txt" }, { "QuestFileHorde", "h.txt" }, { "QuestFileAlliance60", "a60.txt" }, { "QuestFileHorde60", "h60.txt" } }, warnings);
    CHECK(on.QuestFileAlliance == "a.txt" && on.QuestFileHorde == "h.txt" && on.QuestFileAlliance60 == "a60.txt" && on.QuestFileHorde60 == "h60.txt");
    CHECK(on.QuestFile.empty() && warnings.empty());
}

void LevelDungeonSettings()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(defaults.LevelDungeons && defaults.LevelDungeonRuns == 2 && defaults.LfgCommand); // on by default
    BotSettings const set = ParseBotSettings({ { "LevelDungeons", "0" }, { "LevelDungeonRuns", "4" }, { "LfgCommand", "0" } }, warnings);
    CHECK(!set.LevelDungeons && set.LevelDungeonRuns == 4 && !set.LfgCommand && warnings.empty());
    CHECK(ParseBotSettings({ { "LevelDungeonRuns", "0" } }, warnings).LevelDungeonRuns == 1);
    CHECK(ParseBotSettings({ { "LevelDungeonRuns", "99" } }, warnings).LevelDungeonRuns == 5);
}

void SayAndSupplySettings()
{
    std::vector<std::string> warnings;
    BotSettings const off = ParseBotSettings({}, warnings);
    CHECK(!off.ChatSay && off.ChatSayMax == 3 && !off.Supplies.Enabled); // off by default
    CHECK(off.Supplies.AmmoLow == 100 && off.Supplies.AmmoTarget == 400 && off.Supplies.FoodTarget == 10 && off.Supplies.TripEveryMs == 120000);
    CHECK(warnings.empty());

    BotSettings const on = ParseBotSettings({ { "ChatSay", "1" }, { "ChatSayMax", "5" }, { "Supplies", "1" }, { "SupplyAmmoLow", "50" },
                                              { "SupplyAmmoTarget", "300" }, { "SupplyFoodLow", "2" }, { "SupplyFoodTarget", "15" },
                                              { "SupplyDrinkLow", "4" }, { "SupplyDrinkTarget", "20" }, { "SupplyTripSec", "300" } }, warnings);
    CHECK(on.ChatSay && on.ChatSayMax == 5 && on.Supplies.Enabled);
    CHECK(on.Supplies.AmmoLow == 50 && on.Supplies.AmmoTarget == 300 && on.Supplies.FoodLow == 2 && on.Supplies.FoodTarget == 15);
    CHECK(on.Supplies.DrinkLow == 4 && on.Supplies.DrinkTarget == 20 && on.Supplies.TripEveryMs == 300000);
    CHECK(!off.Supplies.Potions && !off.Supplies.Bandages && !on.Supplies.Potions && !on.Supplies.Bandages); // layout 16: off by default
    CHECK(off.Supplies.HealthPotionLow == 1 && off.Supplies.BandageLow == 3 && off.Supplies.UseHealthPotionBelowPct == 35);
    BotSettings const goods = ParseBotSettings({ { "Supplies", "1" }, { "SupplyPotions", "1" }, { "SupplyBandages", "1" }, { "SupplyHealthPotionLow", "2" },
                                                 { "SupplyManaPotionLow", "3" }, { "SupplyBandageLow", "5" }, { "UseHealthPotionBelowPct", "50" },
                                                 { "UseManaPotionBelowPct", "20" }, { "UseBandageBelowPct", "70" } }, warnings);
    CHECK(goods.Supplies.Potions && goods.Supplies.Bandages && goods.Supplies.HealthPotionLow == 2 && goods.Supplies.ManaPotionLow == 3);
    CHECK(goods.Supplies.BandageLow == 5 && goods.Supplies.UseHealthPotionBelowPct == 50 && goods.Supplies.UseManaPotionBelowPct == 20);
    CHECK(goods.Supplies.UseBandageBelowPct == 70 && warnings.empty());
    BotSettings const reagents = ParseBotSettings({ { "Supplies", "1" }, { "SupplyReagents", "1" } }, warnings);
    CHECK(reagents.Supplies.Reagents && !off.Supplies.Reagents && warnings.empty());

    // Presence: off by default; a maximum below its minimum is raised; the day curve has 24 numbers.
    CHECK(!off.Presence.Enabled && off.Presence.OnlineMinMin == 60 && off.Presence.OfflineMaxMin == 240 && off.Presence.DayPercent[20] == 100);
    BotSettings const away = ParseBotSettings({ { "Presence", "1" }, { "PresenceOnlineMinMin", "90" }, { "PresenceOnlineMaxMin", "30" },
                                                { "PresenceOfflineMinMin", "10" }, { "PresenceOfflineMaxMin", "20" }, { "PresenceMinOnline", "40" },
                                                { "PresenceMaxOnline", "800" },
                                                { "PresenceDayPercent", "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24" } }, warnings);
    CHECK(away.Presence.Enabled && away.Presence.OnlineMinMin == 90 && away.Presence.OnlineMaxMin == 90);
    CHECK(away.Presence.OfflineMinMin == 10 && away.Presence.OfflineMaxMin == 20 && away.Presence.MinOnline == 40 && away.Presence.MaxOnline == 800);
    CHECK(away.Presence.DayPercent[0] == 1 && away.Presence.DayPercent[23] == 24 && warnings.empty());
    // Auction: off by default; the run time is one of the fork's three.
    CHECK(!off.Auction.BuyEnabled);                                  // buying is off by default, whatever Auction says
    CHECK(!ParseBotSettings({ { "Auction", "1" } }, warnings).Auction.BuyEnabled);
    BotSettings const buys = ParseBotSettings({ { "Auction", "1" }, { "AuctionBuy", "1" } }, warnings);
    CHECK(buys.Auction.Enabled && buys.Auction.BuyEnabled);
    CHECK(!off.Auction.Enabled && off.Auction.SellPctGreen == 300 && off.Auction.RunMinutes == 120 && off.Auction.EveryMs == 1800000);
    BotSettings const house = ParseBotSettings({ { "Auction", "1" }, { "AuctionSellPctGreen", "250" }, { "AuctionSellPctBlue", "350" }, { "AuctionSellPctEpic", "450" },
                                                 { "AuctionBuyoutPct", "130" }, { "AuctionRelistPct", "80" }, { "AuctionRunHours", "2" },
                                                 { "AuctionMinVendorCopper", "500" }, { "AuctionBagFullPct", "70" }, { "AuctionPerVisit", "3" },
                                                 { "AuctionListedMax", "6" }, { "AuctionEverySec", "600" }, { "AuctionBuyMaxPct", "25" },
                                                 { "AuctionBrowsePerVisit", "2" }, { "AuctionCallsPerTick", "8" } }, warnings);
    CHECK(house.Auction.Enabled && house.Auction.SellPctGreen == 250 && house.Auction.SellPctBlue == 350 && house.Auction.SellPctEpic == 450);
    CHECK(house.Auction.BuyoutPct == 130 && house.Auction.RelistPct == 80 && house.Auction.RunMinutes == 120 && house.Auction.MinVendorCopper == 500);
    CHECK(house.Auction.BagFullPct == 70 && house.Auction.PerVisit == 3 && house.Auction.ListedMax == 6 && house.Auction.EveryMs == 600000);
    CHECK(house.Auction.BuyMaxPct == 25 && house.Auction.BrowsePerVisit == 2 && house.Auction.MaxCallsPerTick == 8 && warnings.empty());
    BotSettings const badHours = ParseBotSettings({ { "AuctionRunHours", "12" } }, warnings);
    CHECK(badHours.Auction.RunMinutes == 120 && warnings.size() == 1);
    BotSettings const day = ParseBotSettings({ { "AuctionRunHours", "24" } }, warnings);
    CHECK(day.Auction.RunMinutes == 120 && warnings.size() == 2); // the core runs every auction 120 minutes whatever is asked
    warnings.clear();
    CHECK(!off.Taxi.Enabled && off.Taxi.MinWalkYards == 1500.0f && off.Taxi.MasterMaxYards == 600.0f && off.Taxi.BlockedMs == 600000);
    BotSettings const flights = ParseBotSettings({ { "Taxi", "1" }, { "TaxiMinWalkYards", "2000" }, { "TaxiMasterMaxYards", "800" }, { "TaxiBlockedSec", "120" } }, warnings);
    CHECK(flights.Taxi.Enabled && flights.Taxi.MinWalkYards == 2000.0f && flights.Taxi.MasterMaxYards == 800.0f && flights.Taxi.BlockedMs == 120000 && warnings.empty());
    BotSettings const bossAt = ParseBotSettings({ { "Scenario", "dm" }, { "ScenarioRoster", "2:1,5:5" }, { "ScenarioBossAt", "6:644,11:643,x" } }, warnings);
    CHECK(bossAt.Scenario.BossAt.size() == 2 && bossAt.Scenario.BossAt[0].first == 5 && bossAt.Scenario.BossAt[0].second == 644 &&
          bossAt.Scenario.BossAt[1].first == 10 && bossAt.Scenario.BossAt[1].second == 643);
    bool warnedAboutX = false;
    for (std::string const& line : warnings)
        warnedAboutX = warnedAboutX || line.find("ScenarioBossAt") != std::string::npos;
    CHECK(warnedAboutX);
    warnings.clear();
    BotSettings const files = ParseBotSettings({ { "DungeonFile", "data/dungeons.txt" }, { "DungeonEventsFile", "data/dungeon_events.txt" } }, warnings);
    CHECK(files.DungeonFile == "data/dungeons.txt" && files.DungeonEventsFile == "data/dungeon_events.txt" && off.DungeonFile.empty() && off.DungeonEventsFile.empty());
    BotSettings const supplies = ParseBotSettings({ { "Scenario", "dm" }, { "ScenarioRoster", "2:1,5:5" }, { "ScenarioSupplies", "4540:5,1205:5,x" } }, warnings);
    CHECK(supplies.Scenario.Supplies.size() == 2 && supplies.Scenario.Supplies[0].first == 4540 && supplies.Scenario.Supplies[1].second == 5);
    bool warnedSupplies = false;
    for (std::string const& line : warnings)
        warnedSupplies = warnedSupplies || line.find("ScenarioSupplies") != std::string::npos;
    CHECK(warnedSupplies);
    warnings.clear();
    BotSettings const badCurve = ParseBotSettings({ { "PresenceDayPercent", "1,2,3" } }, warnings);
    CHECK(badCurve.Presence.DayPercent == presence::Settings().DayPercent && warnings.size() == 1);
    warnings.clear();
    CHECK(warnings.empty());

    // A target below its low mark is raised to it; a value out of range falls back with a warning.
    warnings.clear();
    BotSettings const odd = ParseBotSettings({ { "SupplyAmmoLow", "500" }, { "SupplyAmmoTarget", "100" }, { "ChatSayMax", "999" } }, warnings);
    CHECK(odd.Supplies.AmmoLow == 500 && odd.Supplies.AmmoTarget == 500);
    CHECK(odd.ChatSayMax == 3 && warnings.size() == 1);
}

void MaxBotsIsCapped()
{
    std::vector<std::string> warnings;
    CHECK(ParseBotSettings({ { "MaxBots", "2500" } }, warnings).MaxBots == 2500); // the ceiling itself
    CHECK(warnings.empty());
    BotSettings const s = ParseBotSettings({ { "MaxBots", "2501" } }, warnings);
    CHECK(s.MaxBots == 1); // above the ceiling: the default, with a warning
    CHECK(warnings.size() == 1);
}

void RandomRaceAndClass()
{
    std::vector<std::string> warnings;
    BotSettings const s = ParseBotSettings({ { "Race", "random" }, { "Class", "random" }, { "RandomSeed", "42" } }, warnings);
    CHECK(s.Race == 0 && s.Class == 0); // 0 means random
    CHECK(s.RandomSeed == 42);
    CHECK(warnings.empty());
    CHECK(ParseBotSettings({ { "Race", "random" } }, warnings).RandomSeed == 1); // seed default
    CHECK(ParseBotSettings({}, warnings).RandomSeed == 1);
}

void RampSettings()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(defaults.Ramp.Initial == 300 && defaults.Ramp.Step == 100 && defaults.Ramp.StepMinutes == 15);
    CHECK(defaults.Ramp.Cap == 2500 && defaults.Ramp.StopSecOfDay == 6 * 3600);

    BotSettings const s = ParseBotSettings({ { "BotsInitial", "50" }, { "BotsStep", "25" }, { "BotsStepMinutes", "5" },
                                              { "BotsCap", "400" }, { "BotsStopTime", "05:30" } }, warnings);
    CHECK(s.Ramp.Initial == 50 && s.Ramp.Step == 25 && s.Ramp.StepMinutes == 5 && s.Ramp.Cap == 400);
    CHECK(s.Ramp.StopSecOfDay == 5 * 3600 + 30 * 60);
    CHECK(warnings.empty());

    CHECK(ParseBotSettings({ { "BotsStopTime", "25:00" } }, warnings).Ramp.StopSecOfDay == 6 * 3600); // kept, warned
    CHECK(!warnings.empty());
    CHECK(ParseBotSettings({ { "BotsCap", "2501" } }, warnings).Ramp.Cap == 2500);
}

void ServiceSettings()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(defaults.ServiceEverySec == 600 && defaults.TrainEverySec == 0); // the trainer check is off by default
    BotSettings const s = ParseBotSettings({ { "ServiceEverySec", "300" }, { "TrainEverySec", "30" } }, warnings);
    CHECK(s.ServiceEverySec == 300);
    CHECK(s.TrainEverySec == 0); // 30 is below the minimum: the default (off), with a warning
    CHECK(warnings.size() == 1);
}

void TelemetryRotationSettings()
{
    std::vector<std::string> warnings;
    BotSettings const s = ParseBotSettings({ { "TelemetryMaxMB", "256" }, { "TelemetryKeep", "8" } }, warnings);
    CHECK(s.TelemetryMaxMB == 256 && s.TelemetryKeep == 8);
    CHECK(ParseBotSettings({}, warnings).TelemetryMaxMB == 64);
    CHECK(warnings.empty());
}

void InvalidValuesFallBackWithWarnings()
{
    std::vector<std::string> warnings;
    BotSettings const s = ParseBotSettings({ { "Race", "0" }, { "Class", "warrior" }, { "MaxBots", "2x" } }, warnings);
    CHECK(s.Race == 1 && s.Class == 1 && s.MaxBots == 1);
    CHECK(warnings.size() == 3);
}

void UnknownKeysAreIgnored()
{
    std::vector<std::string> warnings;
    BotSettings const s = ParseBotSettings({ { "Something", "else" } }, warnings);
    CHECK(s.MaxBots == 1);
    CHECK(warnings.empty());
}

void TestSpellIsOptional()
{
    std::vector<std::string> warnings;
    CHECK(ParseBotSettings({}, warnings).TestSpell == 0);
    CHECK(ParseBotSettings({ { "TestSpell", "585" } }, warnings).TestSpell == 585);
    CHECK(warnings.empty());
    CHECK(ParseBotSettings({ { "TestSpell", "-1" } }, warnings).TestSpell == 0);
    CHECK(warnings.size() == 1);
}

void EffectiveMaxBotsIsCappedByGrant()
{
    CHECK(EffectiveMaxBots(4, 16, true) == 4);  // config below the grant
    CHECK(EffectiveMaxBots(16, 2, true) == 2);  // config above the grant: the grant wins
    CHECK(EffectiveMaxBots(3, 0, true) == 0);   // a grant of zero allows no bots
    CHECK(EffectiveMaxBots(3, 5, false) == 0);  // no Tier 2 region: no bots, whatever the grant says
}

void PartySettings()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(defaults.PartySize == 0 && defaults.PartyCount == 1); // no parties until configured
    BotSettings const s = ParseBotSettings({ { "PartySize", "5" }, { "PartyCount", "20" } }, warnings);
    CHECK(s.PartySize == 5 && s.PartyCount == 20);
    CHECK(warnings.empty());
    CHECK(ParseBotSettings({ { "PartySize", "6" } }, warnings).PartySize == 0); // a party holds at most five
    CHECK(warnings.size() == 1);
}

void RandomnessAndStanceSettings()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(defaults.PartyPercent == 30 && defaults.RandomPct == 25);   // only a share of the bots party, and everything varies a little
    CHECK(!defaults.AggressiveStart);                                  // the default stance stays defensive
    BotSettings const s = ParseBotSettings({ { "PartyPercent", "60" }, { "RandomPct", "10" }, { "AggressiveStart", "1" } }, warnings);
    CHECK(s.PartyPercent == 60 && s.RandomPct == 10 && s.AggressiveStart);
    CHECK(warnings.empty());
    CHECK(ParseBotSettings({ { "PartyPercent", "120" } }, warnings).PartyPercent == 30); // out of range: the default, with a warning
    CHECK(warnings.size() == 1);
}

void HubSettings()
{
    std::vector<std::string> warnings;
    CHECK(ParseBotSettings({}, warnings).Hubs.empty()); // no hubs until configured
    BotSettings const s = ParseBotSettings({ { "Hub1_2", "900 0 0" }, { "Hub1_1", "500 0 0" }, { "Hub0_1", "1 2 3" } }, warnings);
    CHECK(s.Hubs.size() == 2);
    CHECK(s.Hubs.at(1).size() == 2 && s.Hubs.at(1)[0][0] == 500.0f && s.Hubs.at(1)[1][0] == 900.0f); // in order n
    CHECK(s.Hubs.at(0)[0][2] == 3.0f);
    CHECK(warnings.empty());
    BotSettings const bad = ParseBotSettings({ { "Hub2", "1 2 3" }, { "Hub2_1", "1 2" }, { "Hubx_1", "1 2 3" } }, warnings);
    CHECK(bad.Hubs.empty());
    CHECK(warnings.size() == 3);
}

void TelemetrySettings()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(defaults.TelemetryFile == "forever-bots-telemetry.csv");
    CHECK(defaults.TelemetrySummarySec == 60);
    CHECK(defaults.LiveFile.empty() && defaults.LiveEveryMs == 1000);
    CHECK(ParseBotSettings({ { "LiveFile", "/tmp/x.json" }, { "LiveEveryMs", "500" } }, warnings).LiveEveryMs == 500);
    CHECK(ParseBotSettings({ { "LiveEveryMs", "5" } }, warnings).LiveEveryMs == 1000);

    BotSettings const off = ParseBotSettings({ { "TelemetryFile", "" } }, warnings);
    CHECK(off.TelemetryFile.empty()); // empty turns telemetry off
    CHECK(ParseBotSettings({ { "TelemetrySummarySec", "300" } }, warnings).TelemetrySummarySec == 300);
    CHECK(ParseBotSettings({ { "TelemetrySummarySec", "0" } }, warnings).TelemetrySummarySec == 60);
    CHECK(!warnings.empty()); // the zero interval is refused with a warning
}

void DungeonLearningKnobsAreBounded()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(defaults.DungeonHealBelowPct == 55 && defaults.DungeonTankHealBelowPct == 75);
    CHECK(defaults.HealerCombatManaPct == 25 && defaults.TankPickup && !defaults.PartyViewPlanner);
    CHECK(defaults.HealerDpsManaPct == 40 && defaults.HealerLosMoveYards == 6 && warnings.empty());
    CHECK(defaults.DamageEvents && defaults.DamageEventMinPct == 20);
    BotSettings const damageSet = ParseBotSettings({ { "DamageEvents", "0" }, { "DamageEventMinPct", "35" } }, warnings);
    CHECK(!damageSet.DamageEvents && damageSet.DamageEventMinPct == 35 && warnings.empty());
    BotSettings const damageBad = ParseBotSettings({ { "DamageEventMinPct", "500" } }, warnings);
    CHECK(damageBad.DamageEventMinPct == 20 && !warnings.empty());
    warnings.clear();
    CHECK(defaults.PullTankFirst && defaults.PullTankHoldMs == 12000 && defaults.PullMemberRangeYards == 12 && defaults.PullMinHpPct == 60);
    CHECK(ParseBotSettings({ { "PullMinHpPct", "0" } }, warnings).PullMinHpPct == 0 && warnings.empty());
    CHECK(defaults.PullReadyManaPct == 60 && defaults.PullReadyMaxMs == 90000);
    CHECK(defaults.WipeReadyPct == 80 && defaults.WipeReadyMaxMs == 240000 && defaults.WipeBackoffCount == 3 && defaults.WipeBackoffMs == 90000);
    CHECK(ParseBotSettings({ { "PullReadyManaPct", "0" }, { "WipeBackoffCount", "0" }, { "WipeReadyPct", "0" } }, warnings).PullReadyManaPct == 0 && warnings.empty());
    CHECK(ParseBotSettings({ { "WipeReadyMaxMs", "5" } }, warnings).WipeReadyMaxMs == 240000 && !warnings.empty());
    warnings.clear();
    CHECK(defaults.WarderPartyHold && !ParseBotSettings({ { "WarderPartyHold", "0" } }, warnings).WarderPartyHold && warnings.empty());
    CHECK(defaults.TankLoosePickup && defaults.TankLooseCooldownMs == 2500 && defaults.TankLooseRangeYards == 40 && defaults.TankLooseChaseYards == 8);
    BotSettings const looseSet = ParseBotSettings({ { "TankLoosePickup", "0" }, { "TankLooseCooldownMs", "4000" }, { "TankLooseRangeYards", "30" }, { "TankLooseChaseYards", "0" } }, warnings);
    CHECK(!looseSet.TankLoosePickup && looseSet.TankLooseCooldownMs == 4000 && looseSet.TankLooseRangeYards == 30 && looseSet.TankLooseChaseYards == 0 && warnings.empty());
    BotSettings const looseBad = ParseBotSettings({ { "TankLooseRangeYards", "5" }, { "TankLooseChaseYards", "1" } }, warnings);
    CHECK(looseBad.TankLooseRangeYards == 40 && looseBad.TankLooseChaseYards == 8 && warnings.size() == 2);
    warnings.clear();
    BotSettings const pullSet = ParseBotSettings({ { "PullTankFirst", "0" }, { "PullTankHoldMs", "5000" }, { "PullMemberRangeYards", "8" } }, warnings);
    CHECK(!pullSet.PullTankFirst && pullSet.PullTankHoldMs == 5000 && pullSet.PullMemberRangeYards == 8 && warnings.empty());

    BotSettings const candidate = ParseBotSettings({ { "DungeonHealBelowPct", "60" }, { "DungeonTankHealBelowPct", "80" },
                                                     { "HealerDpsManaPct", "50" }, { "HealerLosMoveYards", "8" } }, warnings);
    CHECK(candidate.DungeonHealBelowPct == 60 && candidate.DungeonTankHealBelowPct == 80);
    CHECK(candidate.HealerDpsManaPct == 50 && candidate.HealerLosMoveYards == 8 && warnings.empty());

    BotSettings const refused = ParseBotSettings({ { "DungeonHealBelowPct", "99" }, { "DungeonTankHealBelowPct", "0" },
                                                    { "HealerDpsManaPct", "0" }, { "HealerLosMoveYards", "99" } }, warnings);
    CHECK(refused.DungeonHealBelowPct == 55 && refused.DungeonTankHealBelowPct == 75);
    CHECK(refused.HealerDpsManaPct == 40 && refused.HealerLosMoveYards == 6 && warnings.size() == 4);
    warnings.clear();
    BotSettings const tuned = ParseBotSettings({ { "HealerCombatManaPct", "0" }, { "TankPickup", "0" }, { "PartyViewPlanner", "1" } }, warnings);
    CHECK(tuned.HealerCombatManaPct == 0 && !tuned.TankPickup && tuned.PartyViewPlanner && warnings.empty());
    BotSettings const tooHigh = ParseBotSettings({ { "HealerCombatManaPct", "95" } }, warnings);
    CHECK(tooHigh.HealerCombatManaPct == 25 && warnings.size() == 1);
}

// 0.45.0: the combat-drift settings. On by default (30 and 40 yards, the leader's rejoin); 0 turns a leash off, and a value outside 10..80 keeps the default with a warning.
void CombatDriftSettings()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(defaults.DungeonPullLeashYards == 30 && defaults.DungeonChaseLeashYards == 40 && defaults.DungeonLeaderRejoin && warnings.empty());
    BotSettings const off = ParseBotSettings({ { "DungeonPullLeashYards", "0" }, { "DungeonChaseLeashYards", "0" }, { "DungeonLeaderRejoin", "0" } }, warnings);
    CHECK(off.DungeonPullLeashYards == 0 && off.DungeonChaseLeashYards == 0 && !off.DungeonLeaderRejoin && warnings.empty());
    BotSettings const set = ParseBotSettings({ { "DungeonPullLeashYards", "25" }, { "DungeonChaseLeashYards", "60" } }, warnings);
    CHECK(set.DungeonPullLeashYards == 25 && set.DungeonChaseLeashYards == 60 && warnings.empty());
    BotSettings const refused = ParseBotSettings({ { "DungeonPullLeashYards", "5" }, { "DungeonChaseLeashYards", "200" } }, warnings);
    CHECK(refused.DungeonPullLeashYards == 30 && refused.DungeonChaseLeashYards == 40 && warnings.size() == 2);
}

bool HasWarning(std::vector<std::string> const& warnings, std::string const& part)
{
    for (std::string const& w : warnings)
        if (w.find(part) != std::string::npos)
            return true;
    return false;
}

// Scenario = "dm:10,rfc:10": the list, in order, with at most 200 parties in all.
void ScenarioListParsing()
{
    CHECK(!IsScenarioList("rfc") && IsScenarioList("rfc:3") && IsScenarioList("dm,rfc"));
    std::vector<std::string> warnings;
    auto const two = ParseScenarioList("dm:10, rfc:10", warnings);
    CHECK(two.size() == 2 && two[0].Name == "dm" && two[0].Parties == 10 && two[1].Name == "rfc" && two[1].Parties == 10 && warnings.empty());
    auto const plain = ParseScenarioList("dm,rfc:2", warnings);
    CHECK(plain.size() == 2 && plain[0].Parties == 1 && plain[1].Parties == 2);

    auto const over = ParseScenarioList("dm:150,rfc:100,wc:2", warnings);    // 200 in all: rfc is cut to 50, wc dropped
    CHECK(over.size() == 2 && over[0].Parties == 150 && over[1].Parties == 50 && HasWarning(warnings, "more than 200 parties"));
    warnings.clear();
    auto const bad = ParseScenarioList("dm:0,rfc:x,:3,wc:201,sfk:2,sfk:3", warnings);
    CHECK(bad.size() == 1 && bad[0].Name == "sfk" && bad[0].Parties == 2);
    CHECK(HasWarning(warnings, "'dm:0' is not dungeon:parties") && HasWarning(warnings, "'wc:201'") && HasWarning(warnings, "'sfk' is listed twice"));
}

// Each listed dungeon reads its own keys ("<key>.<id>") over the shared ones; the plain dungeon keys are for the single form and are ignored here.
void ScenarioListReadsEachDungeon()
{
    std::vector<std::string> warnings;
    BotSettings const s = ParseBotSettings({ { "Scenario", "dm:10,rfc:10" }, { "ScenarioDurationSec", "3600" }, { "ScenarioRoster", "2:1" }, { "ScenarioLevel", "30" },
                                             { "ScenarioRoster.dm", "1:1,1:5,1:8,3:3,7:9" }, { "ScenarioLevel.dm", "18" }, { "ScenarioEntry.dm", "36 -14.57 -385.48 62.46" },
                                             { "ScenarioStaging.dm", "0 -8913 554 94" }, { "ScenarioRoute1.dm", "1 2 3" }, { "ScenarioPortal.dm", "0 -11208.7 1675.9 24.6" },
                                             { "ScenarioRoster.rfc", "2:1,5:5,8:8,2:9,5:4" }, { "ScenarioLevel.rfc", "17" }, { "ScenarioEntry.rfc", "389 0.8 -8.23 -15.53" },
                                             { "ScenarioRoute1.rfc", "4 5 6" }, { "ScenarioRoute2.rfc", "7 8 9" }, { "ScenarioKillPriority.rfc", "11320" } },
                                           warnings);
    CHECK(s.Scenarios.size() == 2);
    ScenarioSettings const& dm = s.Scenarios[0];
    ScenarioSettings const& rfc = s.Scenarios[1];
    CHECK(dm.Name == "dm" && dm.Parties == 10 && dm.Level == 18 && dm.Roster.size() == 5 && dm.Roster[0][0] == 1 && dm.Roster[4][0] == 7 && dm.Roster[4][1] == 9);
    CHECK(dm.HasEntry && dm.Entry[0] == 36.0f && dm.HasStaging && dm.Staging[0] == 0.0f && dm.HasPortal && dm.Route.size() == 1 && dm.KillPriority.empty());
    CHECK(rfc.Name == "rfc" && rfc.Parties == 10 && rfc.Level == 17 && rfc.Roster[0][0] == 2 && rfc.Entry[0] == 389.0f && !rfc.HasPortal);
    CHECK(rfc.Route.size() == 2 && rfc.Route[1][0] == 7.0f && rfc.KillPriority.size() == 1);
    CHECK(dm.DurationSec == 3600 && rfc.DurationSec == 3600);            // a shared key applies to every dungeon
    CHECK(s.Scenario.Name == "dm");                                       // the first one, for the code that knows one
    CHECK(HasWarning(warnings, "ScenarioRoster: ignored with a list") && HasWarning(warnings, "ScenarioLevel: ignored with a list"));

    // The bots in spawn order: the first dungeon's ten parties first, each slot with the race and class of its place in its own dungeon's roster.
    CHECK(ScenarioBotCount(s.Scenarios) == 100 && ScenarioPartyCount(s.Scenarios) == 20);
    ScenarioSlot const first = ScenarioSlotOf(s.Scenarios, 0);
    CHECK(first.Valid && first.Dungeon == 0 && first.Party == 0 && first.Race == 1 && first.Class == 1);
    ScenarioSlot const lastDm = ScenarioSlotOf(s.Scenarios, 49);
    CHECK(lastDm.Valid && lastDm.Dungeon == 0 && lastDm.Party == 9 && lastDm.Race == 7 && lastDm.Class == 9);
    ScenarioSlot const firstRfc = ScenarioSlotOf(s.Scenarios, 50);
    CHECK(firstRfc.Valid && firstRfc.Dungeon == 1 && firstRfc.Party == 10 && firstRfc.Race == 2 && firstRfc.Class == 1);
    ScenarioSlot const lastRfc = ScenarioSlotOf(s.Scenarios, 99);
    CHECK(lastRfc.Valid && lastRfc.Dungeon == 1 && lastRfc.Party == 19 && lastRfc.Race == 5 && lastRfc.Class == 4);
    CHECK(!ScenarioSlotOf(s.Scenarios, 100).Valid);
    CHECK(ScenarioDungeonOfParty(s.Scenarios, 9) == 0 && ScenarioDungeonOfParty(s.Scenarios, 10) == 1 && ScenarioDungeonOfParty(s.Scenarios, 19) == 1);
}

// A roster with Alliance and Horde races cannot be one party: refused, in both forms. In the list form a roster of another size than the first is dropped too.
void MixedFactionRosterIsRefused()
{
    std::vector<std::string> warnings;
    BotSettings const list = ParseBotSettings({ { "Scenario", "dm:2,rfc:3" }, { "ScenarioRoster.dm", "1:1,2:5,1:8" }, { "ScenarioEntry.dm", "36 1 2 3" },
                                                { "ScenarioRoster.rfc", "2:1,5:5,8:8" }, { "ScenarioEntry.rfc", "389 1 2 3" } },
                                              warnings);
    CHECK(list.Scenarios.size() == 1 && list.Scenarios[0].Name == "rfc" && list.Scenario.Name == "rfc" && ScenarioPartyCount(list.Scenarios) == 3);
    CHECK(HasWarning(warnings, "ScenarioRoster.dm: the roster mixes Alliance and Horde"));

    warnings.clear();
    BotSettings const single = ParseBotSettings({ { "Scenario", "x" }, { "ScenarioRoster", "1:1,5:5" }, { "ScenarioEntry", "36 1 2 3" } }, warnings);
    CHECK(single.Scenario.Name.empty() && single.Scenarios.empty() && HasWarning(warnings, "mixes Alliance and Horde"));

    warnings.clear();
    BotSettings const sizes = ParseBotSettings({ { "Scenario", "dm:1,rfc:1" }, { "ScenarioRoster.dm", "1:1,1:5,1:8" }, { "ScenarioEntry.dm", "36 1 2 3" },
                                                 { "ScenarioRoster.rfc", "2:1,5:5" }, { "ScenarioEntry.rfc", "389 1 2 3" } },
                                               warnings);
    CHECK(sizes.Scenarios.size() == 1 && sizes.Scenarios[0].Name == "dm" && HasWarning(warnings, "ScenarioRoster.rfc: 2 bots, not 3"));

    warnings.clear();
    BotSettings const none = ParseBotSettings({ { "Scenario", "dm:1" } }, warnings);    // no roster for it: nothing runs
    CHECK(none.Scenarios.empty() && none.Scenario.Name.empty() && HasWarning(warnings, "(dungeon dm)") && HasWarning(warnings, "the scenario is off"));
}

// The sim's single-dungeon config reads as before: one dungeon, its parties from ScenarioParties.
void SingleDungeonScenarioIsUnchanged()
{
    std::vector<std::string> warnings;
    BotSettings const s = ParseBotSettings({ { "Scenario", "rfc" }, { "Dungeon", "rfc" }, { "ScenarioParties", "20" }, { "ScenarioLevel", "17" },
                                             { "ScenarioRoster", "2:1,5:5,8:8,2:9,5:4" }, { "ScenarioDurationSec", "3600" }, { "DungeonTeleport", "1" },
                                             { "ScenarioEntry", "389 0.8 -8.23 -15.53" } },
                                           warnings);
    CHECK(warnings.empty());
    CHECK(s.Scenario.Name == "rfc" && s.Scenario.Parties == 20 && s.Scenario.Level == 17 && s.Scenario.Roster.size() == 5 && s.Scenario.DurationSec == 3600);
    CHECK(s.Scenarios.size() == 1 && s.Scenarios[0].Name == "rfc" && s.Scenarios[0].Parties == 20 && s.DungeonTeleport);
    CHECK(ScenarioBotCount(s.Scenarios) == 100 && ScenarioPartyCount(s.Scenarios) == 20);
    ScenarioSlot const slot = ScenarioSlotOf(s.Scenarios, 57);           // the same roster cycle as before: slot % 5
    CHECK(slot.Valid && slot.Dungeon == 0 && slot.Party == 11 && slot.Race == 8 && slot.Class == 8);
    CHECK(ParseBotSettings({ { "Scenario", "rfc" }, { "ScenarioParties", "201" }, { "ScenarioRoster", "2:1" } }, warnings).Scenario.Parties == 1);
}

// 40-man raid support (doc 108): ScenarioRaid is a distinct opt-in mode. Without it (the default) a roster longer than 5 is still
// cut to 5, exactly as before -- a raid must not change five-man scenario behavior. With it, up to 40 is kept, and OneFaction still
// refuses a roster mixing Alliance and Horde (a raid is one faction too).
void RaidRosterAllowsUpToFortyFiveManStaysAtFive()
{
    std::vector<std::string> warnings;
    std::string roster40;
    for (int i = 0; i < 40; ++i)
        roster40 += (i ? "," : "") + std::string("1:1");
    BotSettings const raid = ParseBotSettings({ { "Scenario", "ony" }, { "ScenarioRaid", "1" }, { "ScenarioRoster", roster40 },
                                                 { "ScenarioEntry", "249 1 2 3" } },
                                               warnings);
    CHECK(raid.Scenario.Raid && raid.Scenario.Roster.size() == 40);

    warnings.clear();
    BotSettings const five = ParseBotSettings({ { "Scenario", "ony" }, { "ScenarioRoster", roster40 }, { "ScenarioEntry", "249 1 2 3" } }, warnings);
    CHECK(!five.Scenario.Raid && five.Scenario.Roster.size() == 5); // ScenarioRaid unset: the old 5-bot cap, unchanged

    // One faction applies to a raid roster too.
    warnings.clear();
    std::string mixed;
    for (int i = 0; i < 40; ++i)
        mixed += (i ? "," : "") + std::string(i < 20 ? "1:1" : "5:5"); // race 1 Human (Alliance), race 5 Orc (Horde)
    BotSettings const refused = ParseBotSettings({ { "Scenario", "ony" }, { "ScenarioRaid", "1" }, { "ScenarioRoster", mixed },
                                                    { "ScenarioEntry", "249 1 2 3" } },
                                                  warnings);
    CHECK(refused.Scenario.Name.empty() && HasWarning(warnings, "mixes Alliance and Horde"));
}

} // namespace

void WipeRecoverySettings()
{
    std::vector<std::string> warnings;
    BotSettings const d = ParseBotSettings({}, warnings);
    CHECK(!d.Scenario.WipeRecovery && d.Scenario.WipeRecoverySec == 300 && d.Scenario.MinAlive == 0 && !d.Scenario.HasPortal);
    BotSettings const s = ParseBotSettings({ { "Scenario", "x" }, { "ScenarioRoster", "1:1" }, { "ScenarioWipeRecovery", "1" }, { "ScenarioWipeRecoverySec", "120" }, { "ScenarioMinAlive", "2" },
                                             { "ScenarioLeaderDeadAbandonSec", "90" }, { "ScenarioPortal", "0 -11.0 -390.0 60.0" } }, warnings);
    CHECK(s.Scenario.WipeRecovery && s.Scenario.WipeRecoverySec == 120 && s.Scenario.MinAlive == 2 && s.Scenario.LeaderDeadAbandonSec == 90);
    CHECK(s.Scenario.HasPortal);
}

void ErrandSettings()
{
    std::vector<std::string> warnings;
    BotSettings const off = ParseBotSettings({}, warnings);
    CHECK(!off.Errand.Enabled && off.Errand.DailyMinutes == 60 && off.Errand.GapMinutes == 120 && off.Errand.MaxMinutes == 60);
    CHECK(off.Errand.MaxPerCity == 2 && off.Errand.MaxStartsPerTick == 2);
    BotSettings const on = ParseBotSettings({ { "Errands", "1" }, { "ErrandDailyMin", "90" }, { "ErrandGapMin", "30" }, { "ErrandMaxMin", "45" },
                                              { "ErrandPerCity", "3" }, { "ErrandStartsPerTick", "1" } }, warnings);
    CHECK(on.Errand.Enabled && on.Errand.DailyMinutes == 90 && on.Errand.GapMinutes == 30 && on.Errand.MaxMinutes == 45);
    CHECK(on.Errand.MaxPerCity == 3 && on.Errand.MaxStartsPerTick == 1);
    CHECK(warnings.empty());
    BotSettings const bad = ParseBotSettings({ { "ErrandMaxMin", "1" }, { "ErrandPerCity", "0" } }, warnings);
    CHECK(bad.Errand.MaxMinutes == 60 && bad.Errand.MaxPerCity == 2);
    CHECK(warnings.size() == 2);
}

// CorpseRunTeleportSec: 60 by default, 0 turns it off, 10..3600 otherwise (a value below keeps the default, with a warning).
void CorpseRunTeleportSettings()
{
    std::vector<std::string> warnings;
    CHECK(ParseBotSettings({}, warnings).CorpseRunTeleportSec == 60);
    CHECK(ParseBotSettings({ { "CorpseRunTeleportSec", "0" } }, warnings).CorpseRunTeleportSec == 0);
    CHECK(ParseBotSettings({ { "CorpseRunTeleportSec", "120" } }, warnings).CorpseRunTeleportSec == 120);
    CHECK(warnings.empty());
    CHECK(ParseBotSettings({ { "CorpseRunTeleportSec", "5" } }, warnings).CorpseRunTeleportSec == 60);
    CHECK(warnings.size() == 1);
}

// GhostTeleportHold and the death lockout: defaults, overrides, bad values keep the default with a warning.
void GhostTeleportHoldSettings()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(defaults.GhostTeleportHold && defaults.GhostTeleportHoldMaxMs == 120000 && defaults.DeathLockoutCount == 4 && defaults.DeathLockoutWindowMs == 600000 &&
          defaults.DeathLockoutHoldMs == 90000 && warnings.empty());
    BotSettings const set = ParseBotSettings({ { "GhostTeleportHold", "0" }, { "GhostTeleportHoldMaxMs", "30000" }, { "DeathLockoutCount", "0" },
                                               { "DeathLockoutWindowMs", "300000" }, { "DeathLockoutHoldMs", "0" } }, warnings);
    CHECK(!set.GhostTeleportHold && set.GhostTeleportHoldMaxMs == 30000 && set.DeathLockoutCount == 0 && set.DeathLockoutWindowMs == 300000 && set.DeathLockoutHoldMs == 0);
    CHECK(warnings.empty());
    BotSettings const bad = ParseBotSettings({ { "GhostTeleportHoldMaxMs", "5" }, { "DeathLockoutCount", "1" } }, warnings);
    CHECK(bad.GhostTeleportHoldMaxMs == 120000 && bad.DeathLockoutCount == 4 && warnings.size() == 2);
}

void RealismSettings()
{
    std::vector<std::string> warnings;
    BotSettings const off = ParseBotSettings({}, warnings);
    CHECK(!off.Fidget.Enabled && !off.ReadDelay.Enabled && !off.PassingBuff.Enabled && !off.TownAfk.Enabled && off.Auction.RoundStepCopper == 0); // all off by default
    CHECK(off.ReadDelay.MinSec == 3 && off.ReadDelay.MaxSec == 12 && off.TownAfk.MinMin == 5 && off.TownAfk.MaxMin == 20);
    BotSettings const on = ParseBotSettings({ { "IdleFidget", "1" }, { "ReadDelay", "1" }, { "PassingBuff", "1" }, { "TownAfk", "1" }, { "AuctionRoundStep", "10000" },
                                              { "TownAfkZoneMax", "3" }, { "PassingBuffCooldownSec", "900" }, { "ReadDelayMinSec", "5" }, { "ReadDelayMaxSec", "2" } }, warnings);
    CHECK(on.Fidget.Enabled && on.ReadDelay.Enabled && on.PassingBuff.Enabled && on.TownAfk.Enabled && on.Auction.RoundStepCopper == 10000);
    CHECK(on.TownAfk.ZoneMax == 3 && on.PassingBuff.CooldownSec == 900);
    CHECK(on.ReadDelay.MaxSec >= on.ReadDelay.MinSec);   // an upside-down range is fixed
    CHECK(warnings.empty());
}

void RunTelemetryDirSetting()
{
    std::vector<std::string> warnings;
    CHECK(ParseBotSettings({}, warnings).RunTelemetryDir.empty()); // off by default
    CHECK(warnings.empty());
    BotSettings const on = ParseBotSettings({ { "RunTelemetryDir", "/var/log/forever/sim" } }, warnings);
    CHECK(on.RunTelemetryDir == "/var/log/forever/sim" && warnings.empty());
    std::vector<std::string> bad;
    BotSettings const rel = ParseBotSettings({ { "RunTelemetryDir", "logs/here" } }, bad);
    CHECK(rel.RunTelemetryDir.empty()); // a relative path is refused with one warning
    CHECK(bad.size() == 1 && bad[0].find("RunTelemetryDir") != std::string::npos);
    // The CSV key of the same family is untouched.
    CHECK(ParseBotSettings({}, warnings).TelemetryFile == "forever-bots-telemetry.csv");
}

int main()
{
    RealismSettings();
    CorpseRunTeleportSettings();
    GhostTeleportHoldSettings();
    CombatDriftSettings();
    ErrandSettings();
    ChatSettings();
    QuestListSettings();
    LevelDungeonSettings();
    SayAndSupplySettings();
    RunTelemetryDirSetting();
    DefaultsWhenEmpty();
    RaidRuleRowSwitches();
    TelemetrySettings();
    ReadsValidValues();
    MaxBotsIsCapped();
    RandomRaceAndClass();
    RampSettings();
    TelemetryRotationSettings();
    ServiceSettings();
    InvalidValuesFallBackWithWarnings();
    UnknownKeysAreIgnored();
    EffectiveMaxBotsIsCappedByGrant();
    TestSpellIsOptional();
    PartySettings();
    RandomnessAndStanceSettings();
    HubSettings();
    WipeRecoverySettings();
    DungeonLearningKnobsAreBounded();
    ScenarioListParsing();
    ScenarioListReadsEachDungeon();
    MixedFactionRosterIsRefused();
    SingleDungeonScenarioIsUnchanged();
    RaidRosterAllowsUpToFortyFiveManStaysAtFive();

    if (failures == 0)
        std::printf("settings_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
