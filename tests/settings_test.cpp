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
    BotSettings const off = ParseBotSettings({}, warnings);
    CHECK(!off.ChatCommands && !off.ChatOrders && !off.ChatControl && !off.QuestTeleport); // all off by default
    BotSettings const on = ParseBotSettings({ { "ChatCommands", "1" }, { "ChatOrders", "1" }, { "ChatControl", "1" }, { "QuestTeleport", "1" } }, warnings);
    CHECK(on.ChatCommands && on.ChatOrders && on.ChatControl && on.QuestTeleport && warnings.empty());
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
    CHECK(defaults.HealerDpsManaPct == 40 && defaults.HealerLosMoveYards == 6 && warnings.empty());

    BotSettings const candidate = ParseBotSettings({ { "DungeonHealBelowPct", "60" }, { "DungeonTankHealBelowPct", "80" },
                                                     { "HealerDpsManaPct", "50" }, { "HealerLosMoveYards", "8" } }, warnings);
    CHECK(candidate.DungeonHealBelowPct == 60 && candidate.DungeonTankHealBelowPct == 80);
    CHECK(candidate.HealerDpsManaPct == 50 && candidate.HealerLosMoveYards == 8 && warnings.empty());

    BotSettings const refused = ParseBotSettings({ { "DungeonHealBelowPct", "99" }, { "DungeonTankHealBelowPct", "0" },
                                                    { "HealerDpsManaPct", "0" }, { "HealerLosMoveYards", "99" } }, warnings);
    CHECK(refused.DungeonHealBelowPct == 55 && refused.DungeonTankHealBelowPct == 75);
    CHECK(refused.HealerDpsManaPct == 40 && refused.HealerLosMoveYards == 6 && warnings.size() == 4);
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

int main()
{
    CorpseRunTeleportSettings();
    ErrandSettings();
    ChatSettings();
    SayAndSupplySettings();
    DefaultsWhenEmpty();
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

    if (failures == 0)
        std::printf("settings_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
