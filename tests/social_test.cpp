// SPDX-License-Identifier: GPL-2.0-or-later
// Duels, battlegrounds, guild join and mail gifts, end to end on the module side: the policy turn (TurnSocial) queues an order when the setting is on and
// none when it is off, social::Send hands the order to a real ModuleClient, and a fake core on a Unix socket reads the ActionRequest that arrives.
// No core, no SDK server needed.
#include "bot_policy.h"
#include "mail_gifts.h"
#include "social_actions.h"

#include "forever_module/module_client.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using forever_module::ActionRequest;
using forever_module::BotSnapshot;
using forever_module::Envelope;
using forever_module::MakeUnitRef;
using forever_module::ModuleClient;
using forever_module::ModuleManifest;
using forever_module::NearbyUnit;
using forever_module::WorldTickSnapshotView;

namespace {

int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            ++failures;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

constexpr std::uint64_t kBot = 42;
constexpr std::uint64_t kNow = 10 * 60 * 1000;

NearbyUnit Player(std::uint64_t counter, float x)
{
    NearbyUnit unit;
    unit.guid = MakeUnitRef(2, 0, counter);
    unit.pos_x = x;
    unit.health = 100;
    unit.max_health = 100;
    return unit;
}

// One idle bot at the origin, level `level`, on `map`, with the given nearby units.
WorldTickSnapshotView View(std::uint32_t level, std::uint32_t map, std::vector<NearbyUnit> nearby = {}, std::uint64_t guid = kBot)
{
    WorldTickSnapshotView view;
    view.tick_number = 1;
    view.max_nearby_units = 8;
    BotSnapshot bot;
    bot.guid = guid;
    bot.health = 1000;
    bot.max_health = 1000;
    bot.level = level;
    bot.map_id = map;
    bot.nearby_count = static_cast<std::uint32_t>(nearby.size());
    view.bots.push_back(bot);
    view.nearby.resize(view.max_nearby_units);
    for (std::size_t i = 0; i < nearby.size(); ++i)
        view.nearby[i] = nearby[i];
    return view;
}

PolicyOptions Quiet()
{
    PolicyOptions options;
    options.WanderWhenIdle = false;   // the bot stands where it is: idle
    return options;
}

// A core that only listens: reads the frames a client writes (4-byte little-endian length, then an Envelope).
struct FakeCore
{
    std::string Path;
    int Listen = -1;
    int Conn = -1;

    bool Start()
    {
        Path = "/tmp/forever-social-test-" + std::to_string(::getpid()) + ".sock";
        ::unlink(Path.c_str());
        Listen = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, Path.c_str(), sizeof(address.sun_path) - 1);
        return Listen >= 0 && ::bind(Listen, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && ::listen(Listen, 1) == 0;
    }
    bool Accept()
    {
        Conn = ::accept(Listen, nullptr, nullptr);
        return Conn >= 0;
    }
    bool ReadExact(void* out, std::size_t size, int timeoutMs)
    {
        std::size_t done = 0;
        while (done < size)
        {
            pollfd p{ Conn, POLLIN, 0 };
            if (::poll(&p, 1, timeoutMs) <= 0)
                return false;
            ssize_t const got = ::read(Conn, static_cast<char*>(out) + done, size - done);
            if (got <= 0)
                return false;
            done += std::size_t(got);
        }
        return true;
    }
    bool Read(Envelope& envelope, int timeoutMs = 1000)
    {
        std::array<std::uint8_t, 4> header{};
        if (!ReadExact(header.data(), header.size(), timeoutMs))
            return false;
        std::uint32_t const length = header[0] | (header[1] << 8) | (header[2] << 16) | (std::uint32_t(header[3]) << 24);
        std::string body(length, '\0');
        return ReadExact(body.data(), length, timeoutMs) && envelope.ParseFromString(body);
    }
    ~FakeCore()
    {
        if (Conn >= 0)
            ::close(Conn);
        if (Listen >= 0)
            ::close(Listen);
        ::unlink(Path.c_str());
    }
};

// Sends the orders through a real client and returns the ActionRequests the fake core read, ending with a Log marker so "nothing sent" is a fact.
std::vector<ActionRequest> Deliver(std::vector<social::Order> const& orders)
{
    std::vector<ActionRequest> got;
    FakeCore core;
    ModuleClient client{ ModuleManifest{} };
    if (!core.Start() || !client.Connect(core.Path) || !core.Accept())
    {
        ++failures;
        std::printf("FAIL fake core did not start\n");
        return got;
    }
    for (social::Order const& order : orders)
        CHECK(social::Send(client, order, nullptr));
    client.Log(0, "end-marker");
    Envelope envelope;
    while (core.Read(envelope))
    {
        if (envelope.payload_case() != Envelope::kActionRequest)
            continue;
        if (envelope.action_request().has_log())
            break;
        got.push_back(envelope.action_request());
    }
    return got;
}

std::vector<social::Order> OrdersOf(PolicyOptions const& options, WorldTickSnapshotView const& view, BotPolicy& policy, std::uint64_t now = kNow)
{
    (void)options;
    policy.Decide(view, now);
    return policy.TakeSocialOrders();
}

void DuelsAreSentWhenOnAndNotWhenOff()
{
    NearbyUnit const rival = Player(77, 10.0f);
    // On: the nearest player within 25 yards is challenged.
    PolicyOptions on = Quiet();
    on.Duels.Enabled = true;
    BotPolicy policy(on);
    std::vector<social::Order> orders = OrdersOf(on, View(20, 0, { rival }), policy);
    CHECK(orders.size() == 1);
    if (orders.size() == 1)
    {
        CHECK(orders[0].What == social::Kind::Duel && orders[0].Op == 0 && orders[0].Player == 77 && orders[0].Bot == kBot);
        std::vector<ActionRequest> const sent = Deliver(orders);
        CHECK(sent.size() == 1 && sent[0].has_duel());
        if (sent.size() == 1 && sent[0].has_duel())
            CHECK(sent[0].duel().op() == forever_module::DuelAction::kChallenge && sent[0].duel().bot_guid() == kBot && sent[0].duel().player_guid() == 77);
    }
    // Off (the default): nothing is queued, nothing reaches the core.
    BotPolicy off{ Quiet() };
    std::vector<social::Order> none = OrdersOf(Quiet(), View(20, 0, { rival }), off);
    CHECK(none.empty());
    CHECK(Deliver(none).empty());
    // One of our own bots next to it is not a rival; a player below DuelMinLevel is not asked.
    BotPolicy ours(on);
    WorldTickSnapshotView both = View(20, 0, { Player(kBot + 1, 5.0f) });
    BotSnapshot other = both.bots[0];
    other.guid = kBot + 1;
    both.bots.push_back(other);
    both.bots[1].pos_x = 5.0f;
    CHECK(OrdersOf(on, both, ours).empty() || OrdersOf(on, both, ours)[0].Player != kBot + 1);
    BotPolicy low(on);
    CHECK(OrdersOf(on, View(5, 0, { rival }), low).empty());
    // A challenge to the bot is accepted.
    BotPolicy accept(on);
    accept.Decide(View(20, 0), kNow);
    accept.NoteDuelChallenge(kBot, 88);
    std::vector<social::Order> accepted = OrdersOf(on, View(20, 0), accept, kNow + 1000);
    CHECK(accepted.size() == 1 && accepted[0].Op == 1 && accepted[0].Player == 88);
    std::vector<ActionRequest> const sentAccept = Deliver(accepted);
    CHECK(sentAccept.size() == 1 && sentAccept[0].has_duel() && sentAccept[0].duel().op() == forever_module::DuelAction::kAccept &&
          sentAccept[0].duel().player_guid() == 88);
}

void BattlegroundsAreSentWhenOnAndNotWhenOff()
{
    PolicyOptions on = Quiet();
    on.Battlegrounds.Enabled = true;
    BotPolicy policy(on);
    std::vector<social::Order> orders = OrdersOf(on, View(20, 0), policy);
    CHECK(orders.size() == 1);
    if (orders.size() == 1)
    {
        CHECK(orders[0].What == social::Kind::Battleground && orders[0].Op == 0 && (orders[0].Id == 2 || orders[0].Id == 3));
        std::vector<ActionRequest> const sent = Deliver(orders);
        CHECK(sent.size() == 1 && sent[0].has_battleground());
        if (sent.size() == 1 && sent[0].has_battleground())
            CHECK(sent[0].battleground().op() == forever_module::BattlegroundAction::kQueue && sent[0].battleground().bot_guid() == kBot &&
                  sent[0].battleground().battleground_id() == orders[0].Id);
    }
    // One order at a time: nothing more while the first is in flight.
    CHECK(OrdersOf(on, View(20, 0), policy, kNow + 1000).empty());
    // The core queued the bot; it waits, and after BattlegroundMaxWaitMin the queue is left.
    social::Outcome queued;
    queued.What = social::Kind::Battleground;
    queued.Bot = kBot;
    queued.Op = 0;
    queued.Success = true;
    policy.NoteSocialOutcome(queued, kNow + 2000);
    CHECK(OrdersOf(on, View(20, 0), policy, kNow + 60000).empty());
    std::vector<social::Order> leave = OrdersOf(on, View(20, 0), policy, kNow + 2000 + 11ull * 60000ull);
    CHECK(leave.size() == 1 && leave[0].Op == 1 && leave[0].Id == orders[0].Id);
    std::vector<ActionRequest> const sentLeave = Deliver(leave);
    CHECK(sentLeave.size() == 1 && sentLeave[0].has_battleground() && sentLeave[0].battleground().op() == forever_module::BattlegroundAction::kLeaveQueue);
    // A level too low for any battleground queues nothing; off sends nothing.
    BotPolicy young(on);
    CHECK(OrdersOf(on, View(5, 0), young).empty());
    BotPolicy off{ Quiet() };
    std::vector<social::Order> none = OrdersOf(Quiet(), View(20, 0), off);
    CHECK(none.empty() && Deliver(none).empty());
    // A bot already in a battleground map does not queue.
    BotPolicy inside(on);
    CHECK(OrdersOf(on, View(20, 489), inside).empty());
    CHECK(social::PickBattleground(9, 0) == 0 && social::PickBattleground(15, 3) == 2 && social::IsBattlegroundMap(529) && !social::IsBattlegroundMap(0));
}

void GuildAnswersAreSentWhenOnAndNotWhenOff()
{
    PolicyOptions on = Quiet();
    on.GuildJoin.Enabled = true;
    BotPolicy policy(on);
    std::vector<social::Order> orders = OrdersOf(on, View(20, 0), policy);
    CHECK(orders.size() == 1);
    if (orders.size() == 1)
    {
        CHECK(orders[0].What == social::Kind::Guild && orders[0].Op == 0);
        std::vector<ActionRequest> const sent = Deliver(orders);
        CHECK(sent.size() == 1 && sent[0].has_guild());
        if (sent.size() == 1 && sent[0].has_guild())
            CHECK(sent[0].guild().op() == forever_module::GuildAction::kAccept && sent[0].guild().bot_guid() == kBot);
    }
    // The core had no invitation: not an error, and the next try waits a minute.
    social::Outcome refused;
    refused.What = social::Kind::Guild;
    refused.Bot = kBot;
    refused.Reason = "no pending guild invitation";
    policy.NoteSocialOutcome(refused, kNow + 1000);
    CHECK(OrdersOf(on, View(20, 0), policy, kNow + 5000).empty());
    CHECK(OrdersOf(on, View(20, 0), policy, kNow + 70000).size() == 1);
    // Joined: no more polling; a leave request is sent as kLeave.
    social::Outcome joined = refused;
    joined.Success = true;
    joined.Reason.clear();
    policy.NoteSocialOutcome(joined, kNow + 71000);
    CHECK(OrdersOf(on, View(20, 0), policy, kNow + 200000).empty());
    policy.NoteGuildLeaveRequest(kBot);
    std::vector<social::Order> leave = OrdersOf(on, View(20, 0), policy, kNow + 201000);
    CHECK(leave.size() == 1 && leave[0].Op == 2);
    std::vector<ActionRequest> const sentLeave = Deliver(leave);
    CHECK(sentLeave.size() == 1 && sentLeave[0].has_guild() && sentLeave[0].guild().op() == forever_module::GuildAction::kLeave);
    BotPolicy off{ Quiet() };
    std::vector<social::Order> none = OrdersOf(Quiet(), View(20, 0), off);
    CHECK(none.empty() && Deliver(none).empty());
}

void GiftsAreSentWhenOnAndNotWhenOff()
{
    PolicyOptions on = Quiet();
    on.MailGifts.Enabled = true;
    on.MailGifts.DailyCap = 1;
    BotPolicy policy(on);
    policy.Decide(View(20, 0), kNow);
    CHECK(policy.TakeSocialOrders().empty());   // nobody to thank
    policy.NoteGiftRecipient(kBot, 99);
    std::vector<social::Order> orders = OrdersOf(on, View(20, 0), policy, kNow + 1000);
    CHECK(orders.size() == 1);
    if (orders.size() == 1)
    {
        CHECK(orders[0].What == social::Kind::Gift && orders[0].Player == 99 && orders[0].Bot == kBot);
        // The first thing the core sees is the bag list; the gift itself follows its reply (PickGift below).
        std::vector<ActionRequest> const sent = Deliver(orders);
        CHECK(sent.size() == 1 && sent[0].has_bot_trade() && sent[0].bot_trade().op() == forever_module::BotTradeAction::kListBags &&
              sent[0].bot_trade().bot_guid() == kBot);
    }
    // The gift did not go out (no fit stack): the daily slot comes back and a new thank-you can be sent.
    social::Outcome nothing;
    nothing.What = social::Kind::Gift;
    nothing.Bot = kBot;
    nothing.Reason = "no item fit to give";
    policy.NoteSocialOutcome(nothing, kNow + 2000);
    policy.NoteGiftRecipient(kBot, 100);
    std::vector<social::Order> again = OrdersOf(on, View(20, 0), policy, kNow + 3000);
    CHECK(again.size() == 1 && again[0].Player == 100);
    // Sent: the cap of one a day holds.
    social::Outcome done = nothing;
    done.Success = true;
    done.Reason.clear();
    policy.NoteSocialOutcome(done, kNow + 4000);
    policy.NoteGiftRecipient(kBot, 101);
    CHECK(OrdersOf(on, View(20, 0), policy, kNow + 5000).empty());
    BotPolicy off{ Quiet() };
    off.Decide(View(20, 0), kNow);
    off.NoteGiftRecipient(kBot, 99);
    std::vector<social::Order> none = OrdersOf(Quiet(), View(20, 0), off, kNow + 1000);
    CHECK(none.empty() && Deliver(none).empty());
}

void PickGiftChoosesSmallStacksOnly()
{
    using gifts::BagStack;
    CHECK(gifts::PickGift({}, 0) == -1);
    std::vector<BagStack> stacks{ { 1, 100, 1, "Sword" }, { 2, 200, 40, "Linen Cloth" }, { 3, 300, 5, "Peacebloom" }, { 0, 400, 5, "Ghost" } };
    CHECK(gifts::PickGift(stacks, 0) == 2 && gifts::PickGift(stacks, 7) == 2);   // a single piece, a huge stack and an empty guid are never given
    stacks.push_back({ 5, 500, 20, "Silverleaf" });
    int const a = gifts::PickGift(stacks, 0);
    int const b = gifts::PickGift(stacks, 1);
    CHECK((a == 2 && b == 4) || (a == 4 && b == 2));
    CHECK(gifts::GiftSubject("Peacebloom") == "A thank-you gift: Peacebloom");
    CHECK(gifts::GiftSubject(std::string(100, 'x')).size() == 64);
}

void OrdersWithoutABotOrAnUnknownCoreAreHandled()
{
    ModuleClient client{ ModuleManifest{} };
    social::Order none;
    CHECK(!social::Send(client, none, nullptr));
    CHECK(social::IsUnknownActionReason("unknown action") && !social::IsUnknownActionReason("level too low"));
    CHECK(std::string(social::GapEventName(social::Kind::Duel)) == "duel_gap" && std::string(social::GapEventName(social::Kind::Gift)) == "gift_gap");
    // An older core answers "unknown action": the policy logs the *_gap fallback.
    PolicyOptions on = Quiet();
    on.Duels.Enabled = true;
    std::vector<std::string> events;
    on.OnEvent = [&events](std::string const& line) { events.push_back(line); };
    BotPolicy policy(on);
    std::vector<social::Order> orders = OrdersOf(on, View(20, 0, { Player(77, 10.0f) }), policy);
    social::Outcome refused;
    refused.What = social::Kind::Duel;
    refused.Bot = kBot;
    refused.Reason = "unknown action";
    policy.NoteSocialOutcome(refused, kNow + 100);
    bool gap = false;
    for (std::string const& line : events)
        gap = gap || line.rfind("duel_gap", 0) == 0;
    CHECK(orders.size() == 1 && gap);
}

}  // namespace

int main()
{
    DuelsAreSentWhenOnAndNotWhenOff();
    BattlegroundsAreSentWhenOnAndNotWhenOff();
    GuildAnswersAreSentWhenOnAndNotWhenOff();
    GiftsAreSentWhenOnAndNotWhenOff();
    PickGiftChoosesSmallStacksOnly();
    OrdersWithoutABotOrAnUnknownCoreAreHandled();
    if (failures == 0)
        std::printf("social: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
