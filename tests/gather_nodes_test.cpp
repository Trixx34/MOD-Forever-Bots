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

} // namespace

int main()
{
    ParsesRowsAndSkipsCommentsAndBadLines();
    NearestIsOnTheSkillAndMap();
    if (failures == 0)
        std::printf("gather_nodes_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
