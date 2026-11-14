// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the guild kinds (leveling, utility, raiding, pvp: KindOf and the pvp share), the guild names (ParseNames, ParseStyleWeights, PickName,
// the shipped data/bot_guilds.txt) and the store rows (EncodeRecord, DecodeRecord) of src/bot_guilds.h. No core or SDK needed.
#include "bot_guilds.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
}  // namespace

using namespace bot_guilds;

namespace {

Settings On()
{
    Settings settings;
    settings.Enabled = true;
    return settings;
}

Bot MakeBot(std::uint64_t guid, bool alliance, std::uint64_t guild, std::uint32_t level)
{
    Bot bot;
    bot.Guid = guid;
    bot.Alliance = alliance;
    bot.Guild = guild;
    bot.Level = level;
    bot.Online = true;
    return bot;
}

// Unset keys: no pvp and the guid shares, as before the kinds.
void DefaultsKeepTheGuidShares()
{
    Settings const settings = On();
    CHECK(settings.PvpShare == 0 && !settings.KindByLevel && !settings.Remember && settings.NameFile.empty());
    for (std::uint64_t guid = 1; guid <= 500; ++guid)
        for (std::uint32_t level : { 1u, 20u, 59u, 60u })
            CHECK(KindOf(settings, MakeBot(guid, true, 0, level)) == ShareOf(settings, guid));
}

// PvpShare: about that percent of the bots at PvpMinLevel or more want pvp; nobody below it; the same bot always gets the same answer.
void PvpShareAboveTheMinLevel()
{
    Settings settings = On();
    settings.PvpShare = 30;
    settings.PvpMinLevel = 10;
    int pvp = 0;
    for (std::uint64_t guid = 1; guid <= 2000; ++guid)
    {
        CHECK(KindOf(settings, MakeBot(guid, true, 0, 9)) != Purpose::Pvp);
        bool const in = KindOf(settings, MakeBot(guid, true, 0, 30)) == Purpose::Pvp;
        CHECK(in == InPvpShare(settings, guid));
        CHECK(in == (KindOf(settings, MakeBot(guid, false, 0, 60)) == Purpose::Pvp));
        pvp += in ? 1 : 0;
    }
    CHECK(pvp > 500 && pvp < 700);   // 30 % of 2000 is 600
    settings.PvpShare = 100;
    CHECK(KindOf(settings, MakeBot(7, true, 0, 10)) == Purpose::Pvp);
    settings.PvpShare = 0;
    CHECK(!InPvpShare(settings, 7));
}

// KindByLevel: leveling below the cap, raiding when raid ready, utility for a capped bot that is not ready yet (pre-raid).
void KindByLevelFollowsLevelAndReadiness()
{
    Settings settings = On();
    settings.KindByLevel = true;
    CHECK(KindOf(settings, MakeBot(1, true, 0, 1)) == Purpose::Leveling);
    CHECK(KindOf(settings, MakeBot(2, true, 0, 45)) == Purpose::Leveling);
    CHECK(KindOf(settings, MakeBot(3, true, 0, 60)) == Purpose::Raiding);    // no readiness check set: the level alone
    settings.RaidQualified = [](Bot const& bot) { return bot.Guid % 2 == 0; };
    CHECK(KindOf(settings, MakeBot(4, true, 0, 60)) == Purpose::Raiding);
    CHECK(KindOf(settings, MakeBot(5, true, 0, 60)) == Purpose::Utility);    // not ready: waits in a utility guild
    settings.LevelCap = 50;
    CHECK(KindOf(settings, MakeBot(6, true, 0, 55)) == Purpose::Utility);    // past the leveling cap, below RaidMinLevel
    settings.PvpShare = 100;
    CHECK(KindOf(settings, MakeBot(6, true, 0, 55)) == Purpose::Pvp);        // the pvp share comes first
    CHECK(KindOf(settings, MakeBot(6, true, 0, 5)) == Purpose::Leveling);    // below PvpMinLevel
}

// Pvp bots form a pvp guild per faction; a later pvp bot joins the existing one; a bot set to pvp below the level joins nothing.
void PvpGuildsFormAndTakeJoiners()
{
    Settings settings = On();
    settings.PvpShare = 100;
    settings.PvpMinLevel = 20;
    std::vector<Bot> pool{ MakeBot(1, true, 0, 25), MakeBot(2, true, 0, 30), MakeBot(3, false, 0, 40), MakeBot(4, true, 0, 12) };
    Plan const plan = PlanMoves(settings, {}, pool);
    std::set<std::string> names;
    for (NewGuild const& created : plan.Created)
        if (created.Kind == Purpose::Pvp)
            names.insert(created.Name);
    CHECK(names.size() == 2);
    CHECK(names.count("BotPvp A001") == 1 && names.count("BotPvp H001") == 1);
    // Bot 4 (level 12) is below PvpMinLevel: share mode gives it its guid share (leveling here), not pvp.
    bool fourInPvp = false;
    for (NewGuild const& created : plan.Created)
        for (std::uint64_t member : created.Members)
            fourInPvp = fourInPvp || (member == 4 && created.Kind == Purpose::Pvp);
    CHECK(!fourInPvp);

    // An existing pvp guild takes a pvp joiner of its faction; a leveling guild does not.
    Guild pvpGuild{ 100, Purpose::Pvp, true };
    Guild lvlGuild{ 101, Purpose::Leveling, true };
    std::vector<Bot> later{ MakeBot(1, true, 100, 25), MakeBot(5, true, 0, 22), MakeBot(6, true, 101, 21) };
    Plan const second = PlanMoves(settings, { pvpGuild, lvlGuild }, later);
    bool fiveJoins = false, sixLeaves = false;
    for (Move const& move : second.Moves)
    {
        if (move.Bot == 5)
            fiveJoins = move.To == 100;
        if (move.Bot == 6)
            sixLeaves = move.From == 101;   // at 21 it wants pvp now: it leaves the leveling guild (a switch is leave, then join)
    }
    CHECK(fiveJoins && sixLeaves);

    // An explicit Want of pvp below the level: nothing to join.
    Bot low = MakeBot(9, true, 0, 15);
    low.Want = Purpose::Pvp;
    CHECK(PlanMoves(settings, {}, { low }).Moves.empty());
}

// KindByLevel: a leveling bot that reaches the cap moves on to raiding (raid ready) and keeps no leveling guild.
void LevelingBotGraduatesToRaiding()
{
    Settings settings = On();
    settings.KindByLevel = true;
    Guild lvl{ 10, Purpose::Leveling, false };
    Guild raid{ 11, Purpose::Raiding, false };
    std::vector<Bot> pool{ MakeBot(1, false, 10, 30), MakeBot(2, false, 10, 60), MakeBot(3, false, 11, 60) };
    Plan const plan = PlanMoves(settings, { lvl, raid }, pool);
    CHECK(plan.Moves.size() == 1);
    if (plan.Moves.size() == 1)
        CHECK(plan.Moves[0].Bot == 2 && plan.Moves[0].From == 10 && plan.Moves[0].To == 11);
}

void ConfigKeys()
{
    std::vector<std::string> warnings;
    Settings defaults = LoadSettings({}, warnings);
    CHECK(warnings.empty() && defaults.PvpMinLevel == 10 && defaults.PvpShare == 0 && !defaults.KindByLevel && !defaults.Remember);
    CHECK(defaults.NameStyles[static_cast<std::size_t>(Purpose::Pvp)][static_cast<std::size_t>(NameStyle::Funny)] == 0);
    CHECK(defaults.NameStyles[static_cast<std::size_t>(Purpose::Raiding)][static_cast<std::size_t>(NameStyle::Lore)] >
          defaults.NameStyles[static_cast<std::size_t>(Purpose::Raiding)][static_cast<std::size_t>(NameStyle::Funny)]);

    // The pvp level follows BattlegroundMinLevel unless GuildsPvpMinLevel is set.
    Settings bg = LoadSettings({ { "BattlegroundMinLevel", "21" } }, warnings);
    CHECK(bg.PvpMinLevel == 21);
    Settings own = LoadSettings({ { "BattlegroundMinLevel", "21" }, { "GuildsPvpMinLevel", "30" } }, warnings);
    CHECK(own.PvpMinLevel == 30);
    CHECK(warnings.empty());

    Settings set = LoadSettings({ { "GuildsKindByLevel", "1" }, { "GuildsPvpShare", "25" }, { "GuildsRemember", "true" },
                                  { "GuildsNameFile", "data/bot_guilds.txt" }, { "GuildsNameStylesPvp", "funny:2, lore:0" } },
                                warnings);
    CHECK(warnings.empty());
    CHECK(set.KindByLevel && set.PvpShare == 25 && set.Remember && set.NameFile == "data/bot_guilds.txt");
    StyleWeights const& pvp = set.NameStyles[static_cast<std::size_t>(Purpose::Pvp)];
    CHECK(pvp[0] == 0 && pvp[1] == 2 && pvp[2] == DefaultStyleWeights(Purpose::Pvp)[2]);   // normal not named: its default stays

    warnings.clear();
    Settings bad = LoadSettings({ { "GuildsPvpShare", "300" }, { "GuildsNameStylesRaiding", "epic:3" } }, warnings);
    CHECK(bad.PvpShare == 100 && warnings.size() == 2);
    CHECK(bad.NameStyles[static_cast<std::size_t>(Purpose::Raiding)] == DefaultStyleWeights(Purpose::Raiding));
}

void NamesParse()
{
    std::vector<std::string> warnings;
    std::string const text =
        "# comment\n"
        "\n"
        "guild|lore|30|The Silver Hand\r\n"
        "guild|funny|10|Pull More Rats\n"
        "guild|normal|5|Night Watch\n"
        "guild|reallife|1|Tuesday Club\n"
        "guild|normal|5|night watch\n"           // duplicate, case ignored
        "guild|epic|5|Bad Style\n"
        "guild|lore|0|Zero Weight\n"
        "guild|lore|101|Too Heavy\n"
        "guild|lore|5|A\n"                       // too short
        "guild|lore|5|Name With Digits 2\n"
        "guild|lore|5|This Name Is Far Too Long Ok\n"
        "name|lore|5|Wrong Tag\n"
        "guild|lore|5\n";
    std::vector<NameEntry> const names = ParseNames(text, warnings);
    CHECK(names.size() == 4);
    CHECK(warnings.size() == 9);
    if (names.size() == 4)
    {
        CHECK(names[0].Name == "The Silver Hand" && names[0].Style == NameStyle::Lore && names[0].Weight == 30);
        CHECK(names[3].Style == NameStyle::RealLife);
    }

    StyleWeights weights{ 1, 1, 1, 1 };
    CHECK(ParseStyleWeights("lore:3 funny:0,normal:2  reallife:7", weights));
    CHECK(weights == (StyleWeights{ 3, 0, 2, 7 }));
    CHECK(!ParseStyleWeights("lore=3", weights) && !ParseStyleWeights("lore:x", weights) && !ParseStyleWeights("lore:", weights) &&
          !ParseStyleWeights("lore:500", weights));
    CHECK(weights == (StyleWeights{ 3, 0, 2, 7 }));   // unchanged by a bad text
    CHECK(NameKey("The Silver Hand") == "the silver hand");
}

void NamesPick()
{
    Settings settings = On();
    std::vector<NameEntry> const names{ { NameStyle::Lore, 10, "Lore One" },     { NameStyle::Funny, 10, "Funny One" },
                                        { NameStyle::Normal, 10, "Normal One" }, { NameStyle::RealLife, 10, "Real One" },
                                        { NameStyle::Lore, 10, "Lore Two" } };
    auto const none = [](std::string const&) { return false; };

    // Pvp never takes funny (weight 0); leveling takes every style; the draw is the same for the same seed.
    std::map<std::string, int> pvpCounts, lvlCounts;
    for (std::uint64_t seed = 0; seed < 4000; ++seed)
    {
        auto const pvp = PickName(settings, names, Purpose::Pvp, none, seed);
        auto const lvl = PickName(settings, names, Purpose::Leveling, none, seed);
        CHECK(pvp && lvl);
        if (pvp)
            ++pvpCounts[*pvp];
        if (lvl)
            ++lvlCounts[*lvl];
        CHECK(PickName(settings, names, Purpose::Pvp, none, seed) == pvp);
    }
    CHECK(pvpCounts.count("Funny One") == 0);
    CHECK(lvlCounts.size() == 5);
    for (auto const& [name, count] : lvlCounts)
        CHECK(count > 600 && count < 1000);   // 1 in 5 of 4000 is 800
    // Raiding: lore and normal weigh 3, funny and real life 1: lore (two rows) 60 of 110.
    int raidLore = 0;
    for (std::uint64_t seed = 0; seed < 4000; ++seed)
    {
        auto const raid = PickName(settings, names, Purpose::Raiding, none, seed);
        raidLore += raid && raid->rfind("Lore", 0) == 0 ? 1 : 0;
    }
    CHECK(raidLore > 1950 && raidLore < 2400);   // 60/110 of 4000 is about 2180

    // Taken names (case ignored, NameKey) are never drawn; nothing left: nullopt; an empty list: nullopt.
    std::set<std::string> taken{ "lore one", "normal one", "real one", "lore two" };
    auto const isTaken = [&](std::string const& key) { return taken.count(key) != 0; };
    for (std::uint64_t seed = 0; seed < 50; ++seed)
        CHECK(PickName(settings, names, Purpose::Leveling, isTaken, seed) == std::optional<std::string>("Funny One"));
    CHECK(!PickName(settings, names, Purpose::Pvp, isTaken, 1));      // only funny left, and pvp does not take funny
    taken.insert("funny one");
    CHECK(!PickName(settings, names, Purpose::Leveling, isTaken, 1));
    CHECK(!PickName(settings, {}, Purpose::Leveling, none, 1));
}

// The shipped names file parses without a warning and has names for every kind.
void ShippedNamesFile()
{
    std::ifstream in(FOREVER_BOT_GUILDS_FILE);
    CHECK(static_cast<bool>(in));
    std::ostringstream text;
    text << in.rdbuf();
    std::vector<std::string> warnings;
    std::vector<NameEntry> const names = ParseNames(text.str(), warnings);
    for (std::string const& warning : warnings)
        std::printf("names file: %s\n", warning.c_str());
    CHECK(warnings.empty());
    CHECK(names.size() > 300);
    Settings const settings = On();
    for (Purpose kind : { Purpose::Leveling, Purpose::Utility, Purpose::Raiding, Purpose::Pvp })
        CHECK(PickName(settings, names, kind, nullptr, 42).has_value());
}

void StoreRows()
{
    std::uint64_t guild = 0;
    CHECK(StoreKeyFor(77) == "guild/77");
    CHECK(GuildFromStoreKey("guild/77", guild) && guild == 77);
    CHECK(!GuildFromStoreKey("guild/", guild) && !GuildFromStoreKey("guild/0", guild) && !GuildFromStoreKey("guild/7x", guild) &&
          !GuildFromStoreKey("bot/77", guild));

    for (Purpose kind : { Purpose::Leveling, Purpose::Utility, Purpose::Raiding, Purpose::Pvp })
        for (bool alliance : { true, false })
        {
            Guild const made{ 77, kind, alliance };
            std::string const value = EncodeRecord(made, "The Silver Hand");
            Purpose back = Purpose::Leveling;
            bool side = !alliance;
            std::string name;
            CHECK(DecodeRecord(value, back, side, name));
            CHECK(back == kind && side == alliance && name == "The Silver Hand");
        }
    CHECK(EncodeRecord(Guild{ 5, Purpose::Pvp, false }, "Night Watch") == "pvp H Night Watch");
    Purpose kind = Purpose::Leveling;
    bool alliance = false;
    std::string name = "x";
    CHECK(DecodeRecord("raiding A", kind, alliance, name) && kind == Purpose::Raiding && alliance && name.empty());
    CHECK(!DecodeRecord("", kind, alliance, name) && !DecodeRecord("epic A Name", kind, alliance, name) && !DecodeRecord("pvp X Name", kind, alliance, name) &&
          !DecodeRecord("pvp AName", kind, alliance, name) && !DecodeRecord("pvp", kind, alliance, name));
    CHECK(KindFromName("pvp") == Purpose::Pvp && !KindFromName("Pvp"));
}

}  // namespace

int main()
{
    DefaultsKeepTheGuidShares();
    PvpShareAboveTheMinLevel();
    KindByLevelFollowsLevelAndReadiness();
    PvpGuildsFormAndTakeJoiners();
    LevelingBotGraduatesToRaiding();
    ConfigKeys();
    NamesParse();
    NamesPick();
    ShippedNamesFile();
    StoreRows();
    if (failures == 0)
        std::printf("bot_guild_kinds_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
