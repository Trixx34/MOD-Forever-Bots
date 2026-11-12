// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the scenario director, the leader's route and auto gear (doc 19). No core needed.
// The tests are in scenario_test_NN.cpp, the shared helpers in scenario_test_support.h.
#include "scenario_test_support.h"

#include <cstdio>

int main()
{
    RunScenarioTestsPart1();
    RunScenarioTestsPart2();
    RunScenarioTestsPart3();
    RunScenarioTestsPart4();
    if (failures == 0)
        std::printf("scenario_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
