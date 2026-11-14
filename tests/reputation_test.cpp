// SPDX-License-Identifier: GPL-2.0-or-later
// Reputation goals: ranks, the goal table, the goal choice, the visit rules (src/reputation.h) and the policy's visit to a reputation vendor.
#include "bot_policy.h"
#include "reputation.h"

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

using namespace reputation;
using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotSnapshot;
using forever_module::WorldTickSnapshotView;

void RanksFollowTheCoreThresholds()
{
    CHECK(RankOf(-42000) == kHated);
    CHECK(RankOf(-6001) == kHated);
    CHECK(RankOf(-6000) == kHostile);
    CHECK(RankOf(-3000) == kUnfriendly);
    CHECK(RankOf(-1) == kUnfriendly);
    CHECK(RankOf(0) == kNeutral);
    CHECK(RankOf(2999) == kNeutral);
    CHECK(RankOf(3000) == kFriendly);
    CHECK(RankOf(8999) == kFriendly);
    CHECK(RankOf(9000) == kHonored);
    CHECK(RankOf(20999) == kHonored);
    CHECK(RankOf(21000) == kRevered);
    CHECK(RankOf(41999) == kRevered);
    CHECK(RankOf(42000) == kExalted);
    CHECK(RankOf(1000000) == kExalted);
    CHECK(std::string(RankName(kHonored)) == "Honored");
    CHECK(PointsTo(kHonored, 8000) == 1000);
    CHECK(PointsTo(kHonored, 9500) == 0);
    CHECK(PointsTo(kFriendly, -3000) == 6000);
}

char const* const kFile =
    "# a comment\n"
    "\n"
    "910|Brood of Nozdormu|0|60|60|3|35|0|69\n"
    "2758|Nightclaw Druids|0|15|15|7|1|1|12\n"
    "2798|Darkspear Raiders|2|28|60|5|30|2|40\n"
    "2799|Theramore|1|28|60|5|30|2|50\n"
    "bad line\n"
    "5|X|0|10|5|3|1|1|1\n"
    "6|Y|0|10|20|9|1|1|1\n";

void TheGoalFileParses()
{
    std::vector<std::string> errors;
    std::vector<Goal> const goals = ParseGoals(kFile, errors);
    CHECK(goals.size() == 4);
    CHECK(errors.size() == 3);
    CHECK(errors.size() == 3 && errors[0] == "line 7: expected 9 fields");
    CHECK(goals[2].Faction == 2798 && goals[2].Side == 2 && goals[2].MinLevel == 28 && goals[2].MaxLevel == 60 && goals[2].MinRank == kHonored);
    CHECK(goals[2].Vendors == 2 && goals[2].Quests == 40 && goals[2].Name == "Darkspear Raiders");
    std::vector<std::string> none;
    CHECK(ParseGoals("", none).empty() && none.empty());
}

void TheGoalChoice()
{
    std::vector<std::string> errors;
    std::vector<Goal> const goals = ParseGoals(kFile, errors);
    Facts facts;
    facts.Level = 40;
    facts.Side = 2;
    // Horde at 40: Darkspear (vendors, level 28 gear) is a candidate; Theramore is the other side; Brood has no vendor; Nightclaw's gear (level 15) is in reach and sold.
    auto const pick = ChooseGoal(goals, facts, 7, 0);
    CHECK(pick && goals[*pick].Faction == 2798);   // 40 quests beat Nightclaw's 12
    // A bot of the Alliance gets Theramore.
    facts.Side = 1;
    auto const alliance = ChooseGoal(goals, facts, 7, 0);
    CHECK(alliance && goals[*alliance].Faction == 2799);
    // Too low for any gear of its faction beyond Nightclaw (level 15 gear needs level 10): a level 8 bot has no goal; a level 10 bot gets Nightclaw.
    facts.Level = 8;
    CHECK(!ChooseGoal(goals, facts, 7, 0));
    facts.Level = 10;
    auto const low = ChooseGoal(goals, facts, 7, 0);
    CHECK(low && goals[*low].Faction == 2758);
    // The current goal is kept while it is a candidate, even when another one is richer.
    facts.Level = 40;
    facts.Side = 2;
    auto const kept = ChooseGoal(goals, facts, 7, 2758);
    CHECK(kept && goals[*kept].Faction == 2758);
    // An exalted faction is no goal any more.
    facts.Standings = { { 2798, 42000 } };
    auto const maxed = ChooseGoal(goals, facts, 7, 2798);
    CHECK(maxed && goals[*maxed].Faction == 2758);
    // Equal quest counts spread the bots over the factions by guid.
    std::vector<Goal> twin = { goals[2], goals[2] };
    twin[1].Faction = 3000;
    facts.Standings.clear();
    int first = 0, second = 0;
    for (std::uint64_t guid = 1; guid <= 200; ++guid)
    {
        auto const p = ChooseGoal(twin, facts, guid, 0);
        if (p && *p == 0)
            ++first;
        else if (p)
            ++second;
    }
    CHECK(first > 40 && second > 40);
    // The same bot always gets the same goal.
    CHECK(ChooseGoal(twin, facts, 99, 0) == ChooseGoal(twin, facts, 99, 0));
}

void TheVisitRules()
{
    Settings settings;
    Facts facts;
    facts.VendorValid = true;
    facts.Level = 30;
    facts.Copper = 50000;
    facts.FreeSlots = 4;
    Book book;
    CHECK(!TripDue(settings, facts, book, 1000));   // off by default
    settings.Enabled = true;
    CHECK(TripDue(settings, facts, book, 1000));
    Facts poor = facts;
    poor.Copper = 100;
    CHECK(!TripDue(settings, poor, book, 1000));
    Facts full = facts;
    full.FreeSlots = 0;
    CHECK(!TripDue(settings, full, book, 1000));
    Facts low = facts;
    low.Level = 9;
    CHECK(!TripDue(settings, low, book, 1000));
    Facts noVendor = facts;
    noVendor.VendorValid = false;
    CHECK(!TripDue(settings, noVendor, book, 1000));
    NoteTrip(settings, book, 1000);
    CHECK(!TripDue(settings, facts, book, 1000 + settings.TripEveryMs - 1));
    CHECK(TripDue(settings, facts, book, 1000 + settings.TripEveryMs));
}

void AFailedVisitBacksOffAndDoublesUpToTheCap()
{
    Settings settings;
    settings.Enabled = true;
    Book book;
    NoteResult(settings, book, 0, false);
    CHECK(book.Failures == 1 && book.NextTripMs == settings.FailBackoffMs);
    NoteResult(settings, book, 0, false);
    CHECK(book.NextTripMs == settings.FailBackoffMs * 2);
    for (int i = 0; i < 10; ++i)
        NoteResult(settings, book, 0, false);
    CHECK(book.NextTripMs == settings.MaxBackoffMs);
    NoteResult(settings, book, 5, true);
    CHECK(book.Failures == 0);
}

// ---- the policy ----

WorldTickSnapshotView OneBot(float x, std::uint32_t copper, std::uint32_t slots)
{
    WorldTickSnapshotView view;
    view.tick_number = 1;
    view.max_nearby_units = 8;
    BotSnapshot bot;
    bot.guid = 42;
    bot.health = 1000;
    bot.max_health = 1000;
    bot.level = 30;
    bot.pos_x = x;
    bot.copper = copper;
    bot.bag_free_slots = slots;
    view.bots.push_back(bot);
    view.nearby.resize(view.max_nearby_units);
    return view;
}

PolicyOptions RepOptions(std::vector<std::string>& events)
{
    PolicyOptions options;
    options.AggressiveStart = true;
    options.Reputation.Enabled = true;
    std::vector<std::string> errors;
    options.ReputationGoals = ParseGoals(kFile, errors);
    options.ReputationFacts = [](BotSnapshot const&) {
        Facts facts;
        facts.VendorValid = true;
        facts.VendorX = 100.0f;
        facts.VendorGuid = 777;
        facts.Side = 2;
        return facts;
    };
    options.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    return options;
}

bool Has(std::vector<std::string> const& events, char const* prefix)
{
    for (std::string const& e : events)
        if (e.rfind(prefix, 0) == 0)
            return true;
    return false;
}

BotAction const* VendorBuy(std::vector<BotAction> const& actions)
{
    for (BotAction const& a : actions)
        if (a.type == ActionType::kVendor && (a.spell_id & 32768) != 0)
            return &a;
    return nullptr;
}

void ABotWalksToTheVendorAndBuysThere()
{
    std::vector<std::string> events;
    BotPolicy policy(RepOptions(events));
    // Far from the vendor: a walk, and the goal is logged.
    bool walks = false;
    for (std::uint64_t t = 0; t < 12 && !walks; ++t)
    {
        WorldTickSnapshotView view = OneBot(0.0f, 100000, 10);
        for (BotAction const& a : policy.Decide(view, 60000 + t * 2000))
            if (a.type == ActionType::kMoveTo && a.dest_x > 50.0f)
                walks = true;
    }
    CHECK(Has(events, "rep_goal:"));
    CHECK(walks);
    // At the vendor: the buy goes out once, naming the vendor.
    int buys = 0;
    BotAction buy;
    for (std::uint64_t t = 0; t < 12; ++t)
    {
        WorldTickSnapshotView view = OneBot(99.0f, 100000, 10);
        for (BotAction const& a : policy.Decide(view, 100000 + t * 2000))
            if (a.type == ActionType::kVendor && (a.spell_id & 32768) != 0)
            {
                ++buys;
                buy = a;
            }
    }
    CHECK(buys == 1);
    CHECK(buy.target_guid == 777);
    CHECK(Has(events, "rep_buy:"));
    // Nothing was spent: the visit is judged a failure and the next one waits.
    for (std::uint64_t t = 0; t < 6; ++t)
    {
        WorldTickSnapshotView view = OneBot(99.0f, 100000, 10);
        policy.Decide(view, 200000 + t * 2000);
    }
    CHECK(Has(events, "rep_buy_failed:"));
}

void APurchaseIsJudgedASuccess()
{
    std::vector<std::string> events;
    BotPolicy policy(RepOptions(events));
    std::uint64_t now = 100000;
    for (int t = 0; t < 40 && !Has(events, "rep_buy:"); ++t, now += 2000)
    {
        WorldTickSnapshotView view = OneBot(99.0f, 100000, 10);
        policy.Decide(view, now);
    }
    CHECK(Has(events, "rep_buy:"));
    for (int t = 0; t < 6; ++t, now += 2000)
    {
        WorldTickSnapshotView view = OneBot(99.0f, 20000, 9);   // the core spent the money and filled a slot
        policy.Decide(view, now);
    }
    CHECK(Has(events, "rep_buy_done:"));
    CHECK(!Has(events, "rep_buy_failed:"));
}

void NothingHappensWhenOffOrWithoutTheCoreFacts()
{
    std::vector<std::string> events;
    PolicyOptions off = RepOptions(events);
    off.Reputation.Enabled = false;
    BotPolicy offPolicy(off);
    PolicyOptions noFacts = RepOptions(events);
    noFacts.ReputationFacts = nullptr;
    BotPolicy noFactsPolicy(noFacts);
    for (std::uint64_t t = 0; t < 20; ++t)
    {
        WorldTickSnapshotView a = OneBot(99.0f, 100000, 10);
        CHECK(!VendorBuy(offPolicy.Decide(a, 100000 + t * 2000)));
        WorldTickSnapshotView b = OneBot(99.0f, 100000, 10);
        CHECK(!VendorBuy(noFactsPolicy.Decide(b, 100000 + t * 2000)));
    }
    CHECK(events.empty());
}

} // namespace

int main()
{
    RanksFollowTheCoreThresholds();
    TheGoalFileParses();
    TheGoalChoice();
    TheVisitRules();
    AFailedVisitBacksOffAndDoublesUpToTheCap();
    ABotWalksToTheVendorAndBuysThere();
    APurchaseIsJudgedASuccess();
    NothingHappensWhenOffOrWithoutTheCoreFacts();
    if (failures == 0)
        std::printf("reputation_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
