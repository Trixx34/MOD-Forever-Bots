// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for bot-to-bot mail (src/bot_mail.h). No core or SDK needed.
#include "bot_mail.h"

#include <cstdio>

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
}  // namespace

using namespace botmail;

namespace {

Member Bot(std::uint64_t guid, std::uint64_t guild, std::uint64_t copper, std::uint16_t s0 = 0, std::uint16_t s1 = 0)
{
    Member m;
    m.Guid = guid;
    m.Guild = guild;
    m.Side = 1;
    m.ClassId = 8;
    m.Level = 30;
    m.Copper = copper;
    m.Skill = { s0, s1 };
    m.Free = true;
    return m;
}

Settings On()
{
    Settings s;
    s.Enabled = true;
    return s;
}

gear_score::Item Armor(std::uint32_t ilvl, std::uint32_t inv, std::uint32_t sub, std::uint32_t req)
{
    gear_score::Item item;
    item.Level = ilvl;
    item.Quality = 2;
    item.RequiredLevel = req;
    item.InventoryType = inv;
    item.ArmorSubclass = sub;
    return item;
}

}  // namespace

int main()
{
    // Off by default: nothing is planned.
    {
        State state;
        std::vector<Member> pool{ Bot(1, 7, 900000), Bot(2, 7, 0) };
        CHECK(Plan(Settings{}, state, pool, 0).empty());
        CHECK(!Settings{}.Enabled);
    }
    // Gold: a rich bot gives to a poor guildmate, within the maximum, keeping its reserve.
    {
        State state;
        std::vector<Member> pool{ Bot(1, 7, 200000), Bot(2, 7, 1000), Bot(3, 8, 0) };
        std::vector<Offer> const offers = Plan(On(), state, pool, 1000);
        CHECK(offers.size() == 1);
        if (offers.size() == 1)
        {
            CHECK(offers[0].What == Kind::Gold && offers[0].Sender == 1 && offers[0].Recipient == 2);   // guild 8's bot is not a guildmate
            CHECK(offers[0].Copper == 10000);   // capped by GoldMaxCopper (the core's default gift cap)
        }
    }
    // A sender under its reserve, a different faction and a bot in no guild give or get nothing.
    {
        State state;
        std::vector<Member> pool{ Bot(1, 7, 55000), Bot(2, 7, 0) };   // surplus 5000 < GoldMinCopper
        CHECK(Plan(On(), state, pool, 0).empty());
        State s2;
        std::vector<Member> sides{ Bot(1, 7, 900000), Bot(2, 7, 0) };
        sides[1].Side = 0;
        CHECK(Plan(On(), s2, sides, 0).empty());
        State s3;
        CHECK(Plan(On(), s3, { Bot(1, 0, 900000), Bot(2, 0, 0) }, 0).empty());
        State s4;
        std::vector<Member> busy{ Bot(1, 7, 900000), Bot(2, 7, 0) };
        busy[0].Free = false;
        CHECK(Plan(On(), s4, busy, 0).empty());
    }
    // Caps and cooldown: the plan period, the sender's cooldown, the sender's and the recipient's daily caps.
    {
        Settings s = On();
        s.DailyCap = 2;
        s.CooldownSec = 600;
        s.PlanEverySec = 60;
        s.RecipientDailyCap = 1;
        State state;
        std::vector<Member> pool{ Bot(1, 7, 900000), Bot(2, 7, 0), Bot(3, 7, 0) };
        CHECK(Plan(s, state, pool, 0).size() == 1);
        CHECK(Plan(s, state, pool, 10000).empty());            // before the next plan
        CHECK(Plan(s, state, pool, 61000).empty());            // sender 1 is on cooldown; 2 and 3 have no money
        std::vector<Member> second{ Bot(1, 7, 900000), Bot(2, 7, 0), Bot(3, 7, 0) };
        std::vector<Offer> const later = Plan(s, state, second, 700000);
        CHECK(later.size() == 1 && later[0].Recipient == 3);    // bot 2 got its one mail today
        CHECK(Plan(s, state, second, 1400000).empty());         // the sender's cooldown is over but 2 and 3 are both at the recipient cap
        CHECK(Plan(s, state, second, kDayMs + 2000000).size() == 1);   // a new day
    }
    // A refusal gives the slots back.
    {
        Settings s = On();
        s.DailyCap = 1;
        s.RecipientDailyCap = 1;
        s.CooldownSec = 60;
        State state;
        std::vector<Member> pool{ Bot(1, 7, 900000), Bot(2, 7, 0) };
        CHECK(Plan(s, state, pool, 0).size() == 1);
        CHECK(Plan(s, state, pool, 200000).empty());
        NoteFailed(state, 1, 2);
        CHECK(Plan(s, state, pool, 400000).size() == 1);
    }
    // Materials: a gatherer sends to a guildmate whose profession uses the goods, never to one with the same professions.
    {
        State state;
        std::vector<Member> pool{ Bot(1, 7, 0, kHerbalism, kMining), Bot(2, 7, 0, kAlchemy, kTailoring), Bot(3, 7, 0, kMining, kSkinning) };
        std::vector<Offer> const offers = Plan(On(), state, pool, 0);
        CHECK(!offers.empty());
        for (Offer const& o : offers)
            CHECK(o.What == Kind::Materials && o.Recipient != 3 && o.Recipient != o.Sender);
        Settings off = On();
        off.Materials = false;
        State s2;
        CHECK(Plan(off, s2, pool, 0).empty());
    }
    // PickMaterial: a whole known stack the recipient uses and the sender does not; the biggest wins; nothing else is picked.
    {
        Settings s = On();
        std::vector<Stack> stacks{ { 1, 765, 12, "Silverleaf" }, { 2, 2589, 40, "Linen Cloth" }, { 3, 2447, 20, "Peacebloom" }, { 4, 999999, 30, "Unknown" },
                                   { 5, 2770, 3, "Copper Ore" }, { 0, 2447, 50, "Ghost" }, { 6, 2318, 300, "Light Leather" } };
        CHECK(PickMaterial(s, stacks, { kAlchemy, 0 }, { kMining, 0 }) == 2);        // Peacebloom (20) beats Silverleaf (12)
        CHECK(PickMaterial(s, stacks, { kTailoring, 0 }, { kMining, 0 }) == 1);
        CHECK(PickMaterial(s, stacks, { kAlchemy, 0 }, { kHerbalism, kAlchemy }) == -1);   // the sender uses it itself
        CHECK(PickMaterial(s, stacks, { kBlacksmithing, 0 }, { 0, 0 }) == -1);       // 3 copper ore is under the minimum
        CHECK(PickMaterial(s, stacks, { kLeatherworking, 0 }, { 0, 0 }) == -1);      // 300 is over the maximum
        CHECK(ConsumersOf(999999).empty() && !ConsumersOf(2770).empty());
    }
    CHECK(GoldSubject(10000) == "Some coin for you: 1g");
    CHECK(GoldSubject(12345) == "Some coin for you: 1g 23s 45c");
    CHECK(MaterialSubject(std::string(100, 'x')).size() == 64);

    // Gear: the upgrade is judged by score, the class and level; the sender's own use, a bound item and a quest item stop it.
    {
        CHECK(BestArmorSubclass(1, 20) == 3 && BestArmorSubclass(1, 40) == 4 && BestArmorSubclass(8, 60) == 1 && BestArmorSubclass(11, 60) == 2 &&
              BestArmorSubclass(3, 39) == 2 && BestArmorSubclass(3, 40) == 3);
        gear_score::Item mail = Armor(40, 5, 3, 30);   // chest, mail
        GearCandidate warrior;
        warrior.Guid = 10; warrior.ClassId = 1; warrior.Level = 35;
        warrior.Worn = { Armor(30, 5, 3, 25) };
        GearCandidate mage;
        mage.Guid = 11; mage.ClassId = 8; mage.Level = 35;
        mage.Worn = { Armor(10, 5, 1, 5) };             // would gain more, but cannot wear mail
        GearCandidate lowWarrior = warrior;
        lowWarrior.Guid = 12; lowWarrior.Level = 20;     // below the required level
        GearCandidate sameLevel = warrior;
        sameLevel.Guid = 13; sameLevel.Worn = { Armor(40, 5, 3, 30) };   // not strictly better by the margin
        std::vector<GearCandidate> all{ mage, lowWarrior, sameLevel, warrior };
        CHECK(PickGearRecipient(mail, false, false, all) == 3);
        CHECK(PickGearRecipient(mail, true, false, all) == -1);          // bound
        CHECK(PickGearRecipient(mail, false, true, all) == -1);          // the sender would wear it
        gear_score::Item quest = mail;
        quest.QuestItem = true;
        CHECK(PickGearRecipient(quest, false, false, all) == -1);
        GearCandidate empty = warrior;
        empty.Guid = 9; empty.Worn.clear();               // an empty slot is an upgrade; the lowest guid wins a tie of gains only
        std::vector<GearCandidate> withEmpty{ warrior, empty };
        CHECK(PickGearRecipient(mail, false, false, withEmpty) == 1);
        CHECK(PickGearRecipient(Armor(40, 0, 0, 0), false, false, all) == -1);   // not equipment
    }

    if (failures == 0) std::printf("bot_mail: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
