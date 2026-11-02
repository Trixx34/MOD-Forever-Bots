// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the whisper replies and busy lines (src/chat_reply.h). No core or SDK needed.
#include "chat_reply.h"

#include <cstdio>
#include <set>
#include <string>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

void PlainQuestionsClassify()
{
    CHECK(ClassifyWhisper("hello") == WhisperIntent::Hello);
    CHECK(ClassifyWhisper("Hi there!") == WhisperIntent::Hello);
    CHECK(ClassifyWhisper("what are you doing?") == WhisperIntent::Status);
    CHECK(ClassifyWhisper("status") == WhisperIntent::Status);
    CHECK(ClassifyWhisper("where are you") == WhisperIntent::Where);
    CHECK(ClassifyWhisper("can I join you") == WhisperIntent::NeedGroup);
    CHECK(ClassifyWhisper("thanks") == WhisperIntent::Thanks);
    CHECK(ClassifyWhisper("") == WhisperIntent::Other);
    CHECK(ClassifyWhisper("buy 5 linen bandages") == WhisperIntent::Other);
}

void WordsMustMatchWholeNotInsideOthers()
{
    // "hi" inside "this" or "high" is not a greeting
    CHECK(ClassifyWhisper("this is high level stuff") == WhisperIntent::Other);
}

void BusyReasonPrefersDeadThenCombatThenDungeonThenTravelling()
{
    CHECK(BusyReason(true, true, true, true) == BusyKind::Dead);
    CHECK(BusyReason(true, false, true, true) == BusyKind::Combat);
    CHECK(BusyReason(false, false, true, true) == BusyKind::Dungeon);
    CHECK(BusyReason(false, false, false, true) == BusyKind::Travelling);
    CHECK(BusyReason(false, false, false, false) == BusyKind::None);
}

void ReplyLinesStayPerBotAndVaryAcrossBots()
{
    for (std::uint64_t guid = 1; guid <= 6; ++guid)
    {
        std::string const first = ReplyLine(WhisperIntent::Status, guid, BotActivity::Grinding);
        CHECK(!first.empty());
        CHECK(first == ReplyLine(WhisperIntent::Status, guid, BotActivity::Grinding));
        CHECK(first.size() <= kMaxReplyBytes);
    }
    std::set<std::string> variants;
    for (std::uint64_t guid = 1; guid <= 3; ++guid)
        variants.insert(ReplyLine(WhisperIntent::Hello, guid, BotActivity::Grinding));
    CHECK(variants.size() == 3);
    CHECK(ReplyLine(WhisperIntent::Other, 1, BotActivity::Grinding).empty());
}

void EveryIntentAndActivityFitTheLimit()
{
    WhisperIntent const intents[] = { WhisperIntent::Hello, WhisperIntent::Status, WhisperIntent::Where, WhisperIntent::NeedGroup, WhisperIntent::Thanks };
    BotActivity const activities[] = { BotActivity::Grinding, BotActivity::Following, BotActivity::Holding, BotActivity::Travelling, BotActivity::Dungeon, BotActivity::Fighting };
    for (WhisperIntent intent : intents)
        for (BotActivity activity : activities)
            for (std::uint64_t guid = 1; guid <= 3; ++guid)
                CHECK(ReplyLine(intent, guid, activity).size() <= kMaxReplyBytes);
}

void BusyLinesExistForEveryReasonAndFitTheLimit()
{
    BusyKind const reasons[] = { BusyKind::None, BusyKind::Dead, BusyKind::Combat, BusyKind::Dungeon, BusyKind::Travelling };
    for (BusyKind reason : reasons)
        for (std::uint64_t guid = 1; guid <= 3; ++guid)
        {
            std::string const line = BusyLine(guid, reason);
            CHECK(!line.empty());
            CHECK(line.size() <= kMaxReplyBytes);
        }
    std::string const long_text(400, 'x');
    CHECK(ClampReply(long_text).size() == kMaxReplyBytes);
    CHECK(ClampReply("short") == "short");
}

void CooldownHoldsPerPlayerAndBot()
{
    WhisperLimiter limiter(60000, 6);
    CHECK(limiter.Allow(10, 100, 1000));
    CHECK(!limiter.Allow(10, 100, 30000));          // same pair inside the cooldown
    CHECK(limiter.Allow(10, 100, 61000));           // cooldown over
    CHECK(limiter.Allow(10, 101, 30000));           // another bot of the same player is not held back
    CHECK(limiter.Allow(11, 100, 30000));           // another player is not blocked either
}

void PerMinuteCapLimitsAPlayerAcrossBots()
{
    WhisperLimiter limiter(0, 3);
    CHECK(limiter.Allow(20, 200, 0));
    CHECK(limiter.Allow(20, 201, 1000));
    CHECK(limiter.Allow(20, 202, 2000));
    CHECK(!limiter.Allow(20, 203, 3000));           // fourth in the minute
    CHECK(limiter.Allow(21, 203, 3000));            // another player still gets an answer
    CHECK(limiter.Allow(20, 203, 60000));           // the window has moved on (oldest is a minute old)
}

void TrackedPairsStayBounded()
{
    WhisperLimiter limiter(60000, 100000);
    for (std::uint64_t bot = 1; bot <= WhisperLimiter::kMaxTracked + 500; ++bot)
        limiter.Allow(7, bot, bot);
    CHECK(limiter.Tracked() <= WhisperLimiter::kMaxTracked + 1);
}

} // namespace

int main()
{
    PlainQuestionsClassify();
    WordsMustMatchWholeNotInsideOthers();
    BusyReasonPrefersDeadThenCombatThenDungeonThenTravelling();
    ReplyLinesStayPerBotAndVaryAcrossBots();
    EveryIntentAndActivityFitTheLimit();
    BusyLinesExistForEveryReasonAndFitTheLimit();
    CooldownHoldsPerPlayerAndBot();
    PerMinuteCapLimitsAPlayerAcrossBots();
    TrackedPairsStayBounded();
    if (failures == 0)
        std::printf("chat_reply_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
