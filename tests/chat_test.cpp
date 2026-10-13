// Unit tests for the chat commands: the parser (chat_commands.h) and BotPolicy::ApplyChat. No core needed.
#include "bot_policy.h"
#include "chat_commands.h"

#include <cstdio>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotSnapshot;
using forever_module::MakeUnitRef;
using forever_module::NearbyUnit;
using forever_module::WorldTickSnapshotView;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

constexpr std::uint64_t kPlayer = 900; // the human leader
std::uint64_t PlayerRef() { return MakeUnitRef(2, 0, kPlayer); }

void ParsesBasicVerbs()
{
    CHECK(ParseChatCommand("follow", false)->Verb == ChatVerb::Follow);
    CHECK(ParseChatCommand("Follow off", false)->Verb == ChatVerb::FollowOff);
    CHECK(ParseChatCommand("stay", false)->Verb == ChatVerb::Stay);
    CHECK(ParseChatCommand("stay off", false)->Verb == ChatVerb::StayOff);
    CHECK(ParseChatCommand("goto here", false)->Verb == ChatVerb::GotoHere);
    CHECK(ParseChatCommand("release", false)->Verb == ChatVerb::Release);
    CHECK(ParseChatCommand("status", false)->Verb == ChatVerb::Status);
}

void OrdersNeedTheSwitch()
{
    CHECK(!ParseChatCommand("pull", false));
    CHECK(ParseChatCommand("pull", true)->Verb == ChatVerb::Pull);
    CHECK(ParseChatCommand("heal", true)->Verb == ChatVerb::Heal);
    CHECK(!ParseChatCommand("stop", false) && ParseChatCommand("stop", true)->Verb == ChatVerb::Stop);
    CHECK(ParseChatCommand("passive", true)->Verb == ChatVerb::Passive);
    CHECK(ParseChatCommand("aggressive", true)->Verb == ChatVerb::Aggressive);
}

void OrdinaryChatIsNotACommand()
{
    CHECK(!ParseChatCommand("hello there", true));
    CHECK(!ParseChatCommand("follow me please now", true));
    CHECK(!ParseChatCommand("goto", true));
    CHECK(!ParseChatCommand("goto there", true));
    CHECK(!ParseChatCommand("pull the boss", true));
    CHECK(!ParseChatCommand("", true));
    CHECK(!ParseChatCommand("all", true)); // a selector alone is not a command
}

void ParsesSelectors()
{
    auto cmd = ParseChatCommand("healer follow", false);
    CHECK(cmd && !cmd->Selector.All && cmd->Selector.Matches(5) && cmd->Selector.Matches(11) && !cmd->Selector.Matches(1));
    cmd = ParseChatCommand("tank pull", true);
    CHECK(cmd && cmd->Selector.Matches(1) && !cmd->Selector.Matches(5));
    cmd = ParseChatCommand("dps stay", false);
    CHECK(cmd && cmd->Selector.Matches(4) && cmd->Selector.Matches(8) && !cmd->Selector.Matches(5));
    cmd = ParseChatCommand("Rogues stay off", false);
    CHECK(cmd && cmd->Verb == ChatVerb::StayOff && cmd->Selector.Matches(4) && !cmd->Selector.Matches(1));
    cmd = ParseChatCommand("priest,rogue follow", false);
    CHECK(cmd && cmd->Selector.Matches(5) && cmd->Selector.Matches(4) && !cmd->Selector.Matches(8));
    cmd = ParseChatCommand("all status", false);
    CHECK(cmd && cmd->Selector.All);
    cmd = ParseChatCommand("status", false);
    CHECK(cmd && cmd->Selector.All);
}

// Bots 10..14 led by the human player: 10 warrior, 11 priest, 12 mage, 13 rogue, 14 priest.
std::uint32_t ClassOf(std::uint64_t guid)
{
    switch (guid)
    {
        case 10: return 1;
        case 11: return 5;
        case 12: return 8;
        case 13: return 4;
        default: return 5;
    }
}

WorldTickSnapshotView Group()
{
    WorldTickSnapshotView view;
    view.tick_number = 1;
    view.max_nearby_units = 4;
    for (std::uint64_t guid = 10; guid <= 14; ++guid)
    {
        BotSnapshot bot;
        bot.guid = guid;
        bot.health = 1000;
        bot.max_health = 1000;
        bot.level = 20;
        bot.map_id = 0;
        bot.group_leader_guid = PlayerRef();
        view.bots.push_back(bot);
    }
    view.nearby.resize(5 * view.max_nearby_units);
    return view;
}

BotPolicy Policy()
{
    PolicyOptions options;
    options.ClassOf = ClassOf;
    return BotPolicy(options);
}

bool Has(std::vector<BotAction> const& actions, std::uint64_t bot, ActionType type, std::uint64_t target = 0)
{
    for (BotAction const& action : actions)
        if (action.bot_guid == bot && action.type == type && (target == 0 || action.target_guid == target))
            return true;
    return false;
}

BotPolicy::ChatContext Ctx()
{
    BotPolicy::ChatContext ctx;
    ctx.Issuer = kPlayer;
    ctx.Pos = Point{ 100.0f, 200.0f, 5.0f };
    return ctx;
}

void FollowMakesTheBotsFollowTheSpeaker()
{
    BotPolicy policy = Policy();
    WorldTickSnapshotView view = Group();
    auto lines = policy.ApplyChat(view, Ctx(), *ParseChatCommand("follow", false), 1000);
    CHECK(lines.empty());
    auto const actions = policy.Decide(view, 1000);
    for (std::uint64_t guid = 10; guid <= 14; ++guid)
        CHECK(Has(actions, guid, ActionType::kFollow, PlayerRef()));
}

void SelectorLimitsWhoObeys()
{
    BotPolicy policy = Policy();
    WorldTickSnapshotView view = Group();
    policy.ApplyChat(view, Ctx(), *ParseChatCommand("healer follow", false), 1000);
    auto const actions = policy.Decide(view, 1000);
    CHECK(Has(actions, 11, ActionType::kFollow, PlayerRef()) && Has(actions, 14, ActionType::kFollow, PlayerRef()));
    CHECK(!Has(actions, 10, ActionType::kFollow, PlayerRef()) && !Has(actions, 12, ActionType::kFollow, PlayerRef()));
}

void OnlyTheSpeakersOwnGroupObeys()
{
    BotPolicy policy = Policy();
    WorldTickSnapshotView view = Group();
    BotPolicy::ChatContext other = Ctx();
    other.Issuer = 901; // not the leader of these bots
    auto lines = policy.ApplyChat(view, other, *ParseChatCommand("follow", false), 1000);
    CHECK(lines.size() == 1 && lines[0].find("No bot") == 0);
    auto const actions = policy.Decide(view, 1000);
    CHECK(!Has(actions, 10, ActionType::kFollow, MakeUnitRef(2, 0, 901)));
}

void WhisperReachesOnlyThatBot()
{
    BotPolicy policy = Policy();
    WorldTickSnapshotView view = Group();
    BotPolicy::ChatContext whisper = Ctx();
    whisper.Recipient = 12;
    policy.ApplyChat(view, whisper, *ParseChatCommand("follow", false), 1000);
    auto const actions = policy.Decide(view, 1000);
    CHECK(Has(actions, 12, ActionType::kFollow, PlayerRef()));
    CHECK(!Has(actions, 11, ActionType::kFollow, PlayerRef()));
}

void StayHoldsAndFollowOffReleases()
{
    BotPolicy policy = Policy();
    WorldTickSnapshotView view = Group();
    policy.ApplyChat(view, Ctx(), *ParseChatCommand("follow", false), 1000);
    policy.Decide(view, 1000);
    policy.ApplyChat(view, Ctx(), *ParseChatCommand("stay", false), 1100);
    auto const actions = policy.Decide(view, 1100);
    CHECK(Has(actions, 10, ActionType::kStop));
    CHECK(!Has(actions, 10, ActionType::kFollow));
}

void StayBotsStartNoFights()
{
    BotPolicy policy = Policy();
    WorldTickSnapshotView view = Group();
    NearbyUnit mob;
    mob.guid = MakeUnitRef(8, 7, 3);
    mob.pos_x = 10.0f;
    mob.health = 100;
    mob.is_hostile = true;
    view.nearby[1 * view.max_nearby_units] = mob; // bot 11 sees a mob
    view.bots[1].nearby_count = 1;
    policy.ApplyChat(view, Ctx(), *ParseChatCommand("stay", false), 1000);
    auto const actions = policy.Decide(view, 1000);
    CHECK(!Has(actions, 11, ActionType::kFollow, mob.guid) && !Has(actions, 11, ActionType::kAttackTarget));
}

void GotoHereSendsTheBotsToTheSpeaker()
{
    BotPolicy policy = Policy();
    WorldTickSnapshotView view = Group();
    policy.ApplyChat(view, Ctx(), *ParseChatCommand("goto here", false), 1000);
    auto const actions = policy.Decide(view, 1000);
    int moves = 0;
    for (BotAction const& action : actions)
        if (action.type == ActionType::kMoveTo)
        {
            ++moves;
            float const dx = action.dest_x - 100.0f, dy = action.dest_y - 200.0f;
            CHECK(dx * dx + dy * dy < 20.0f * 20.0f);
        }
    CHECK(moves == 5);
}

void GotoOnAnotherMapIsRefused()
{
    BotPolicy policy = Policy();
    WorldTickSnapshotView view = Group();
    BotPolicy::ChatContext ctx = Ctx();
    ctx.MapId = 36;
    auto lines = policy.ApplyChat(view, ctx, *ParseChatCommand("goto here", false), 1000);
    CHECK(lines.size() == 1 && lines[0].find("refused") != std::string::npos);
}

void PullIsForTanksAndNeedsATarget()
{
    BotPolicy policy = Policy();
    WorldTickSnapshotView view = Group();
    BotPolicy::ChatContext ctx = Ctx();
    auto lines = policy.ApplyChat(view, ctx, *ParseChatCommand("pull", true), 1000); // no target selected
    CHECK(!lines.empty() && lines[0].find("refused") != std::string::npos);

    NearbyUnit mob;
    mob.guid = MakeUnitRef(8, 7, 3);
    mob.pos_x = 20.0f;
    mob.health = 100;
    mob.is_hostile = true;
    view.nearby[0] = mob; // bot 10 (the warrior) sees it
    view.bots[0].nearby_count = 1;
    ctx.TargetRef = mob.guid;
    lines = policy.ApplyChat(view, ctx, *ParseChatCommand("tank pull", true), 1000);
    CHECK(lines.empty());
    auto const actions = policy.Decide(view, 1000);
    CHECK(Has(actions, 10, ActionType::kFollow, mob.guid) || Has(actions, 10, ActionType::kAttackTarget));

    lines = policy.ApplyChat(view, ctx, *ParseChatCommand("priest pull", true), 1100); // a priest is not a tank
    CHECK(!lines.empty() && lines[0].find("not a tank") != std::string::npos);
}

void HealCastsOnTheSelectedFriend()
{
    BotPolicy policy = Policy();
    WorldTickSnapshotView view = Group();
    BotPolicy::ChatContext ctx = Ctx();
    ctx.TargetRef = MakeUnitRef(2, 0, 12); // bot 12
    policy.ApplyChat(view, ctx, *ParseChatCommand("healer heal", true), 1000);
    auto const actions = policy.Decide(view, 1000);
    CHECK(Has(actions, 11, ActionType::kCastSpell, ctx.TargetRef));
    CHECK(Has(actions, 14, ActionType::kCastSpell, ctx.TargetRef));
    CHECK(!Has(actions, 10, ActionType::kCastSpell));
    // once only
    auto const again = policy.Decide(view, 1000 + 5000);
    CHECK(!Has(again, 11, ActionType::kCastSpell, ctx.TargetRef));
}

void ReleaseOnlyForTheDead()
{
    BotPolicy policy = Policy();
    WorldTickSnapshotView view = Group();
    auto lines = policy.ApplyChat(view, Ctx(), *ParseChatCommand("release", false), 1000);
    CHECK(!lines.empty() && lines[0].find("refused") != std::string::npos);

    // A dead bot with a living healer waits for a resurrect; the release order skips the wait.
    view = Group();
    view.bots[2].health = 0;
    view.bots[2].life_state = 1;
    PolicyOptions partyOptions;
    partyOptions.ClassOf = ClassOf;
    partyOptions.PartySize = 5; // the module's own party: a living priest is there to resurrect, so the dead bot waits
    BotPolicy second(partyOptions);
    CHECK(!Has(second.Decide(view, 1000), 12, ActionType::kReleaseSpirit));
    second.ApplyChat(view, Ctx(), *ParseChatCommand("release", false), 1500);
    auto const actions = second.Decide(view, 2000);
    CHECK(Has(actions, 12, ActionType::kReleaseSpirit));
}

void StatusGivesALinePerBot()
{
    BotPolicy policy = Policy();
    WorldTickSnapshotView view = Group();
    policy.Decide(view, 1000);
    auto const lines = policy.ApplyChat(view, Ctx(), *ParseChatCommand("status", false), 1100);
    CHECK(lines.size() == 5);
    CHECK(lines[0].find("Bot 10") == 0 && lines[0].find("hp 100%") != std::string::npos);
}

void PassiveBotsDoNotAssist()
{
    BotPolicy policy = Policy();
    WorldTickSnapshotView view = Group();
    NearbyUnit mob;
    mob.guid = MakeUnitRef(8, 7, 3);
    mob.pos_x = 10.0f;
    mob.health = 100;
    mob.is_hostile = true;
    view.nearby[2 * view.max_nearby_units] = mob;
    view.bots[2].nearby_count = 1;
    policy.ApplyChat(view, Ctx(), *ParseChatCommand("passive", true), 1000);
    auto actions = policy.Decide(view, 1000);
    CHECK(!Has(actions, 12, ActionType::kFollow, mob.guid) && !Has(actions, 12, ActionType::kAttackTarget));
    // Attacked: it defends itself.
    view.bots[2].in_combat = 1;
    actions = policy.Decide(view, 2000);
    CHECK(Has(actions, 12, ActionType::kFollow, mob.guid) || Has(actions, 12, ActionType::kAttackTarget));
}

void AcceptsAPlayersInvite()
{
    PolicyOptions options;
    options.ClassOf = ClassOf;
    options.AcceptInvites = true;
    BotPolicy policy(options);
    WorldTickSnapshotView view = Group();
    view.bots[0].group_leader_guid = 0;
    view.bots[0].group_invite_from = PlayerRef();
    auto actions = policy.Decide(view, 1000);
    bool accepted = false;
    for (BotAction const& action : actions)
        accepted = accepted || (action.bot_guid == 10 && action.type == ActionType::kGroup && action.spell_id == 2);
    CHECK(accepted);

    BotPolicy off = Policy(); // the switch is off by default
    actions = off.Decide(view, 1000);
    for (BotAction const& action : actions)
        CHECK(!(action.bot_guid == 10 && action.type == ActionType::kGroup));
}

void AltsAreNotAutoGeared()
{
    PolicyOptions options;
    options.ClassOf = ClassOf;
    options.AutoGearing = true;
    options.IsAlt = [](std::uint64_t guid) { return guid == 11; };
    BotPolicy policy(options);
    auto const actions = policy.Decide(Group(), 1000);
    CHECK(Has(actions, 10, ActionType::kAutoGear));
    CHECK(!Has(actions, 11, ActionType::kAutoGear));
}

} // namespace

int main()
{
    ParsesBasicVerbs();
    OrdersNeedTheSwitch();
    OrdinaryChatIsNotACommand();
    ParsesSelectors();
    FollowMakesTheBotsFollowTheSpeaker();
    SelectorLimitsWhoObeys();
    OnlyTheSpeakersOwnGroupObeys();
    WhisperReachesOnlyThatBot();
    StayHoldsAndFollowOffReleases();
    StayBotsStartNoFights();
    GotoHereSendsTheBotsToTheSpeaker();
    GotoOnAnotherMapIsRefused();
    PullIsForTanksAndNeedsATarget();
    HealCastsOnTheSelectedFriend();
    ReleaseOnlyForTheDead();
    StatusGivesALinePerBot();
    PassiveBotsDoNotAssist();
    AcceptsAPlayersInvite();
    AltsAreNotAutoGeared();
    if (failures == 0)
        std::printf("chat_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
