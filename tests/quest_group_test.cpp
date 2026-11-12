// SPDX-License-Identifier: GPL-2.0-or-later
// Unit tests for the group quest invites (quest_group.h): who leads, who is invited, and the planner's WaitGroup step.
#include "quest_data.h"
#include "quest_group.h"
#include "quest_planner.h"

#include <cstdio>
#include <memory>
#include <sstream>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

quest_group::Settings On()
{
    quest_group::Settings settings;
    settings.Enabled = true;
    return settings;
}

quest_group::Candidate Bot(std::uint64_t guid, float x, bool free = true)
{
    quest_group::Candidate c;
    c.Guid = guid;
    c.Map = 0;
    c.X = x;
    c.Free = free;
    return c;
}

quest_group::Candidate Seeker(std::uint64_t guid, float x, std::uint32_t quest, std::uint32_t size)
{
    quest_group::Candidate c = Bot(guid, x);
    c.Waiting = quest;
    c.WantSize = size;
    c.Log = { quest };
    return c;
}

void OffByDefault()
{
    quest_group::Settings off;
    CHECK(quest_group::PlanInvites(off, { Seeker(1, 0, 900, 3), Seeker(2, 5, 900, 3) }).empty());
}

void TheLowestGuidOfTheSeekersLeadsAndInvitesTheOthersOnTheQuest()
{
    auto const invites = quest_group::PlanInvites(On(), { Seeker(7, 0, 900, 3), Seeker(3, 10, 900, 3), Seeker(5, 20, 900, 3), Bot(9, 5) });
    CHECK(invites.size() == 2);
    CHECK(invites[0].Leader == 3 && invites[0].Invitee == 5 && invites[0].Quest == 900); // nearest first among the same quest
    CHECK(invites[1].Leader == 3 && invites[1].Invitee == 7);
    // Bot 9 has neither the quest nor a guild: not invited.
    for (auto const& invite : invites)
        CHECK(invite.Invitee != 9);
}

void APartyOfFiveFromThreeSeekersAndGuildMates()
{
    quest_group::Candidate a = Seeker(1, 0, 900, 5);
    a.Guild = 77;
    quest_group::Candidate m1 = Bot(2, 10);
    m1.Guild = 77;
    quest_group::Candidate m2 = Bot(3, 20);
    m2.Guild = 88;
    quest_group::Candidate m3 = Bot(4, 30);
    m3.Guild = 77;
    auto invites = quest_group::PlanInvites(On(), { a, Seeker(5, 15, 900, 5), m1, m2, m3 });
    CHECK(invites.size() == 3); // the seeker (same quest), then the two guild mates; bot 3 is another guild's
    CHECK(invites[0].Invitee == 5 && invites[1].Invitee == 2 && invites[2].Invitee == 4);
    quest_group::Settings noGuild = On();
    noGuild.GuildHelpers = false;
    invites = quest_group::PlanInvites(noGuild, { a, Seeker(5, 15, 900, 5), m1, m2, m3 });
    CHECK(invites.size() == 1 && invites[0].Invitee == 5);
}

void OnlyBotsInRangeOnTheSameMapAndFreeAreInvited()
{
    quest_group::Candidate far = Seeker(2, 500, 900, 3);
    quest_group::Candidate otherMap = Seeker(3, 5, 900, 3);
    otherMap.Map = 1;
    quest_group::Candidate busy = Seeker(4, 5, 900, 3);
    busy.Free = false;
    // Seeker 1 finds nobody; each of the others is then a seeker without partners for itself, and none is invited by 1.
    auto const invites = quest_group::PlanInvites(On(), { Seeker(1, 0, 900, 3), far, otherMap, busy });
    for (auto const& invite : invites)
        CHECK(invite.Leader != 1);
}

void ALeaderWithAPartyInvitesOnlyForThePlacesLeft()
{
    quest_group::Candidate leader = Seeker(1, 0, 900, 4);
    leader.PartySize = 3;
    leader.Leads = true;
    auto invites = quest_group::PlanInvites(On(), { leader, Seeker(2, 5, 900, 4), Seeker(3, 6, 900, 4) });
    CHECK(invites.size() == 1 && invites[0].Invitee == 2);
    leader.PartySize = 4;
    CHECK(quest_group::PlanInvites(On(), { leader, Seeker(2, 5, 900, 4) }).empty());
}

void ASeekerInAnotherBotsPartyDoesNotLead()
{
    quest_group::Candidate member = Seeker(1, 0, 900, 3);
    member.PartySize = 2; // in somebody else's party, not leading
    member.Free = false;
    auto const invites = quest_group::PlanInvites(On(), { member, Seeker(2, 5, 900, 3), Seeker(3, 6, 900, 3) });
    for (auto const& invite : invites)
        CHECK(invite.Leader != 1);
}

std::shared_ptr<QuestData const> GroupData()
{
    std::istringstream in("start A\nanchor 0 0 0 0\nquest 900 giver 0 0 0 ender 0 0 0\nreq 900 level 60 group 3\nhunt 900 0 5 77\narea 900 100 0 0\n");
    std::vector<std::string> warnings;
    auto data = std::make_shared<QuestData>(ParseQuestData(in, warnings));
    CHECK(warnings.empty() && data->Starts[0].Quests[0].Group == 3);
    return data;
}

forever_module::BotSnapshot Logged(std::uint32_t partySize)
{
    forever_module::BotSnapshot bot;
    bot.guid = 10;
    bot.level = 60;
    bot.map_id = 0;
    bot.group_size = partySize;
    bot.quest_count = 1;
    bot.quests[0].quest_id = 900;
    bot.quests[0].status = 1;
    return bot;
}

void AGroupQuestIsNotTakenUnlessGroupsAreOn()
{
    QuestPlanner off(GroupData());
    QuestMemory memory;
    forever_module::BotSnapshot bot;
    bot.level = 60;
    CHECK(off.Next(bot, memory, 1000).Kind == QuestStepKind::None);
    QuestPlannerOptions options;
    options.GroupQuests = true;
    QuestPlanner on(GroupData(), nullptr, options);
    QuestMemory memory2;
    CHECK(on.Next(bot, memory2, 1000).Kind == QuestStepKind::Accept);
}

void TheLoggedGroupQuestWaitsForTheParty()
{
    QuestPlannerOptions options;
    options.GroupQuests = true;
    options.GroupWaitMs = 100000;
    QuestPlanner planner(GroupData(), nullptr, options);
    QuestMemory memory;
    QuestStep step = planner.Next(Logged(0), memory, 1000);
    CHECK(step.Kind == QuestStepKind::WaitGroup && step.Quest == 900 && step.GroupSize == 3);
    step = planner.Next(Logged(2), memory, 50000);
    CHECK(step.Kind == QuestStepKind::WaitGroup); // two of three
    step = planner.Next(Logged(3), memory, 60000);
    CHECK(step.Kind == QuestStepKind::Hunt && step.Quest == 900); // the party is there: hunt
    // The party broke up: the wait starts again from now, not from the first one.
    step = planner.Next(Logged(1), memory, 120000);
    CHECK(step.Kind == QuestStepKind::WaitGroup);
    step = planner.Next(Logged(1), memory, 120000 + options.GroupWaitMs + 1);
    CHECK(step.Kind == QuestStepKind::Abandon && step.Quest == 900); // the timeout: the quest is given up
    planner.NoteSent(memory, kQuestAbandon, 900, 300000);
    CHECK(memory.BlockedUntilMs[900] == 300000 + kBlockAfterAbandonMs);
}

void TheWaitDoesNotCountAsStuck()
{
    QuestPlannerOptions options;
    options.GroupQuests = true;
    options.GroupWaitMs = 2 * kStuckMs;
    QuestPlanner planner(GroupData(), nullptr, options);
    QuestMemory memory;
    planner.Next(Logged(0), memory, 1000);
    CHECK(planner.Next(Logged(0), memory, 1000 + kStuckMs + 5000).Kind == QuestStepKind::WaitGroup);
    // The party forms: the hunt is not abandoned for the minutes spent waiting.
    CHECK(planner.Next(Logged(3), memory, 1000 + kStuckMs + 6000).Kind == QuestStepKind::Hunt);
}

void ACompleteGroupQuestIsHandedInWithoutAParty()
{
    QuestPlannerOptions options;
    options.GroupQuests = true;
    QuestPlanner planner(GroupData(), nullptr, options);
    QuestMemory memory;
    forever_module::BotSnapshot bot = Logged(0);
    bot.quests[0].status = 2;
    CHECK(planner.Next(bot, memory, 1000).Kind == QuestStepKind::HandIn);
}

} // namespace

int main()
{
    OffByDefault();
    TheLowestGuidOfTheSeekersLeadsAndInvitesTheOthersOnTheQuest();
    APartyOfFiveFromThreeSeekersAndGuildMates();
    OnlyBotsInRangeOnTheSameMapAndFreeAreInvited();
    ALeaderWithAPartyInvitesOnlyForThePlacesLeft();
    ASeekerInAnotherBotsPartyDoesNotLead();
    AGroupQuestIsNotTakenUnlessGroupsAreOn();
    TheLoggedGroupQuestWaitsForTheParty();
    TheWaitDoesNotCountAsStuck();
    ACompleteGroupQuestIsHandedInWithoutAParty();
    if (failures == 0)
        std::printf("quest_group_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
