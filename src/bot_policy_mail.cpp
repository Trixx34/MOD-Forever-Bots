// SPDX-License-Identifier: GPL-2.0-or-later
// BotMail (config key BotMail, off by default): the pool-level turn that plans mails between guildmates (bot_mail.h) and queues them as social orders.
// Gold and materials only; gear needs item facts the core does not send yet (doc/bot-trade-mail.md).
#include "bot_policy_internal.h"

void BotPolicy::DriveMail(forever_module::WorldTickSnapshotView const& view, std::uint64_t nowMs)
{
    botmail::Settings const& settings = Opt().BotMail;
    if (!settings.Enabled)
        return;

    std::vector<botmail::Member> members;
    members.reserve(view.bots.size());
    for (BotSnapshot const& bot : view.bots)
    {
        botmail::Member member;
        member.Guid = bot.guid;
        member.Guild = bot.guild_guid;
        member.Side = Opt().SideOf ? Opt().SideOf(bot.guid) : 0;
        member.ClassId = Opt().ClassOf ? Opt().ClassOf(bot.guid) : 0;
        member.Level = bot.level;
        member.Copper = bot.copper;
        member.Skill = { bot.profession_skill[0], bot.profession_skill[1] };
        auto const known = _bots.find(bot.guid);
        bool const busy = known != _bots.end() && known->second.SocialBusyUntilMs > nowMs;
        member.Free = !busy && bot.life_state == 0 && bot.in_flight == 0 && bot.in_combat == 0 && bot.group_role == 0;
        if (Opt().IsAlt && Opt().IsAlt(bot.guid))
            member.Free = false;   // a player's own character: never moved by the module
        members.push_back(member);
    }

    for (botmail::Offer const& offer : botmail::Plan(settings, _mailState, members, nowMs))
    {
        social::Order order;
        order.What = social::Kind::Mail;
        order.Bot = offer.Sender;
        order.Player = offer.Recipient;
        order.Op = offer.What == botmail::Kind::Gold ? 0 : 1;
        order.Copper = offer.Copper;
        order.Skill[0] = offer.RecipientSkill[0];
        order.Skill[1] = offer.RecipientSkill[1];
        order.Skill[2] = offer.SenderSkill[0];
        order.Skill[3] = offer.SenderSkill[1];
        order.MinCount = settings.MaterialMinCount;
        order.MaxCount = settings.MaterialMaxCount;
        if (auto const known = _bots.find(order.Bot); known != _bots.end())
            known->second.SocialBusyUntilMs = nowMs + 30000;
        _mailTo[order.Bot] = order.Player;
        if (Opt().OnEvent)
            Opt().OnEvent("bot_mail: sender=" + std::to_string(order.Bot) + " recipient=" + std::to_string(order.Player) + " kind=" +
                             (order.Op == 0 ? "gold copper=" + std::to_string(order.Copper) : std::string("materials")));
        _socialOrders.push_back(std::move(order));
    }
}
