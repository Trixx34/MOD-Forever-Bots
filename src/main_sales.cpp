// SPDX-License-Identifier: GPL-2.0-or-later
// main_detail helpers: loot candidates and vendor sales (declared in main_internal.h).
#include "main_internal.h"

namespace main_detail {

auction::Candidate CandidateOf(forever_module::PlayerItemLootedEvent const& event)
{
    auction::Candidate item;
    item.ItemGuid = event.item_guid();
    item.Entry = event.item_entry();
    item.Count = event.count() ? event.count() : 1;
    item.Quality = static_cast<std::uint8_t>(event.quality());
    item.VendorSellCopper = event.sell_price();
    item.QuestItem = event.item_class() == kItemClassQuest;
    item.Needed = event.item_class() == kItemClassWeapon || event.item_class() == kItemClassArmor;
    item.Stackable = item.Count != 1;   // the core lists single items only
    item.Goods = event.item_class() == kItemClassTradeGoods;
    return item;
}

sell::Item SellItemOf(forever_module::PlayerItemLootedEvent const& event)
{
    sell::Item item;
    item.Entry = event.item_entry();
    item.Count = event.count() ? event.count() : 1;
    item.Quality = static_cast<std::uint8_t>(event.quality());
    item.VendorSellCopper = event.sell_price();
    item.QuestItem = event.item_class() == kItemClassQuest;
    item.Equipment = event.item_class() == kItemClassWeapon || event.item_class() == kItemClassArmor;
    return item;
}

void AppendSales(SellBook& book, BotSettings const& settings, forever_module::WorldTickSnapshotView const& view,
                 std::vector<forever_module::BotAction>& actions, std::uint32_t maxActions, std::uint64_t now, std::shared_ptr<AltRegistry> const& alts)
{
    std::unordered_set<std::uint64_t> busy;
    for (forever_module::BotAction const& action : actions)
        busy.insert(action.bot_guid);
    for (forever_module::BotSnapshot const& bot : view.bots)
    {
        if (actions.size() >= maxActions)
            return;
        if (busy.count(bot.guid) || bot.group_role != 0 || (alts && alts->Has(bot.guid)) || bot.life_state != 0 || bot.in_combat != 0 ||
            bot.vendor_valid == 0 || bot.vendor_guid == 0)
            continue;

        // The sale sent last time: the core's reply to it is the bot's last action. Until the reply comes, the bot waits.
        auto const flight = book.Flight.find(bot.guid);
        if (flight != book.Flight.end())
        {
            bool const lastWasSale = bot.last_action_type == static_cast<std::uint8_t>(forever_module::ActionType::kSellToVendor) &&
                                     bot.last_action_spell == flight->second.Sent.Entry;
            sell::Outcome const outcome = sell::Resolve(flight->second, bot.last_action_sequence, lastWasSale,
                                                        bot.last_action_status == forever_module::BotActionStatus::kApplied, now);
            if (outcome == sell::Outcome::kWaiting)
                continue;
            if (outcome == sell::Outcome::kSold)
            {
                auto& items = book.Items[bot.guid];
                for (auto it = items.begin(); it != items.end();)
                    it = it->second.Entry == flight->second.Sent.Entry ? items.erase(it) : std::next(it);
            }
            book.Flight.erase(flight);
        }

        std::vector<sell::Sale>& pending = book.Pending[bot.guid];
        if (!pending.empty() && sell::VisitExpired(book.LastVisitMs[bot.guid], now))
            pending.clear();                    // the bot never got there: the items stay for the next visit
        if (pending.empty())
        {
            std::vector<sell::Item> items;
            for (auto const& [itemGuid, item] : book.Items[bot.guid])
                items.push_back(item);
            if (!sell::VisitDue(settings.Sell, !items.empty(), book.LastVisitMs[bot.guid], now))
                continue;
            book.LastVisitMs[bot.guid] = now;
            pending = sell::PlanSales(settings.Sell, items);
            if (pending.empty())
                continue;
        }

        float const dx = bot.pos_x - bot.vendor_x;
        float const dy = bot.pos_y - bot.vendor_y;
        if (dx * dx + dy * dy > kSellReachYards * kSellReachYards)
        {
            // The core found no route to the vendor (it may be far, or across water): the visit is over, the items stay for the next one. The walk used to be sent on
            // every tick without the policy's pause: 140 bots refused 5,000 times a minute.
            if (bot.last_action_type == static_cast<std::uint8_t>(forever_module::ActionType::kMoveTo) &&
                bot.last_action_status == forever_module::BotActionStatus::kNoRoute && bot.last_action_sequence != book.WalkRefusedSeq[bot.guid])
            {
                book.WalkRefusedSeq[bot.guid] = bot.last_action_sequence;
                pending.clear();
                continue;
            }
            std::uint64_t& next = book.WalkNextMs[bot.guid];
            if (now < next)
                continue;
            next = now + kSellWalkRepeatMs;
            forever_module::BotAction walk;     // the vendor's position is on the bot's map, so the walk stays on it
            walk.bot_guid = bot.guid;
            walk.type = forever_module::ActionType::kMoveTo;
            walk.dest_x = bot.vendor_x;
            walk.dest_y = bot.vendor_y;
            actions.push_back(walk);
            continue;
        }

        sell::Sale const sale = pending.front();
        pending.erase(pending.begin());

        forever_module::BotAction action;
        action.bot_guid = bot.guid;
        action.type = forever_module::ActionType::kSellToVendor;
        action.spell_id = sale.Entry;
        action.dest_x = 0.0f;                   // 0 = every one the bot carries of it
        action.target_guid = bot.vendor_guid;
        actions.push_back(action);
        book.Flight[bot.guid] = sell::InFlight{ sale, bot.last_action_sequence, now };
    }
}

} // namespace main_detail
