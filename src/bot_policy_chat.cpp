// SPDX-License-Identifier: GPL-2.0-or-later
#include "bot_policy_internal.h"

std::vector<std::string> BotPolicy::ApplyChat(WorldTickSnapshotView const& view, ChatContext const& chat, ChatCommand const& command,
                                               std::uint64_t nowMs, std::vector<ChatAck>* acks)
{
    // help needs no bot (any party member can ask for it, led bots or not), and is never said out loud.
    if (command.Verb == ChatVerb::Help)
        return ChatHelpLines(chat.Orders, chat.Control);

    std::vector<std::string> lines;
    std::vector<std::uint64_t> accepted; // the bots that took the order, for the say-back
    std::uint64_t const issuerRef = PlayerRef(chat.Issuer);
    std::uint32_t matched = 0, refused = 0, index = 0;
    char const* refusal = "";
    for (BotSnapshot const& bot : view.bots)
    {
        // Only the bots of the speaker's own group obey, and a whisper reaches only the bot it names.
        if (bot.group_leader_guid != issuerRef || (chat.Recipient != 0 && bot.guid != chat.Recipient))
            continue;
        std::uint32_t const classId = ClassFor(_options, bot.guid);
        if (!command.Selector.Matches(classId))
            continue;
        ++matched;
        BotState& state = StateOf(bot.guid);
        std::uint32_t const refusedBefore = refused;
        auto const refuse = [&](char const* why)
        {
            ++refused;
            refusal = why;
        };
        switch (command.Verb)
        {
            case ChatVerb::Follow:
                state.ChatStay = false;
                state.ChatFollow = issuerRef;
                state.GotoActive = false;
                state.Last.reset();
                break;
            case ChatVerb::FollowOff:
                state.ChatFollow = 0;
                state.Last.reset();
                break;
            case ChatVerb::Stay:
                state.ChatStay = true;
                state.ChatFollow = 0;
                state.GotoActive = false;
                state.Last.reset();
                break;
            case ChatVerb::StayOff:
                state.ChatStay = false;
                state.Last.reset();
                break;
            case ChatVerb::Stop:
                state.ChatStay = false;
                state.ChatFollow = 0;
                state.GotoActive = false;
                state.ForcedTarget = 0;
                state.ForcedHeal = 0;
                state.TankLead = false;
                state.Events.reset();
                state.Last.reset();
                break;
            case ChatVerb::Lead:
            {
                // Only in a dungeon the module has a route for, and only for a bot that is in the speaker's group (the loop above) and so can lead it.
                auto const dungeon = _options.DungeonsByMap.find(bot.map_id);
                if (dungeon == _options.DungeonsByMap.end() || dungeon->second.Route.empty())
                {
                    refuse("not in a dungeon with a known route");
                    break;
                }
                state.TankLead = true;
                state.ChatFollow = 0;
                state.ChatStay = false;
                state.GotoActive = false;
                state.Events.reset();
                _tankKilled[bot.map_id].clear();
                // On from the point nearest to where the tank stands, not from the entrance: it can be told in the middle of a dungeon.
                std::size_t nearest = 0;
                float nearestDistance = 1e30f;
                for (std::size_t i = 0; i < dungeon->second.Route.size(); ++i)
                {
                    float const distance = DistanceTo(bot, dungeon->second.Route[i]);
                    if (distance < nearestDistance)
                    {
                        nearestDistance = distance;
                        nearest = i;
                    }
                }
                state.RouteIndex = nearest;
                state.RouteNoRoute = 0;
                state.RouteBossSinceMs = 0;
                state.Last.reset();
                lines.push_back("lead: " + std::to_string(bot.guid) + " leads from point " + std::to_string(nearest + 1) + " of " + std::to_string(dungeon->second.Route.size()) + ".");
                break;
            }
            case ChatVerb::LeadOff:
                state.TankLead = false;
                state.Events.reset();
                state.Last.reset();
                break;
            case ChatVerb::GotoHere:
            {
                if (bot.map_id != chat.MapId)
                {
                    refuse("on another map");
                    break;
                }
                float dx = 0.0f, dy = 0.0f;
                ChatSpreadOffset(index, dx, dy);
                state.GotoPoint = Point{ chat.Pos.X + dx, chat.Pos.Y + dy, chat.Pos.Z };
                state.GotoActive = true;
                state.GotoUntilMs = nowMs + kGotoTimeoutMs;
                state.ChatFollow = 0;
                state.ChatStay = true; // holds the spot once there
                state.TrainAfterGoto = false;
                state.Last.reset();
                break;
            }
            case ChatVerb::Train:
            {
                // The speaker stands at a class trainer: the bot walks up to the speaker's own spot (no spread, a trainer is only reachable within a few
                // yards), then learns what that trainer teaches it (the goto ending sends the train action).
                if (bot.life_state != 0)
                {
                    refuse("dead");
                    break;
                }
                if (bot.map_id != chat.MapId)
                {
                    refuse("on another map");
                    break;
                }
                state.GotoPoint = Point{ chat.Pos.X, chat.Pos.Y, chat.Pos.Z };
                state.GotoActive = true;
                state.GotoUntilMs = nowMs + kGotoTimeoutMs;
                state.ChatFollow = 0;
                state.ChatStay = true; // holds the spot once there
                state.TrainAfterGoto = true;
                state.Last.reset();
                break;
            }
            case ChatVerb::Release:
                if (bot.life_state != 1)
                    refuse("not dead (or already released)");
                else
                    state.ReleaseNow = true;
                break;
            case ChatVerb::Status:
            {
                std::string line = "Bot " + std::to_string(bot.guid) + " (class " + std::to_string(classId) + "): ";
                line += state.SeenLife == 1 ? "dead" : state.SeenLife == 2 ? "ghost" : state.SeenInCombat ? "fighting" : "idle";
                line += ", hp " + std::to_string(state.SeenHealthPct) + "%";
                if (bot.max_power > 0)
                    line += ", power " + std::to_string(state.SeenPowerPct) + "%";
                if (state.ChatStay) line += ", stay";
                if (state.ChatFollow != 0) line += ", following you";
                if (state.Stance_ == BotState::Stance::Passive) line += ", passive";
                if (state.Stance_ == BotState::Stance::Defensive) line += ", defensive";
                if (state.Role_ != BotState::Role::Auto)
                    line += state.Role_ == BotState::Role::Tank ? ", role tank" : state.Role_ == BotState::Role::Healer ? ", role healer" : ", role dps";
                if (state.Focus != 0) line += ", focus set";
                if (state.FollowYards > 0.0f) line += ", follow " + std::to_string(int(state.FollowYards)) + "y";
                if (state.GotoActive) line += ", walking to you";
                lines.push_back(std::move(line));
                break;
            }
            case ChatVerb::Pull:
                if (classId != kWarriorClass && state.Role_ != BotState::Role::Tank)
                    refuse("not a tank");
                else if (chat.TargetRef == 0 || forever_module::UnitRefType(chat.TargetRef) != 8)
                    refuse("select a creature first");
                else
                {
                    state.ForcedTarget = chat.TargetRef;
                    state.ForcedUntilMs = nowMs + kPullOrderMs;
                    state.ChatStay = false;
                    state.AvoidUntilMs.erase(chat.TargetRef);
                }
                break;
            case ChatVerb::Heal:
                if (HealSpellFor(classId, bot.level) == 0)
                    refuse("not a healer");
                else if (chat.TargetRef == 0 || forever_module::UnitRefType(chat.TargetRef) != kPlayerType)
                    refuse("select a friend first");
                else
                    state.ForcedHeal = chat.TargetRef;
                break;
            case ChatVerb::Passive:
                state.Stance_ = BotState::Stance::Passive;
                break;
            case ChatVerb::Aggressive:
                state.Stance_ = BotState::Stance::Aggressive;
                break;
            case ChatVerb::Stance:
                state.Stance_ = command.Arg == "passive" ? BotState::Stance::Passive
                                : command.Arg == "defensive" ? BotState::Stance::Defensive : BotState::Stance::Aggressive;
                break;
            case ChatVerb::Role:
                state.Role_ = command.Arg == "tank" ? BotState::Role::Tank : command.Arg == "healer" ? BotState::Role::Healer
                              : command.Arg == "dps" ? BotState::Role::Dps : BotState::Role::Auto;
                break;
            case ChatVerb::Focus:
                if (chat.TargetRef == 0 || forever_module::UnitRefType(chat.TargetRef) != 8)
                    refuse("select a creature first");
                else
                    state.Focus = chat.TargetRef;
                break;
            case ChatVerb::FocusOff:
                state.Focus = 0;
                break;
            case ChatVerb::Distance:
                state.FollowYards = command.Arg == "default" ? 0.0f : command.Value;
                state.FollowWalking = false;
                break;
            case ChatVerb::Summon:
                if (bot.life_state != 0)
                    refuse("dead");
                else
                {
                    forever_module::BotAction summon;
                    summon.bot_guid = bot.guid;
                    summon.type = ActionType::kSummon;
                    summon.target_guid = issuerRef;
                    summon.dest_x = float(index);
                    state.OrderAction = summon;
                    state.GotoActive = false;
                    state.Last.reset();
                }
                break;
            case ChatVerb::Revive:
                if (bot.life_state == 0)
                    refuse("not dead");
                else
                {
                    forever_module::BotAction revive;
                    revive.bot_guid = bot.guid;
                    revive.type = ActionType::kRevive;
                    revive.target_guid = issuerRef;
                    revive.dest_x = float(index);
                    state.OrderAction = revive;
                    state.ReleaseNow = false;
                }
                break;
            case ChatVerb::Mount:
            case ChatVerb::Dismount:
                if (bot.life_state != 0)
                    refuse("dead");
                else
                {
                    forever_module::BotAction mount;
                    mount.bot_guid = bot.guid;
                    mount.type = ActionType::kMount;
                    mount.spell_id = command.Verb == ChatVerb::Mount ? 1u : 0u;
                    state.OrderAction = mount;
                }
                break;
            case ChatVerb::None:
            case ChatVerb::Help: // handled above, before this loop
                break;
        }
        if (refused == refusedBefore)
            accepted.push_back(bot.guid);
        ++index;
    }
    if (command.Verb == ChatVerb::Lead && !accepted.empty())
    {
        // The rest of the group follows the tank, not the speaker: orders to follow or hold the speaker are dropped.
        for (BotSnapshot const& other : view.bots)
        {
            if (other.group_leader_guid != issuerRef || std::find(accepted.begin(), accepted.end(), other.guid) != accepted.end())
                continue;
            BotState& follower = StateOf(other.guid);
            follower.ChatFollow = 0;
            follower.ChatStay = false;
            follower.GotoActive = false;
            follower.Last.reset();
        }
    }
    if (acks && chat.Say)
    {
        for (std::uint64_t const guid : PickSpeakers(accepted, command.Verb, chat.Recipient != 0, chat.SayMax))
            if (std::string text = AckLine(command.Verb, guid); !text.empty())
                acks->push_back({ guid, std::move(text) });
    }
    if (matched == 0)
        lines.push_back("No bot of your group matched.");
    else if (refused != 0)
        lines.push_back(std::string(ChatVerbName(command.Verb)) + ": " + std::to_string(refused) + " of " + std::to_string(matched) + " bot(s) refused (" + refusal + ").");
    return lines;
}
