// SPDX-License-Identifier: GPL-2.0-or-later
// Profession progression (2026-11-20, Professions, default off; doc/professions.md). Prod, 800 bots at average level 16.2: Mining, Skinning, Enchanting, Alchemy,
// Fishing at about 1, Herbalism 5, Blacksmithing 2, Engineering 2.6, Leatherworking 6.7, Tailoring 28.8; only Cooking (37.8) and First Aid (58.8) progress. The causes
// (doc/professions.md): Gathering is off on prod, so Mining, Herbalism and Skinning never gather and the crafting lines never get ore, herbs or leather; the core's
// kSkillUp chooses only among recipes whose materials the bot carries and never names a missing reagent, so the supply trip never buys thread or vials; skinning
// shares the "nothing near" back-off of the node search (up to 48 minutes), so the corpses a bot makes go unskinned; a gather walk that arrives at an empty pooled
// node takes the same node again (it is still the nearest).
//
// What Professions adds (each part only with Professions = 1):
//  - a skill cap per profession: SkillCapPerLevel x level (default 5 x level), never above the profession's maximum; no new ask once a line reaches it;
//  - a skinning ask right after the bot loots a corpse it fought (SkinAfterLoot), whatever the node back-off says;
//  - node walks near the bot (NodeWalk): at most NodeMaxYards away, not in a place too high for the bot (level_safe, the gather row's area level) or dangerous
//    (danger_map), along a straight line that is clear of both, never to a node found empty in the last NodeEmptyMs (shared by all bots), not while grouped;
//  - the craft plan (CraftPlan): when the core's kSkillUp finds no recipe with materials for a crafting profession, the bot names (kCraft) the easiest recipe of
//    RecipeFile that still raises its skill; the core then notes the first missing reagent and a supply trip may buy it (SupplyReagents);
//  - an event line for every step: profession_skillup, profession_cap, profession_skin, profession_node_walk, profession_node_empty, profession_craft_plan.
// Cooking and First Aid are unchanged.
//
// Pure logic: no core, no clock, no file access (tests/professions_test.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gather { struct NodePoint; struct NodeFilter; }

namespace professions
{
constexpr std::uint32_t kMining = 186;
constexpr std::uint32_t kHerbalism = 182;
constexpr std::uint32_t kSkinning = 393;

struct Options
{
    bool Enabled = false;                 // Professions: the master switch of everything below (off: today's behaviour, no new line)
    std::uint32_t SkillCapPerLevel = 5;   // ProfessionSkillCapPerLevel (1..75): no new ask for a line whose value is at this x level or above
    bool SkinAfterLoot = true;            // ProfessionSkinAfterLoot: ask to skin right after looting a corpse the bot fought
    bool NodeWalk = true;                 // ProfessionNodeWalk: walk to a safe node near the bot (needs GatherNodesFile)
    float NodeMaxYards = 150.0f;          // ProfessionNodeMaxYards (20..600): the farthest node a walk goes to
    std::uint64_t NodeGapMs = 300000;     // ProfessionNodeGapSec (0..86400, default 300): no new node walk for this long after one ends
    std::uint64_t NodeEmptyMs = 1200000;  // ProfessionNodeEmptyMin (1..1440, default 20): a node found empty is skipped by every bot for this long
    bool CraftPlan = true;                // ProfessionCraftPlan: name the easiest recipe of the table when kSkillUp had nothing (needs ProfessionRecipeFile)
};

// The skill a bot may reach in a profession now: perLevel x level, at most `professionMax` when that is known (non-zero). perLevel 0 = no cap of its own.
std::uint32_t SkillCap(std::uint32_t level, std::uint32_t perLevel, std::uint32_t professionMax);
// value below the cap. Level 0 (unknown) never caps.
bool BelowCap(std::uint32_t value, std::uint32_t level, std::uint32_t perLevel, std::uint32_t professionMax);

// A rise of a profession's value between two snapshots: the event profession_skillup.
struct SkillUp
{
    std::uint32_t Skill = 0;
    std::uint32_t From = 0;
    std::uint32_t To = 0;
};
// The rises of the two primary professions (slot by slot): a slot whose skill line is the same and whose value went up. A first sight (previous value 0), a
// changed skill line or a value that went down is no rise.
std::vector<SkillUp> SkillUps(std::uint16_t const prevSkill[2], std::uint16_t const prevValue[2], std::uint16_t const skill[2], std::uint16_t const value[2]);
// "profession_skillup: bot=G skill=S from=A to=B"
std::string SkillUpLine(std::uint64_t bot, SkillUp const& up);

// One recipe of data/profession_recipes.txt (tools/gen_profession_recipes.py).
struct Recipe
{
    std::uint32_t Skill = 0;
    std::uint32_t Spell = 0;
    std::uint32_t MinSkill = 0;      // the core's MinSkillLineRank: below it the recipe gives no skill
    std::uint32_t TrivialLow = 0;    // yellow from here
    std::uint32_t TrivialHigh = 0;   // grey from here: no skill any more
    std::vector<std::pair<std::uint32_t, std::uint32_t>> Reagents;   // item entry, count
};
// "skill spell minSkill trivialLow trivialHigh reagent:count,..." per line; '#' starts a comment (also after a row); blank lines are skipped. A malformed line is
// left out and described in `errors` ("line N: ...").
std::vector<Recipe> ParseRecipeTable(std::string_view text, std::vector<std::string>& errors);
// The recipe to name for `skill` at `value`: of those that still raise it (MinSkill <= value < TrivialHigh) and that `skip` does not reject, the easiest (lowest
// TrivialLow, then lowest MinSkill, then lowest spell id: one answer for a given table). Returns the index into `recipes`.
std::optional<std::size_t> PlanRecipe(std::vector<Recipe> const& recipes, std::uint32_t skill, std::uint32_t value,
                                      std::function<bool(std::uint32_t spell)> const& skip = {});

// The nodes found empty, shared by all bots: a node marked is skipped until its time is over. Bounded (the oldest go first past kMaxKept).
class EmptyNodes
{
public:
    static constexpr std::size_t kMaxKept = 4096;
    void Mark(std::size_t node, std::uint64_t untilMs);
    bool Empty(std::size_t node, std::uint64_t nowMs) const;
    std::size_t Size() const { return _until.size(); }
private:
    std::unordered_map<std::size_t, std::uint64_t> _until;
};

// The nearest node of `skill` on `map` that the filter allows (gather::NearestNode's rules), within maxYards of (x, y), that `unsafe` does not reject (level,
// danger, empty). `skipped` counts the nodes in range left out by `unsafe`. Returns the index into `nodes`.
std::optional<std::size_t> NearestSafeNode(std::vector<gather::NodePoint> const& nodes, std::uint32_t skill, std::uint32_t map, float x, float y,
                                           gather::NodeFilter const& filter, float maxYards, std::function<bool(std::size_t)> const& unsafe,
                                           std::uint32_t& skipped);

} // namespace professions
