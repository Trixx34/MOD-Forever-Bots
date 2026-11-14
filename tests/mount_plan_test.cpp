// SPDX-License-Identifier: GPL-2.0-or-later
// The mount plan: the vendor table, the choice of a mount, the riding step before it (src/mount_plan.h), and the shipped data files.
#include "mount_plan.h"
#include "errand_trainers.h"
#include "riding_training.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

constexpr std::uint64_t kGold = 10000;

char const* const kTable =
    "# comment\n"
    "vendor|1|0|384|1|-9455.4 -1385.3 47.2|Katie|Horse Breeder\n"
    "vendor|4|0|1261|2|-5537.0 -1323.7 399.2|Veron|Ram Breeder\n"
    "vendor|1|1|4885|3|-3835.7 -4397.7 11.0|Gregor|Horse Breeder\n"
    "item|384|2414|40|75|200000|Pinto Bridle\n"
    "item|384|18777|60|150|2000000|Swift Brown Steed\n"
    "item|1261|5864|40|75|200000|Gray Ram\n"
    "item|4885|5655|40|75|200000|Chestnut Mare Bridle\n"
    "item|999|1|40|75|1|Orphan (no vendor line)\n"
    "bad line\n"
    "item|384|x|40|75|1|Broken\n";

std::vector<riding::Rank> Ranks()
{
    std::vector<std::string> errors;
    return riding::ParseRanks("33388|40|75|900000|Apprentice\n33391|60|150|9000000|Journeyman\n", errors);
}

mounts::Table Table()
{
    std::vector<std::string> errors;
    return mounts::ParseTable(kTable, errors);
}

void TheTableParsesAndBadLinesAreDescribed()
{
    std::vector<std::string> errors;
    mounts::Table const table = mounts::ParseTable(kTable, errors);
    CHECK(table.Vendors.size() == 3);
    CHECK(table.Items.size() == 4);          // the orphan is dropped; the broken lines are described
    CHECK(errors.size() == 2);
    CHECK(table.Vendors[0].Entry == 384 && table.Vendors[0].RaceMask == 1 && table.Vendors[0].Map == 0 && table.Vendors[0].Guid == 1);
    CHECK(table.Items[1].Entry == 18777 && table.Items[1].Level == 60 && table.Items[1].Skill == 150 && table.Items[1].Copper == 2000000);
}

void RaceBitsAndSpendable()
{
    CHECK(mounts::RaceBit(1) == 1 && mounts::RaceBit(8) == 128 && mounts::RaceBit(0) == 0 && mounts::RaceBit(95) == 0);
    CHECK(mounts::Spendable(100000, 40, 1000) == 60000);
    CHECK(mounts::Spendable(10, 40, 1000) == 0);
}

void ChooseMountNeedsRaceMapLevelSkillAndMoney()
{
    mounts::Table const table = Table();
    // A human on map 0 at level 45 with the first rank and 30 gold: the Pinto Bridle at Katie's.
    auto pick = mounts::ChooseMount(table, 1, 45, 75, 300000, 0, 0.0f, 0.0f);
    CHECK(pick && table.Items[pick->Item].Entry == 2414 && table.Vendors[pick->Vendor].Entry == 384);
    CHECK(!mounts::ChooseMount(table, 1, 45, 75, 199999, 0, 0.0f, 0.0f));      // short of copper
    CHECK(!mounts::ChooseMount(table, 1, 45, 0, 300000, 0, 0.0f, 0.0f));       // no riding skill
    CHECK(!mounts::ChooseMount(table, 1, 39, 75, 300000, 0, 0.0f, 0.0f));      // too low
    CHECK(!mounts::ChooseMount(table, 4, 45, 75, 300000, 0, 0.0f, 0.0f));      // no vendor for a Night Elf (race 4) in this table
    CHECK(!mounts::ChooseMount(table, 0, 45, 75, 300000, 0, 0.0f, 0.0f));      // an unknown race
    pick = mounts::ChooseMount(table, 1, 45, 75, 300000, 1, 0.0f, 0.0f);       // on the other map: the horse breeder there
    CHECK(pick && table.Items[pick->Item].Entry == 5655);
    // At 60 with Journeyman and 250 gold: the swift steed beats the bridle (higher level first).
    pick = mounts::ChooseMount(table, 1, 60, 150, 2500000, 0, 0.0f, 0.0f);
    CHECK(pick && table.Items[pick->Item].Entry == 18777);
    // With only 30 gold the cheaper one is chosen.
    pick = mounts::ChooseMount(table, 1, 60, 150, 300000, 0, 0.0f, 0.0f);
    CHECK(pick && table.Items[pick->Item].Entry == 2414);
}

void CheapestMountFollowsTheRankAndLevel()
{
    mounts::Table const table = Table();
    CHECK(mounts::CheapestMount(table, 1, 40, 75) == 200000);
    CHECK(mounts::CheapestMount(table, 1, 40, 0) == 0);
    CHECK(mounts::CheapestMount(table, 1, 39, 75) == 0);
    CHECK(mounts::CheapestMount(table, 3, 40, 75) == 200000);                  // the dwarf's ram
    CHECK(mounts::CheapestMount(table, 4, 60, 150) == 0);
}

void SkillAfterTrainingPaysRanksInOrder()
{
    auto const ranks = Ranks();
    CHECK(mounts::SkillAfterTraining(ranks, 39, 0, 100000000) == 0);
    CHECK(mounts::SkillAfterTraining(ranks, 40, 0, 899999) == 0);
    CHECK(mounts::SkillAfterTraining(ranks, 45, 0, 900000) == 75);
    CHECK(mounts::SkillAfterTraining(ranks, 60, 0, 9000000) == 75);            // the second rank is not paid for after the first
    CHECK(mounts::SkillAfterTraining(ranks, 60, 0, 9900000) == 150);
    CHECK(mounts::SkillAfterTraining(ranks, 60, 75, 9000000) == 150);
}

void DecideTrainsThenBuysAndOnlyAfterARefusal()
{
    auto const ranks = Ranks();
    mounts::Table const table = Table();
    // A human at 40 refused a mount, with 130 gold (reserve 5000 copper a level = 20 gold): trains (90 gold) and keeps the 20 gold for the bridle.
    auto plan = mounts::Decide(ranks, table, true, 1, 40, 0, 1300000, 5000, 0, 0.0f, 0.0f);
    CHECK(plan.What == mounts::Step::Train && plan.Rank == 0);
    plan = mounts::Decide(ranks, table, false, 1, 40, 0, 1300000, 5000, 0, 0.0f, 0.0f);      // not refused: nothing
    CHECK(plan.What == mounts::Step::None);
    plan = mounts::Decide(ranks, table, true, 1, 40, 0, 1000000, 5000, 0, 0.0f, 0.0f);       // 100 gold: not enough for rank and bridle and reserve
    CHECK(plan.What == mounts::Step::None);
    plan = mounts::Decide(ranks, table, true, 1, 40, 75, 1000000, 5000, 0, 0.0f, 0.0f);      // has the rank: buys the bridle
    CHECK(plan.What == mounts::Step::Buy && table.Items[plan.Mount.Item].Entry == 2414);
    plan = mounts::Decide(ranks, table, true, 1, 40, 75, 300000, 5000, 0, 0.0f, 0.0f);       // 30 gold, reserve 20: not enough
    CHECK(plan.What == mounts::Step::None);
    plan = mounts::Decide(ranks, table, true, 95, 60, 75, 99999999, 5000, 0, 0.0f, 0.0f);    // a race with no vendor: nothing
    CHECK(plan.What == mounts::Step::None);
    // At 60 with the first rank and 1100 gold: trains the second rank, then the steed follows.
    plan = mounts::Decide(ranks, table, true, 1, 60, 75, 11000000, 5000, 0, 0.0f, 0.0f);
    CHECK(plan.What == mounts::Step::Train && plan.Rank == 1);
    plan = mounts::Decide(ranks, table, true, 1, 60, 150, 2600000, 5000, 0, 0.0f, 0.0f);
    CHECK(plan.What == mounts::Step::Buy && table.Items[plan.Mount.Item].Entry == 18777);
}

std::string Slurp(std::string const& path)
{
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void TheShippedFilesParseAndAgree()
{
    std::string const dir = MODULE_DATA_DIR;
    std::vector<std::string> errors;
    mounts::Table const table = mounts::ParseTable(Slurp(dir + "/mount_vendors.txt"), errors);
    CHECK(errors.empty());
    CHECK(table.Vendors.size() >= 10 && table.Items.size() >= 50);
    for (mounts::Item const& item : table.Items)
        CHECK(item.Level >= 40 && (item.Skill == 75 || item.Skill == 150) && (item.Level < 60 ? item.Skill == 75 : item.Skill == 150));
    // Every race a bot can be (1..8) has a mount vendor.
    for (std::uint32_t race = 1; race <= 8; ++race)
        CHECK(mounts::CheapestMount(table, race, 60, 150) != 0);
    std::vector<std::string> rankErrors;
    auto const ranks = riding::ParseRanks(Slurp(dir + "/riding_ranks.txt"), rankErrors);
    CHECK(rankErrors.empty() && ranks.size() == 2 && ranks[0].Skill == 75 && ranks[1].Skill == 150 && ranks[0].CostCopper < ranks[1].CostCopper);
    std::vector<std::string> trainerErrors;
    auto const trainers = errand::ParseTrainerTable(Slurp(dir + "/riding_trainers.txt"), trainerErrors);
    CHECK(trainerErrors.empty() && trainers.size() == 7);
    int alliance = 0, horde = 0;
    for (errand::Trainer const& t : trainers)
    {
        CHECK(t.Skill == 762);
        alliance += t.Side == errand::kAlliance;
        horde += t.Side == errand::kHorde;
    }
    CHECK(alliance == 3 && horde == 4);
}

} // namespace

int main()
{
    TheTableParsesAndBadLinesAreDescribed();
    RaceBitsAndSpendable();
    ChooseMountNeedsRaceMapLevelSkillAndMoney();
    CheapestMountFollowsTheRankAndLevel();
    SkillAfterTrainingPaysRanksInOrder();
    DecideTrainsThenBuysAndOnlyAfterARefusal();
    TheShippedFilesParseAndAgree();
    if (failures == 0)
        std::printf("mount_plan_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
