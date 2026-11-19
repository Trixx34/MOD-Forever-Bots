// SPDX-License-Identifier: GPL-2.0-or-later
// Dynamic parties (config DynamicParties; party_reason.h): BuildParties cuts the world bots into parties once per tick, and this keeps in each party only the
// members that have a reason to be in it now. The owner's rule (2026-11-14): a bot is in a party only while it makes sense, on the same quests or for a group
// quest; it leaves to do its professions, auctions, vendor, training; and it never groups with a bot on another map (Desolace and Ironforge).
// What is dropped here is handled by the existing party code: TurnTrainAndParty's leader invites only the members left in membersOf, and a bot in a bot
// party that is not in leaderOf leaves it (the "not chosen for the parties" branch). A dungeon run, a tank-led route and a scenario are never judged.
#include "bot_policy_internal.h"

namespace {

bool HasUnfinished(BotSnapshot const& bot, std::uint32_t quest)
{
    for (std::size_t i = 0; i < bot.quest_count && i < forever_module::kMaxQuestLog; ++i)
        if (bot.quests[i].quest_id == quest)
            return bot.quests[i].status == 1;
    return false;
}

// A quest both bots have in the log and neither has finished: the objectives they share.
bool SharesQuest(BotSnapshot const& a, BotSnapshot const& b)
{
    for (std::size_t i = 0; i < a.quest_count && i < forever_module::kMaxQuestLog; ++i)
        if (a.quests[i].quest_id != 0 && a.quests[i].status == 1 && HasUnfinished(b, a.quests[i].quest_id))
            return true;
    return false;
}

constexpr std::size_t kDungeonPartyMin = 3;   // DriveLevelDungeons' kLevelDungeonMinMembers: a smaller party is not sent into a dungeon, so it is no reason to gather

} // namespace

void BotPolicy::ApplyPartyReasons(WorldTickSnapshotView const& view, std::unordered_map<std::uint64_t, std::size_t> const& indexOf,
                                  std::unordered_map<std::uint64_t, std::uint64_t>& leaderOf,
                                  std::unordered_map<std::uint64_t, std::vector<std::uint64_t>>& membersOf, std::uint64_t nowMs)
{
    party_reason::Settings const& settings = Opt().DynamicParties;
    if (!settings.Enabled || membersOf.empty())
        return;

    if (!_questGroupSizeBuilt)
    {
        _questGroupSizeBuilt = true;
        if (Opt().Quests)
            for (QuestStartDef const& start : Opt().Quests->Starts)
                for (QuestDef const& quest : start.Quests)
                    if (quest.Group >= 2)
                        _questGroupSize[quest.Id] = quest.Group;
    }

    auto const event = [&](std::string const& text)
    {
        if (Opt().OnEvent)
            Opt().OnEvent(text);
    };
    auto const snapshotOf = [&](std::uint64_t guid) -> BotSnapshot const*
    {
        auto const at = indexOf.find(guid);
        return at == indexOf.end() ? nullptr : &view.bots[at->second];
    };
    // On an errand: a profession or Auction House errand, a vendor, supply or repair walk, a walk to an anvil or forge, a train order's walk, a flight.
    auto const onErrand = [&](BotSnapshot const& bot, BotState const& state)
    {
        return state.Errand.has_value() || state.Traveling || state.RepairTrip || state.CraftWalking || (state.GotoActive && state.TrainAfterGoto) ||
               bot.in_flight != 0 || (state.TaxiFlight.OrderedMs != 0 && nowMs < state.TaxiWaitUntilMs);
    };
    // Needs one now: broken gear (or worn after a death), short of supplies, or bags too full to hand in a quest.
    auto const needsErrand = [&](BotSnapshot const& bot, BotState const& state)
    {
        if (state.ServiceUrgent)
            return true;
        if (nowMs >= state.NextRepairMs && state.RepairCheckAtMs == 0)
        {
            repair::Facts facts;
            facts.LowestPct = bot.durability_pct;
            facts.RepairmanKnown = bot.repair_valid != 0;
            facts.AfterDeath = state.RepairAfterDeath;
            facts.Copper = bot.copper;
            facts.Level = bot.level;
            if (repair::Needed(Opt().Repair, facts) != repair::Why::None)
                return true;
        }
        return nowMs >= state.NextSupplyMs && state.SupplyCheckAtMs == 0 && supply::NextStop(supply::Assess(Opt().Supplies, FactsOf(bot))) != supply::Stop::None;
    };
    auto const busy = [](BotSnapshot const& bot) { return bot.life_state != 0 || bot.in_combat != 0; };
    // In a group quest's party (DriveQuestGroups) that is not this one: led by a registered quest-group leader.
    auto const inQuestGroup = [&](BotSnapshot const& bot, std::uint64_t exceptLeader)
    {
        if (bot.group_role == 0 || bot.group_leader_guid == 0)
            return false;
        std::uint64_t const leader = bot.group_leader_guid & forever_module::kUnitRefCounterMask;
        return leader != exceptLeader && _questGroupLeaders.count(leader) != 0;
    };

    std::vector<std::uint64_t> leaders;
    leaders.reserve(membersOf.size());
    for (auto const& entry : membersOf)
        leaders.push_back(entry.first);
    for (std::uint64_t const leaderGuid : leaders)
    {
        std::vector<std::uint64_t>& members = membersOf[leaderGuid];
        BotSnapshot const* const leader = snapshotOf(leaderGuid);
        if (leader == nullptr)
            continue; // offline: nobody can be invited to it anyway
        BotState& leaderState = StateOf(leaderGuid);

        // Assigned groups: a level dungeon run, a tank-led route. Every member is in, and stays in until the run is over.
        if (_levelRuns.count(leaderGuid) != 0 || leaderState.TankLead || leaderState.LevelRun || IsRouteActive(leaderGuid))
        {
            for (std::uint64_t const member : members)
            {
                party_reason::Memory& memory = StateOf(member).PartyMemory;
                if (!memory.In)
                {
                    memory.In = true;
                    memory.LastWhy = party_reason::Why::Dungeon;
                    memory.SinceMs = nowMs;
                }
                memory.BreakDueMs = settings.ErrandEveryMs != 0 ? nowMs + settings.ErrandEveryMs : 0;   // no errand break right after the run
            }
            continue;
        }

        bool const leaderFrozen = busy(*leader);
        bool leading = false;
        for (std::uint64_t const member : members)
            leading = leading || StateOf(member).PartyMemory.In;
        // The leader leads a group quest's party, or sits in one: the world party is off meanwhile.
        bool const leaderInQuestGroup = _questGroupLeaders.count(leaderGuid) != 0 || inQuestGroup(*leader, 0);
        bool leaderBusy = leaderInQuestGroup;
        if (!leaderFrozen && !leaderBusy)
            leaderBusy = onErrand(*leader, leaderState) || needsErrand(*leader, leaderState) ||
                         party_reason::LeaderOnBreak(settings, leading, leaderState.PartyLeadMemory, nowMs);

        // A level dungeon the party is due for (LevelDungeons): its level is the lowest of the leader and the members on its map.
        bool dungeonDue = false;
        if (Opt().LevelDungeons && !leaderBusy)
        {
            std::uint32_t level = leader->level;
            std::size_t here = 1;
            for (std::uint64_t const member : members)
                if (BotSnapshot const* const other = snapshotOf(member); other && other->map_id == leader->map_id && other->life_state == 0)
                {
                    level = std::min(level, other->level);
                    ++here;
                }
            dungeonDue = here >= kDungeonPartyMin && Opt().DungeonsByMap.count(leader->map_id) == 0 && LevelDungeonDue(leaderGuid, level);
        }
        bool leaderGroupQuest = false;
        for (std::size_t i = 0; i < leader->quest_count && i < forever_module::kMaxQuestLog && !leaderGroupQuest; ++i)
            leaderGroupQuest = leader->quests[i].status == 1 && _questGroupSize.count(leader->quests[i].quest_id) != 0;

        std::vector<std::uint64_t> kept;
        kept.reserve(members.size());
        for (std::uint64_t const member : members)
        {
            BotSnapshot const* const other = snapshotOf(member);
            if (other == nullptr)
                continue; // offline
            BotState& state = StateOf(member);
            party_reason::Facts facts;
            facts.SameMap = other->map_id == leader->map_id;
            facts.Distance = facts.SameMap ? Distance2d(*other, *leader) : 1e9f;
            facts.SharedQuest = SharesQuest(*other, *leader);
            facts.GroupQuest = leaderGroupQuest;
            facts.DungeonDue = dungeonDue;
            facts.OnErrand = onErrand(*other, state);
            facts.NeedsErrand = !busy(*other) && needsErrand(*other, state);
            facts.LeaderBusy = leaderBusy;
            facts.OtherParty = inQuestGroup(*other, leaderGuid) || _questGroupLeaders.count(member) != 0;
            // A fight or a death freezes the party as it is, but never across maps: a member whose leader is on another map cannot help it.
            facts.Frozen = facts.SameMap && !leaderInQuestGroup && (leaderFrozen || busy(*other));
            party_reason::Verdict const verdict = party_reason::Judge(settings, facts, state.PartyMemory, nowMs);
            if (verdict.Changed)
            {
                if (verdict.In)
                    event("party_join: bot=" + std::to_string(member) + " leader=" + std::to_string(leaderGuid) + " why=" + party_reason::WhyName(verdict.Reason) +
                          " map=" + std::to_string(other->map_id) + " dist=" + std::to_string(int(facts.Distance)));
                else
                    event("party_leave: bot=" + std::to_string(member) + " leader=" + std::to_string(leaderGuid) + " why=" + party_reason::OutName(verdict.Cause) +
                          " map=" + std::to_string(other->map_id) + " leader_map=" + std::to_string(leader->map_id) +
                          (facts.SameMap ? " dist=" + std::to_string(int(facts.Distance)) : std::string()));
            }
            if (verdict.In)
                kept.push_back(member);
            else
                leaderOf.erase(member);
        }
        if (kept.empty())
        {
            leaderOf.erase(leaderGuid);
            membersOf.erase(leaderGuid);
        }
        else
            members.swap(kept);
    }
}
