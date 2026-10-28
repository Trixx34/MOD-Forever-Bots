// Profession trainers in the capitals for the town errands (data/errand_trainers.txt), the training-due rule and the per-bot choice of a trainer.
// Pure logic: no core, no clock, no file access. The module reads the file and passes its text to ParseTrainerTable (tests/errand_trainers_test.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace errand {

// Capital ids, as used in errand::Bot::Cities and the trainer table.
constexpr std::uint32_t kStormwind = 1;
constexpr std::uint32_t kIronforge = 2;
constexpr std::uint32_t kDarnassus = 3;
constexpr std::uint32_t kUndercity = 4;
constexpr std::uint32_t kOrgrimmar = 5;
constexpr std::uint32_t kThunderBluff = 6;

// The capital id for a name as the table spells it ("Stormwind", "ThunderBluff", ...), 0 when unknown.
std::uint32_t CityOf(std::string_view name);

// The name for a capital id, the spelling CityOf takes ("Stormwind"); "city<id>" for an id that is not a capital.
std::string CityName(std::uint32_t city);

// Sides as the table writes them: 1 Alliance, 2 Horde.
constexpr std::uint32_t kAlliance = 1;
constexpr std::uint32_t kHorde = 2;

struct Trainer
{
    std::uint32_t Side = 0;
    std::uint32_t City = 0;
    std::uint32_t Map = 0;
    std::uint32_t Entry = 0;
    std::uint32_t Skill = 0;        // the profession skill line id
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    std::string Name;
    std::string Subname;
    std::uint64_t Guid = 0;         // the spawn guid
};

// Parses the table: one trainer per line, "side|city|map|entry|skill|x y z|name|subname|profession|guid". Blank lines and lines starting with '#' are
// skipped. A malformed line is left out and described in `errors` ("line N: ..."), so one bad row does not drop the table.
std::vector<Trainer> ParseTrainerTable(std::string_view text, std::vector<std::string>& errors);

// The training-due rule. A bot's two primary professions are the slots with a skill line (profession_skill != 0); a slot is due when its value is
// below its maximum. Returns the first due slot, or -1 when none is due.
int TrainingDueSlot(std::uint16_t const skill[2], std::uint16_t const value[2], std::uint16_t const max[2]);

// The trainer a bot goes to for a skill in a capital: the trainer of its side and skill in that city, nearest on the bot's map first, else the
// nearest of the city. Returns the index into `trainers`, or nullopt when the city has none for that skill (the bot is not sent).
std::optional<std::size_t> ChooseTrainer(std::vector<Trainer> const& trainers, std::uint32_t side, std::uint32_t skill, std::uint32_t city,
                                         std::uint32_t map, float x, float y, float z);

} // namespace errand
