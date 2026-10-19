// Unit tests for the errand trainer table, the training-due rule and the per-bot trainer choice (src/errand_trainers.h). No core needed.
#include "errand_trainers.h"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace errand;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

constexpr char kTable[] =
    "# a comment\n"
    "\n"
    "1|Stormwind|0|1218|182|-9059.7 148.6 115.2|Herbalist Pomeroy|Herbalism Trainer|Herbalism|20000012\n"
    "2|Ironforge|0|4258|164|-4788.0 -1129.0 498.9|Bengus Deepforge|Artisan Blacksmith|Blacksmithing|20002013\r\n"
    "1|Stormwind|0|5513|186|-8434.0 693.4 103.4|Gelman Stonehand|Mining Trainer|Mining|20037606\n";

void ParsesGoodRowsAndSkipsCommentsAndBlanks()
{
    std::vector<std::string> errors;
    std::vector<Trainer> const t = ParseTrainerTable(kTable, errors);
    CHECK(errors.empty());
    CHECK(t.size() == 3);
    if (t.size() != 3)
        return;
    CHECK(t[0].Side == kAlliance && t[0].City == kStormwind && t[0].Map == 0);
    CHECK(t[0].Entry == 1218 && t[0].Skill == 182 && t[0].Guid == 20000012);
    CHECK(t[0].Name == "Herbalist Pomeroy" && t[0].Subname == "Herbalism Trainer");
    CHECK(t[0].X > -9059.8f && t[0].X < -9059.6f && t[0].Z > 115.1f && t[0].Z < 115.3f);
    CHECK(t[1].Side == kHorde && t[1].City == kIronforge); // CRLF line ends are trimmed
    CHECK(t[1].Guid == 20002013);
}

void BadRowsAreReportedAndLeftOut()
{
    constexpr char bad[] =
        "3|Stormwind|0|1218|182|0 0 0|X|Y|Herbalism|1\n"       // side
        "1|Gotham|0|1218|182|0 0 0|X|Y|Herbalism|1\n"           // city
        "1|Stormwind|2|1218|182|0 0 0|X|Y|Herbalism|1\n"        // map
        "1|Stormwind|0|0|182|0 0 0|X|Y|Herbalism|1\n"           // entry
        "1|Stormwind|0|1218|0|0 0 0|X|Y|Herbalism|1\n"          // skill
        "1|Stormwind|0|1218|182|0 0|X|Y|Herbalism|1\n"          // position
        "1|Stormwind|0|1218|182|0 0 nan|X|Y|Herbalism|1\n"      // non-finite
        "1|Stormwind|0|1218|182|0 0 0|X|Y|Herbalism|-1\n"       // guid
        "1|Stormwind|0|1218|182|0 0 0|X|Y\n"                    // too few fields
        "2|Undercity|0|4596|164|1681.9 285.5 -62.1|James Van Brunt|Expert Blacksmith|Blacksmithing|20041843\n";
    std::vector<std::string> errors;
    std::vector<Trainer> const t = ParseTrainerTable(bad, errors);
    CHECK(errors.size() == 9);
    CHECK(t.size() == 1 && t[0].Entry == 4596 && t[0].City == kUndercity);
    if (!errors.empty())
        CHECK(errors[0].rfind("line 1:", 0) == 0);
}

void CityNamesAreMapped()
{
    CHECK(CityOf("Stormwind") == kStormwind && CityOf("Ironforge") == kIronforge && CityOf("Darnassus") == kDarnassus);
    CHECK(CityOf("Undercity") == kUndercity && CityOf("Orgrimmar") == kOrgrimmar && CityOf("ThunderBluff") == kThunderBluff);
    CHECK(CityOf("Shattrath") == 0);
}

void TrainingDueRule()
{
    std::uint16_t const skill[2] = { 164, 0 };
    std::uint16_t const value[2] = { 150, 0 };
    std::uint16_t const max[2] = { 300, 0 };
    CHECK(TrainingDueSlot(skill, value, max) == 0);                       // a primary below its maximum is due
    std::uint16_t const full[2] = { 300, 0 };
    CHECK(TrainingDueSlot(skill, full, max) == -1);                       // at its maximum: not due
    std::uint16_t const none[2] = { 0, 0 };
    CHECK(TrainingDueSlot(none, value, max) == -1);                       // no primary profession: not due
    std::uint16_t const second[2] = { 0, 186 };
    std::uint16_t const secondValue[2] = { 0, 75 };
    std::uint16_t const secondMax[2] = { 0, 150 };
    CHECK(TrainingDueSlot(second, secondValue, secondMax) == 1);          // the slot without a skill is skipped; the second is due
    std::uint16_t const onlyEmpty[2] = { 0, 0 };
    CHECK(TrainingDueSlot(onlyEmpty, onlyEmpty, onlyEmpty) == -1);
    std::uint16_t const both[2] = { 164, 186 };
    std::uint16_t const bothValue[2] = { 300, 75 };
    std::uint16_t const bothMax[2] = { 300, 150 };
    CHECK(TrainingDueSlot(both, bothValue, bothMax) == 1);                // the first slot is done, the second is due
}

void TrainerChoice()
{
    std::vector<std::string> errors;
    std::vector<Trainer> const t = ParseTrainerTable(
        "1|Stormwind|0|1|164|0 0 0|Far|X|Y|10\n"
        "1|Stormwind|0|2|164|100 0 0|Near|X|Y|11\n"
        "1|Stormwind|1|3|164|5 0 0|Other map|X|Y|12\n"
        "2|Orgrimmar|1|4|164|5 0 0|Horde|X|Y|13\n"
        "1|Stormwind|0|5|171|1 0 0|Alchemy|X|Y|14\n",
        errors);
    CHECK(errors.empty() && t.size() == 5);
    // Same map and nearest wins, even if another trainer is on another map closer.
    auto const near = ChooseTrainer(t, kAlliance, 164, kStormwind, 0, 90.0f, 0.0f, 0.0f);
    CHECK(near && t[*near].Entry == 2);
    // A bot on the other map gets the trainer on its map.
    auto const onMap = ChooseTrainer(t, kAlliance, 164, kStormwind, 1, 0.0f, 0.0f, 0.0f);
    CHECK(onMap && t[*onMap].Entry == 3);
    // Only the bot's side, skill and city count.
    auto const horde = ChooseTrainer(t, kHorde, 164, kOrgrimmar, 0, 0.0f, 0.0f, 0.0f);
    CHECK(horde && t[*horde].Entry == 4);
    CHECK(!ChooseTrainer(t, kHorde, 164, kStormwind, 0, 0.0f, 0.0f, 0.0f));
    CHECK(!ChooseTrainer(t, kAlliance, 165, kStormwind, 0, 0.0f, 0.0f, 0.0f));
    // A trainer of another city is not chosen for this one.
    CHECK(!ChooseTrainer(t, kAlliance, 171, kIronforge, 0, 0.0f, 0.0f, 0.0f));
}

void ShippedTableParses()
{
    std::ifstream in(FOREVER_ERRAND_TRAINERS_FILE);
    CHECK(in.good());
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::vector<std::string> errors;
    std::vector<Trainer> const t = ParseTrainerTable(buffer.str(), errors);
    CHECK(errors.empty());
    CHECK(t.size() == 90);
    std::size_t alliance = 0;
    for (Trainer const& trainer : t)
        alliance += trainer.Side == kAlliance;
    CHECK(alliance == 44 && t.size() - alliance == 46);
    for (Trainer const& trainer : t)
        CHECK(trainer.Subname.find("Junior") == std::string::npos);
}

} // namespace

int main()
{
    ParsesGoodRowsAndSkipsCommentsAndBlanks();
    BadRowsAreReportedAndLeftOut();
    CityNamesAreMapped();
    TrainingDueRule();
    TrainerChoice();
    ShippedTableParses();
    if (failures == 0)
        std::printf("errand_trainers_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
