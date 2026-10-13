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
    CHECK(warnings.empty());
}

void ReadsValidValues()
{
    std::vector<std::string> warnings;
    BotSettings const s = ParseBotSettings({ { "MaxBots", "4" }, { "Race", "2" }, { "Class", "8" } }, warnings);
    CHECK(s.MaxBots == 4 && s.Race == 2 && s.Class == 8);
    CHECK(warnings.empty());
}

void MaxBotsIsCapped()
{
    std::vector<std::string> warnings;
    CHECK(ParseBotSettings({ { "MaxBots", "1500" } }, warnings).MaxBots == 1500); // the ceiling itself
    CHECK(warnings.empty());
    BotSettings const s = ParseBotSettings({ { "MaxBots", "1501" } }, warnings);
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
    CHECK(defaults.Ramp.Cap == 1500 && defaults.Ramp.StopSecOfDay == 6 * 3600);

    BotSettings const s = ParseBotSettings({ { "BotsInitial", "50" }, { "BotsStep", "25" }, { "BotsStepMinutes", "5" },
                                              { "BotsCap", "400" }, { "BotsStopTime", "05:30" } }, warnings);
    CHECK(s.Ramp.Initial == 50 && s.Ramp.Step == 25 && s.Ramp.StepMinutes == 5 && s.Ramp.Cap == 400);
    CHECK(s.Ramp.StopSecOfDay == 5 * 3600 + 30 * 60);
    CHECK(warnings.empty());

    CHECK(ParseBotSettings({ { "BotsStopTime", "25:00" } }, warnings).Ramp.StopSecOfDay == 6 * 3600); // kept, warned
    CHECK(!warnings.empty());
    CHECK(ParseBotSettings({ { "BotsCap", "1501" } }, warnings).Ramp.Cap == 1500);
}

void ServiceSettings()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(defaults.ServiceEverySec == 600 && defaults.TrainEverySec == 600);
    BotSettings const s = ParseBotSettings({ { "ServiceEverySec", "300" }, { "TrainEverySec", "30" } }, warnings);
    CHECK(s.ServiceEverySec == 300);
    CHECK(s.TrainEverySec == 600); // 30 is below the minimum: default, with a warning
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

void VendorSpotSettings()
{
    std::vector<std::string> warnings;
    CHECK(ParseBotSettings({}, warnings).VendorSpots.empty()); // no vendors until configured
    BotSettings const s = ParseBotSettings({ { "VendorPos1", "-8866.5 -132.25 83.0" }, { "VendorPos10", "1 2 3" } }, warnings);
    CHECK(s.VendorSpots.size() == 2);
    CHECK(s.VendorSpots.at(1)[0] == -8866.5f && s.VendorSpots.at(1)[1] == -132.25f && s.VendorSpots.at(1)[2] == 83.0f);
    CHECK(s.VendorSpots.at(10)[2] == 3.0f);
    CHECK(warnings.empty());
    BotSettings const bad = ParseBotSettings({ { "VendorPos2", "abc" }, { "VendorPos3", "1 2" }, { "VendorPos4", "1 2 3 4" } }, warnings);
    CHECK(bad.VendorSpots.empty());
    CHECK(warnings.size() == 3);
}

void TelemetrySettings()
{
    std::vector<std::string> warnings;
    BotSettings const defaults = ParseBotSettings({}, warnings);
    CHECK(defaults.TelemetryFile == "forever-bots-telemetry.csv");
    CHECK(defaults.TelemetrySummarySec == 60);

    BotSettings const off = ParseBotSettings({ { "TelemetryFile", "" } }, warnings);
    CHECK(off.TelemetryFile.empty()); // empty turns telemetry off
    CHECK(ParseBotSettings({ { "TelemetrySummarySec", "300" } }, warnings).TelemetrySummarySec == 300);
    CHECK(ParseBotSettings({ { "TelemetrySummarySec", "0" } }, warnings).TelemetrySummarySec == 60);
    CHECK(!warnings.empty()); // the zero interval is refused with a warning
}

} // namespace

int main()
{
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
    VendorSpotSettings();

    if (failures == 0)
        std::printf("settings_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
