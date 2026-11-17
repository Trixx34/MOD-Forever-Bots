// SPDX-License-Identifier: GPL-2.0-or-later
// BotPolicy::TurnReputation: the reputation goal of a bot and its visits to the vendor of a reputation-gated upgrade (reputation.h, doc/reputation.md).
#include "bot_policy_internal.h"

void BotPolicy::TurnReputation(DecideTick& tick, BotTurn& turn)
{
    if (!_options.Reputation.Enabled)
        return;
    std::uint64_t const nowMs = tick.nowMs;
    BotSnapshot const& bot = turn.bot;
    BotState& state = turn.state;

    // The planner: which faction and which quest or creature earns the bot reputation now. It is cheap when nothing is due and bounded by the budget when it plans.
    if (_options.ReputationPlan.Enabled && _options.ReputationPlanData && bot.level >= _options.Reputation.MinLevel)
    {
        reputation::Facts planFacts = _options.ReputationFacts ? _options.ReputationFacts(bot) : reputation::Facts{};
        planFacts.Level = bot.level;
        if (planFacts.Side == 0 && _options.SideOf)
            planFacts.Side = _options.SideOf(bot.guid);
        reputation::Step const step = reputation::Advance(*_options.ReputationPlanData, _options.ReputationPlan, state.RepTrack, planFacts, bot.guid, nowMs);
        if (step.Valid && step.Changed && _options.OnEvent)
            _options.OnEvent("rep_plan: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " faction=" + std::to_string(step.Faction) +
                             (step.Src->What == reputation::Source::kKill ? " kill=" : " quest=") + std::to_string(step.Src->Id) + " points=" + std::to_string(step.Src->Points));
    }
    if (!_options.ReputationFacts)
        return;
    reputation::Settings const& settings = _options.Reputation;

    // A buy made earlier is judged once the core has counted again: less money or fewer free slots is a purchase; otherwise the vendor had nothing the bot
    // could use (the rank, the money or the gear score said no) and the next visit waits longer.
    if (state.RepCheckAtMs != 0 && nowMs >= state.RepCheckAtMs)
    {
        state.RepCheckAtMs = 0;
        bool const bought = bot.copper < state.RepCopperBefore || bot.bag_free_slots < state.RepSlotsBefore;
        reputation::NoteResult(settings, state.RepBook, nowMs, bought);
        if (_options.OnEvent)
            _options.OnEvent(std::string(bought ? "rep_buy_done" : "rep_buy_failed") + ": bot=" + std::to_string(bot.guid) + " faction=" +
                             std::to_string(state.RepGoal) + " copper=" + std::to_string(bot.copper) + " failures=" + std::to_string(state.RepBook.Failures));
    }

    // Only a bot with nothing else to do, alone, goes shopping for reputation gear.
    bool const free = turn.idle && !turn.hasTarget && bot.in_combat == 0 && !turn.lowHealth && !turn.partyMember && !state.InFight && !state.ChatStay &&
                      state.ChatFollow == 0 && !turn.stagingHold && !turn.dungeonHold && !turn.returningToDungeon;
    if (!free)
    {
        state.RepTrip = false;
        return;
    }

    reputation::Facts facts = _options.ReputationFacts(bot);
    facts.Level = bot.level;
    facts.Copper = bot.copper;
    facts.FreeSlots = bot.bag_free_slots;
    if (facts.Side == 0 && _options.SideOf)
        facts.Side = _options.SideOf(bot.guid);

    if (auto const goal = reputation::ChooseGoal(_options.ReputationGoals, facts, bot.guid, state.RepGoal))
    {
        std::uint32_t const faction = _options.ReputationGoals[*goal].Faction;
        if (faction != state.RepGoal)
        {
            state.RepGoal = faction;
            if (_options.OnEvent)
                _options.OnEvent("rep_goal: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " faction=" + std::to_string(faction) +
                                 " name=" + _options.ReputationGoals[*goal].Name);
        }
    }
    else
        state.RepGoal = 0;

    if (state.RepCheckAtMs != 0 || !reputation::TripDue(settings, facts, state.RepBook, nowMs))
    {
        state.RepTrip = false;
        return;
    }

    Point const vendor{ facts.VendorX, facts.VendorY, facts.VendorZ };
    if (DistanceTo(bot, vendor) > kVendorReachYards)
    {
        if (!state.RepTrip)
        {
            state.RepTrip = true;
            state.RepTripStartMs = nowMs;
        }
        if (nowMs - state.RepTripStartMs <= kTravelTimeoutMs)
        {
            turn.wanted = Decision{ ActionType::kMoveTo, 0, vendor };
            turn.moveWhy = "rep_vendor";
            turn.idle = false;
            return;
        }
        // The walk never got there: a visit that bought nothing.
        state.RepTrip = false;
        reputation::NoteResult(settings, state.RepBook, nowMs, false);
        if (_options.OnEvent)
            _options.OnEvent("rep_buy_failed: bot=" + std::to_string(bot.guid) + " reason=walk_timeout failures=" + std::to_string(state.RepBook.Failures));
        return;
    }

    state.RepTrip = false;
    BotAction buy;
    buy.bot_guid = bot.guid;
    buy.type = ActionType::kVendor;
    buy.spell_id = kServiceBuyReputation | (rotation::GearProfileFor(turn.classId, turn.spec) << kServiceGearProfileShift);
    buy.target_guid = facts.VendorGuid;
    tick.actions.push_back(buy);
    reputation::NoteTrip(settings, state.RepBook, nowMs);
    state.RepCheckAtMs = nowMs + kSupplyJudgeAfterMs;
    state.RepCopperBefore = bot.copper;
    state.RepSlotsBefore = bot.bag_free_slots;
    turn.idle = false;
    if (_options.OnEvent)
        _options.OnEvent("rep_buy: bot=" + std::to_string(bot.guid) + " level=" + std::to_string(bot.level) + " faction=" + std::to_string(state.RepGoal) +
                         " copper=" + std::to_string(bot.copper));
}
