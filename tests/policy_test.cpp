// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for BotPolicy, on hand-built snapshots. No core needed.
// The tests are in policy_test_NN.cpp, the shared helpers in policy_test_support.h.
#include "policy_test_support.h"

#include <cstdio>

int main()
{
    RunPolicyTestsPart1();
    RunPolicyTestsPart2();
    RunPolicyTestsPart3();
    RunPolicyTestsPart4();
    RunPolicyTestsPart5();
    RunPolicyTestsPart6();
    RunPolicyTestsPart7();
    RunPolicyTestsPart8();
    RunPolicyTestsPart9();

    if (failures == 0)
        std::printf("policy_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
