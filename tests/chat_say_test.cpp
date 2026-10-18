// Unit tests for the say-back lines (src/chat_say.h). No core or SDK needed.
#include "chat_say.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

void EveryOrderHasALineAndStatusHasNone()
{
    ChatVerb const verbs[] = { ChatVerb::Follow, ChatVerb::FollowOff, ChatVerb::Stay, ChatVerb::StayOff, ChatVerb::Stop, ChatVerb::GotoHere,
                               ChatVerb::Release, ChatVerb::Pull, ChatVerb::Heal, ChatVerb::Passive, ChatVerb::Aggressive, ChatVerb::Summon,
                               ChatVerb::Revive, ChatVerb::Mount, ChatVerb::Dismount, ChatVerb::Role, ChatVerb::Stance, ChatVerb::Focus,
                               ChatVerb::FocusOff, ChatVerb::Distance };
    for (ChatVerb verb : verbs)
        for (std::uint64_t guid = 1; guid <= 6; ++guid)
        {
            std::string const line = AckLine(verb, guid);
            CHECK(!line.empty() && line.size() <= 60); // a /say line is short
        }
    CHECK(AckLine(ChatVerb::Status, 1).empty());
    CHECK(AckLine(ChatVerb::None, 1).empty());
}

void TheWordingIsStablePerBotAndVariesAcrossBots()
{
    CHECK(AckLine(ChatVerb::Follow, 77) == AckLine(ChatVerb::Follow, 77));
    std::set<std::string> lines;
    for (std::uint64_t guid = 1; guid <= 40; ++guid)
        lines.insert(AckLine(ChatVerb::Follow, guid));
    CHECK(lines.size() == 3); // all three wordings are used by a group of 40
}

void ACapLimitsHowManyBotsSpeak()
{
    std::vector<std::uint64_t> accepted;
    for (std::uint64_t guid = 100; guid < 140; ++guid)
        accepted.push_back(guid);

    CHECK(PickSpeakers(accepted, ChatVerb::Follow, false, 0).empty());
    auto three = PickSpeakers(accepted, ChatVerb::Follow, false, 3);
    CHECK(three.size() == 3);
    CHECK(std::all_of(three.begin(), three.end(), [&](std::uint64_t g) { return std::find(accepted.begin(), accepted.end(), g) != accepted.end(); }));
    CHECK(PickSpeakers(accepted, ChatVerb::Follow, false, 3) == three); // the same on every run
    CHECK(PickSpeakers(accepted, ChatVerb::Follow, false, 100).size() == 40);

    // Not always the same bots: another verb picks differently.
    CHECK(PickSpeakers(accepted, ChatVerb::Stay, false, 3) != three || PickSpeakers(accepted, ChatVerb::Heal, false, 3) != three);
}

void AWhisperedBotAlwaysAnswers()
{
    std::vector<std::uint64_t> one = { 123 };
    CHECK(PickSpeakers(one, ChatVerb::Follow, true, 0) == one);
    CHECK(PickSpeakers(one, ChatVerb::Follow, false, 0).empty());
}

} // namespace

int main()
{
    EveryOrderHasALineAndStatusHasNone();
    TheWordingIsStablePerBotAndVariesAcrossBots();
    ACapLimitsHowManyBotsSpeak();
    AWhisperedBotAlwaysAnswers();
    if (failures == 0)
        std::printf("chat_say_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
