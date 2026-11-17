// SPDX-License-Identifier: GPL-2.0-or-later
// The gathering node table: parsing and the nearest node of a skill (src/gather_nodes.h).
#include "gather_nodes.h"

#include <cstdio>
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

void ParsesRowsAndSkipsCommentsAndBadLines()
{
    std::vector<std::string> errors;
    auto const nodes = gather::ParseNodeTable(
        "# skill map x y z entry\n"
        "182 0 -8900.5 -131.0 83.0 1617\n"
        "\n"
        "186 1 -1200 3300 40 1731\r\n"
        "186 1 oops 3300 40 1731\n"
        "182 0 1 2 3\n",
        errors);
    CHECK(nodes.size() == 2);
    CHECK(errors.size() == 2);
    if (nodes.size() == 2)
    {
        CHECK(nodes[0].Skill == 182 && nodes[0].Map == 0 && nodes[0].Entry == 1617);
        CHECK(nodes[0].X == -8900.5f && nodes[0].Y == -131.0f);
        CHECK(nodes[1].Skill == 186 && nodes[1].Entry == 1731);
    }
}

void NearestIsOnTheSkillAndMap()
{
    std::vector<std::string> errors;
    auto const nodes = gather::ParseNodeTable(
        "182 0 0 0 0 1\n"
        "182 0 10 0 0 2\n"
        "182 1 1 0 0 3\n"
        "186 0 2 0 0 4\n",
        errors);
    auto const herb = gather::NearestNode(nodes, 182, 0, 9.0f, 0.0f);
    CHECK(herb.has_value() && nodes[*herb].Entry == 2);
    auto const mining = gather::NearestNode(nodes, 186, 0, 9.0f, 0.0f);
    CHECK(mining.has_value() && nodes[*mining].Entry == 4);
    CHECK(!gather::NearestNode(nodes, 182, 530, 0.0f, 0.0f).has_value());
    CHECK(!gather::NearestNode(nodes, gather::kSkillSkinning, 0, 0.0f, 0.0f).has_value());
}

void OptionalColumnsAndTheFilter()
{
    std::vector<std::string> errors;
    auto const nodes = gather::ParseNodeTable(
        "186 0 0 0 0 1 125 20 0\n"      // iron, needs 125, level 20 area
        "186 0 50 0 0 2 1 5 1\n"        // copper, level 5, Alliance
        "186 0 80 0 0 3 1 5 2\n"        // copper, level 5, Horde
        "186 0 200 0 0 4 1\n"           // only the required skill
        "186 0 1 2 3 5 1 2 3\n"         // side 3 is bad
        "186 0 1 2 3 5 x\n",            // bad required skill
        errors);
    CHECK(errors.size() == 2);
    CHECK(nodes.size() == 4);
    CHECK(nodes[0].RequiredSkill == 125 && nodes[0].AreaLevel == 20 && nodes[0].Side == 0);
    CHECK(nodes[3].RequiredSkill == 1 && nodes[3].AreaLevel == 0);
    // a bot with Mining 1, level 5, Alliance: not the iron, not the Horde copper
    gather::NodeFilter filter;
    filter.SkillValue = 1;
    filter.Level = 5;
    filter.Side = 1;
    auto const pick = gather::NearestNode(nodes, 186, 0, 0.0f, 0.0f, filter);
    CHECK(pick.has_value() && nodes[*pick].Entry == 2);
    // the same bot at level 1: the level 5 area is within the slack of 3? no (5 > 1 + 3), so it falls to the node with no area level
    filter.Level = 1;
    auto const low = gather::NearestNode(nodes, 186, 0, 0.0f, 0.0f, filter);
    CHECK(low.has_value() && nodes[*low].Entry == 4);
    // skill 125 and level 20: the iron is allowed and nearest
    filter.SkillValue = 125;
    filter.Level = 20;
    auto const iron = gather::NearestNode(nodes, 186, 0, 0.0f, 0.0f, filter);
    CHECK(iron.has_value() && nodes[*iron].Entry == 1);
    // nothing allowed: skill 0 cannot take any node that needs 1
    filter.SkillValue = 0;
    CHECK(!gather::NearestNode(nodes, 186, 0, 0.0f, 0.0f, filter).has_value());
    // the default filter allows everything, as before the columns existed
    auto const any = gather::NearestNode(nodes, 186, 0, 0.0f, 0.0f);
    CHECK(any.has_value() && nodes[*any].Entry == 1);
}

} // namespace

int main()
{
    ParsesRowsAndSkipsCommentsAndBadLines();
    NearestIsOnTheSkillAndMap();
    OptionalColumnsAndTheFilter();
    if (failures == 0)
        std::printf("gather_nodes_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
