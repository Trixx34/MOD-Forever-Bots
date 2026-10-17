// Unit tests for the bot trade decisions (src/trade_policy.h). No core or SDK needed.
#include "trade_policy.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace trade;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

std::vector<BagItem> Bag()
{
    return {
        { 1001, 2589, 20, "Linen Cloth" },
        { 1002, 6948, 1, "Hearthstone" },
        { 1003, 2592, 5, "Wool Cloth" },
        { 1004, 4306, 8, "Silk Cloth" },
        { 1005, 2589, 7, "Linen Cloth" },
    };
}

constexpr std::uint64_t kBot = 42;
constexpr std::uint64_t kPlayer = 7;

std::size_t Count(std::vector<Step> const& steps, Step::Kind kind)
{
    std::size_t n = 0;
    for (Step const& s : steps)
        n += s.What == kind;
    return n;
}

// ---- the whisper pages ----

void EmptyBagHasOneLine()
{
    auto const lines = ListWhispers({});
    CHECK(lines.size() == 1);
    CHECK(lines[0].find("nothing") != std::string::npos);
}

void SmallBagIsOnePageWithNumbers()
{
    auto const lines = ListWhispers(Bag());
    CHECK(lines.size() == 2); // the instruction and one page
    CHECK(lines[1].rfind("[1/1] ", 0) == 0);
    CHECK(lines[1].find("1) Linen Cloth x20") != std::string::npos);
    CHECK(lines[1].find("2) Hearthstone;") != std::string::npos); // no "x1" for a single item
    CHECK(lines[1].find("5) Linen Cloth x7") != std::string::npos);
}

void BigBagIsPagedUnderTheChatLimitWithRunningNumbers()
{
    std::vector<BagItem> items;
    for (int i = 0; i < 60; ++i)
        items.push_back({ std::uint64_t(2000 + i), std::uint32_t(100 + i), 3, "Some Quite Long Item Name Number " + std::to_string(i) });
    auto const lines = ListWhispers(items);
    CHECK(lines.size() > 3);
    bool allShort = true;
    for (auto const& line : lines)
        allShort = allShort && line.size() <= kWhisperBytes;
    CHECK(allShort);
    CHECK(lines[1].rfind("[1/", 0) == 0);
    CHECK(lines.back().find("60) ") != std::string::npos); // the numbers run on across pages
    std::string all;
    for (auto const& line : lines)
        all += line;
    for (int n : { 1, 17, 33, 60 })
        CHECK(all.find(std::to_string(n) + ") Some") != std::string::npos);
}

// ---- reading a reply ----

void NumbersAndCounts()
{
    auto const r = ParseReply("2, 3 x5, 1x20", Bag());
    CHECK(r.Chosen.size() == 3);
    CHECK(r.Chosen[0].Index == 1 && r.Chosen[0].Count == 0);
    CHECK(r.Chosen[1].Index == 2 && r.Chosen[1].Count == 5);
    CHECK(r.Chosen[2].Index == 0 && r.Chosen[2].Count == 20);
    CHECK(r.NotFound.empty());
}

void NamesExactPrefixAndInside()
{
    auto items = Bag();
    CHECK(ParseReply("hearthstone", items).Chosen.at(0).Index == 1);          // exact, any case
    CHECK(ParseReply("HEARTH", items).Chosen.at(0).Index == 1);               // the start of the name
    CHECK(ParseReply("wool", items).Chosen.at(0).Index == 2);                 // the start of the name
    CHECK(ParseReply("silk", items).Chosen.at(0).Index == 3);
    CHECK(ParseReply("stone", items).Chosen.at(0).Index == 1);                // inside the name
}

void CountBeforeOrAfterAName()
{
    auto items = Bag();
    auto a = ParseReply("3 wool cloth", items);
    CHECK(a.Chosen.size() == 1 && a.Chosen[0].Index == 2 && a.Chosen[0].Count == 3);
    auto b = ParseReply("wool cloth x3", items);
    CHECK(b.Chosen.size() == 1 && b.Chosen[0].Index == 2 && b.Chosen[0].Count == 3);
    auto c = ParseReply("3x wool cloth", items);
    CHECK(c.Chosen.size() == 1 && c.Chosen[0].Index == 2 && c.Chosen[0].Count == 3);
    // a name that starts with x is a name, not a count
    std::vector<BagItem> odd = { { 1, 9, 2, "Xylophone" } };
    auto d = ParseReply("3 xylophone", odd);
    CHECK(d.Chosen.size() == 1 && d.Chosen[0].Count == 3);
}

void AndCommasAndSemicolonsSeparate()
{
    auto const r = ParseReply("hearthstone and silk cloth; 3", Bag());
    CHECK(r.Chosen.size() == 3);
}

void AllAndCancel()
{
    CHECK(ParseReply("all", Bag()).All);
    CHECK(ParseReply("  Cancel ", Bag()).Cancel);
    CHECK(!ParseReply("cancel the order please", Bag()).Cancel); // only the bare word
}

void UnknownAndAmbiguousAndOutOfRange()
{
    auto items = Bag();
    auto r = ParseReply("dragon sword, 9, 0", items);
    CHECK(r.Chosen.empty());
    CHECK(r.NotFound.size() == 3);
    auto amb = ParseReply("cloth", items); // Linen, Wool and Silk Cloth
    CHECK(amb.Chosen.empty() && amb.Ambiguous.size() == 1);
    CHECK(amb.Ambiguous[0].find("Wool Cloth") != std::string::npos);
}

void SeveralStacksOfOneItemTakeTurns()
{
    auto items = Bag(); // two stacks of Linen Cloth: index 0 and 4
    auto r = ParseReply("linen cloth, linen cloth", items);
    CHECK(r.Chosen.size() == 2 && r.Chosen[0].Index == 0 && r.Chosen[1].Index == 4);
    auto once = ParseReply("1, 1, 1", items);
    CHECK(once.Chosen.size() == 1); // the same stack is not chosen twice
}

// ---- the trade itself ----

TradeBook Opened()
{
    TradeBook book;
    auto steps = book.OnEvent(kBot, kPlayer, Status::Requested, 1000);
    CHECK(steps.size() == 2 && steps[0].What == Step::Kind::Begin && steps[1].What == Step::Kind::ListBags);
    auto whispers = book.OnBagList(kBot, Bag(), 1100);
    CHECK(Count(whispers, Step::Kind::Whisper) == 2);
    CHECK(whispers[0].Partner == kPlayer && whispers[0].Bot == kBot);
    return book;
}

void RequestOpensAndListsThenPlacesChosenItems()
{
    TradeBook book = Opened();
    auto steps = book.OnWhisper(kPlayer, kBot, "2 and 3", 1200);
    CHECK(steps.size() == 2);
    CHECK(steps[0].What == Step::Kind::SetItem && steps[0].Slot == 0 && steps[0].ItemGuid == 1002 && steps[0].Text == "Hearthstone");
    CHECK(steps[1].What == Step::Kind::SetItem && steps[1].Slot == 1 && steps[1].ItemGuid == 1003);
    // a later ask continues in the next free slot, and an item already in the window is not placed twice
    auto more = book.OnWhisper(kPlayer, kBot, "3, silk", 1300);
    CHECK(more.size() == 1 && more[0].Slot == 2 && more[0].ItemGuid == 1004);
}

void WhispersFromOthersAndBeforeTheListAreOrdinaryChat()
{
    TradeBook book;
    CHECK(book.OnWhisper(kPlayer, kBot, "2", 100).empty()); // no trade
    book.OnEvent(kBot, kPlayer, Status::Requested, 200);
    CHECK(book.OnWhisper(kPlayer, kBot, "2", 300).empty()); // not listed yet
    book.OnBagList(kBot, Bag(), 400);
    CHECK(book.OnWhisper(99, kBot, "2", 500).empty());      // someone else
    CHECK(!book.OnWhisper(kPlayer, kBot, "2", 600).empty());
}

void SevenWantedFillsSixSlotsAndSaysSo()
{
    TradeBook book;
    book.OnEvent(kBot, kPlayer, Status::Requested, 0);
    std::vector<BagItem> many;
    for (int i = 0; i < 8; ++i)
        many.push_back({ std::uint64_t(500 + i), std::uint32_t(10 + i), 1, "Thing" + std::to_string(i) });
    book.OnBagList(kBot, many, 1);
    auto steps = book.OnWhisper(kPlayer, kBot, "all", 2);
    CHECK(Count(steps, Step::Kind::SetItem) == 6);
    CHECK(steps[5].Slot == 5);
    CHECK(Count(steps, Step::Kind::Whisper) == 1);
    CHECK(steps.back().Text.find("holds 6") != std::string::npos);
}

void AWholeStackIsNeededAndTheBotSaysSo()
{
    TradeBook book = Opened();
    auto steps = book.OnWhisper(kPlayer, kBot, "linen cloth x5", 1200); // the first stack has 20
    CHECK(Count(steps, Step::Kind::SetItem) == 0);
    CHECK(steps.size() == 1 && steps[0].Text.find("whole stacks") != std::string::npos);
    auto fine = book.OnWhisper(kPlayer, kBot, "linen cloth x20", 1300);
    CHECK(Count(fine, Step::Kind::SetItem) == 1);
}

void UnknownWordsGetAnAnswer()
{
    TradeBook book = Opened();
    auto steps = book.OnWhisper(kPlayer, kBot, "dragon sword", 1200);
    CHECK(steps.size() == 1 && steps[0].What == Step::Kind::Whisper);
    CHECK(steps[0].Text.find("dragon sword") != std::string::npos);
}

void BotAcceptsWhenThePartnerDoesAndOnlyThen()
{
    TradeBook book = Opened();
    CHECK(book.OnEvent(kBot, kPlayer, Status::Changed, 1500).empty());
    auto steps = book.OnEvent(kBot, kPlayer, Status::PartnerAccepted, 1600);
    CHECK(steps.size() == 1 && steps[0].What == Step::Kind::Accept && steps[0].Bot == kBot);
    // an accept for a bot that is not in a trade does nothing
    CHECK(book.OnEvent(555, kPlayer, Status::PartnerAccepted, 1700).empty());
}

void CompleteThanksAndEnds()
{
    TradeBook book = Opened();
    auto steps = book.OnEvent(kBot, kPlayer, Status::Completed, 2000);
    CHECK(steps.size() == 1 && steps[0].What == Step::Kind::Whisper && steps[0].Partner == kPlayer);
    CHECK(!book.Active(kBot));
    CHECK(book.OnWhisper(kPlayer, kBot, "2", 2100).empty());
}

void CancelEnds()
{
    TradeBook book = Opened();
    CHECK(book.OnEvent(kBot, kPlayer, Status::Cancelled, 2000).empty());
    CHECK(!book.Active(kBot));
}

void CancelByWhisperCancelsTheTrade()
{
    TradeBook book = Opened();
    auto steps = book.OnWhisper(kPlayer, kBot, "cancel", 1200);
    CHECK(steps.size() == 1 && steps[0].What == Step::Kind::Cancel);
    CHECK(!book.Active(kBot));
}

void IdleTradesAreCancelled()
{
    TradeBook book(1000);
    book.OnEvent(kBot, kPlayer, Status::Requested, 0);
    CHECK(book.OnIdle(900).empty());
    auto steps = book.OnIdle(1500);
    CHECK(Count(steps, Step::Kind::Cancel) == 1 && Count(steps, Step::Kind::Whisper) == 1);
    CHECK(!book.Active(kBot));
    // activity keeps a trade alive
    book.OnEvent(kBot, kPlayer, Status::Requested, 5000);
    book.OnEvent(kBot, kPlayer, Status::Changed, 5800);
    CHECK(book.OnIdle(6500).empty());
}

void ARefusedItemIsExplained()
{
    TradeBook book = Opened();
    auto steps = book.OnItemRefused(kBot, "Hearthstone");
    CHECK(steps.size() == 1 && steps[0].Text.find("Hearthstone") != std::string::npos && steps[0].Partner == kPlayer);
    CHECK(book.OnItemRefused(555, "x").empty());
}

void ANewRequestReplacesAStaleTrade()
{
    TradeBook book = Opened();
    book.OnWhisper(kPlayer, kBot, "2", 1200);
    book.OnEvent(kBot, 8, Status::Requested, 3000); // another player asks
    CHECK(book.PartnerOf(kBot) == 8);
    CHECK(book.OnWhisper(kPlayer, kBot, "3", 3100).empty());
}

} // namespace

int main()
{
    EmptyBagHasOneLine();
    SmallBagIsOnePageWithNumbers();
    BigBagIsPagedUnderTheChatLimitWithRunningNumbers();
    NumbersAndCounts();
    NamesExactPrefixAndInside();
    CountBeforeOrAfterAName();
    AndCommasAndSemicolonsSeparate();
    AllAndCancel();
    UnknownAndAmbiguousAndOutOfRange();
    SeveralStacksOfOneItemTakeTurns();
    RequestOpensAndListsThenPlacesChosenItems();
    WhispersFromOthersAndBeforeTheListAreOrdinaryChat();
    SevenWantedFillsSixSlotsAndSaysSo();
    AWholeStackIsNeededAndTheBotSaysSo();
    UnknownWordsGetAnAnswer();
    BotAcceptsWhenThePartnerDoesAndOnlyThen();
    CompleteThanksAndEnds();
    CancelEnds();
    CancelByWhisperCancelsTheTrade();
    IdleTradesAreCancelled();
    ARefusedItemIsExplained();
    ANewRequestReplacesAStaleTrade();

    if (failures == 0)
        std::printf("trade_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
