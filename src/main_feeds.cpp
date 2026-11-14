// SPDX-License-Identifier: GPL-2.0-or-later
// main_detail: presence, the loot books, the Auction House and the town errands (the Run() thread's timers and the pump's feeds) (declared in main_internal.h, called from main()).
#include "main_internal.h"

namespace main_detail {

namespace {

// A bot's looted items as a list, after the bot's level is stamped on the ones that have no loot level yet (auction::StampLootLevel; the loot event
// does not carry the level, the feed does).
std::vector<auction::Candidate> StampedItems(std::unordered_map<std::uint64_t, auction::Candidate>& looted, std::uint32_t level)
{
    std::vector<auction::Candidate*> pointers;
    pointers.reserve(looted.size());
    for (auto& [guid, candidate] : looted)
        pointers.push_back(&candidate);
    auction::StampLootLevel(pointers, level);
    std::vector<auction::Candidate> items;
    items.reserve(looted.size());
    for (auto const& [guid, candidate] : looted)
        items.push_back(candidate);
    return items;
}

} // namespace

// Presence (bot_presence.h): the pump's feed, and a timer that logs bots off and back on.
void SetUpPresence(forever_module::ModuleClient& client, RunContext& run, BotSettings& settings, PumpExtras& extras, std::shared_ptr<trade::TradeBook> const& trades)
{
    auto feed = std::make_shared<PresenceFeed>();
    extras.Presence = feed;
    run.Levels = feed;
    auto scheduler = std::make_shared<presence::Scheduler>(settings.Presence, settings.RandomSeed ^ 0x9e3779b97f4a7c15ull);
    client.AddTimer(kPresenceInterval, [&run, feed, scheduler, trades]
    {
        std::vector<presence::Online> list;
        std::uint64_t fedAt = 0;
        {
            std::lock_guard<std::mutex> lock(feed->Lock);
            list = feed->Bots;
            fedAt = feed->AtMs;
        }
        std::uint64_t const now = nowMs();
        if (fedAt == 0 || now - fedAt > kPresenceStaleMs)
            return;
        for (presence::Online& bot : list)
            if (trades->Active(bot.Guid))
                bot.Idle = false; // a trade in progress is not cut short
        scheduler->SetSettings(run.Settings.Presence);
        presence::Decisions const decisions = scheduler->Tick(now, LocalSecondsOfDay(), list);
        for (std::uint64_t guid : decisions.LogOff)
        {
            run.Roster.NoteOffline(guid);
            run.Client.ReleaseControl(guid); // the core logs it out (Bot.Presence.LogoutOnRelease)
        }
        for (std::uint64_t guid : decisions.LogOn)
        {
            run.Roster.NoteRequestSent(guid);
            run.Client.RequestControl(guid, {}, [&run, scheduler, guid](forever_module::ControlResponse const& reply)
            {
                if (reply.granted())
                {
                    run.Roster.OnGranted(guid, reply.bot_guid());
                    return;
                }
                run.Roster.OnDenied(guid);
                if (reply.reason() == "character not found")
                {
                    scheduler->Forget(guid);
                    run.Roster.ForgetOffline(guid);
                    run.Client.StoreDelete(bot_store::KeyFor(guid));
                }
                else
                    scheduler->LogOnFailed(guid, nowMs());
                std::cerr << "forever-bots: presence: could not bring bot " << guid << " back: " << reply.reason() << "\n";
            });
        }
        if (!decisions.LogOff.empty() || !decisions.LogOn.empty())
            std::cerr << "forever-bots: presence: " << decisions.LogOff.size() << " log off, " << decisions.LogOn.size() << " back; "
                      << run.Roster.OfflineCount() << " away, " << run.Roster.Owned().size() << " online\n";
    });
    std::cerr << "forever-bots: presence is on: sessions " << settings.Presence.OnlineMinMin << " to " << settings.Presence.OnlineMaxMin
              << " min, away " << settings.Presence.OfflineMinMin << " to " << settings.Presence.OfflineMaxMin << " min\n";
}

// The loot handler that fills the auction and the sale books, and the vendor sales for the pump.
void SetUpLootBooks(forever_module::ModuleClient& client, RunContext& run, BotSettings& settings, std::shared_ptr<AuctionBook> const& book,
                    std::shared_ptr<SellBook> const& sells, PumpExtras& extras)
{
    client.OnNotify("PlayerScript.OnLootItem", [&run, &settings, book, sells](forever_module::EventNotify const& notify)
    {
        if (notify.event_payload_case() != forever_module::EventNotify::kPlayerItemLooted)
            return;
        forever_module::PlayerItemLootedEvent const& event = notify.player_item_looted();
        if (!run.Roster.Owned().count(event.player_guid()))
            return;
        if (settings.Sell.Enabled)
        {
            sell::Item const sold = SellItemOf(event);
            if (sell::Sellable(sold))
            {
                auto& items = sells->Items[event.player_guid()];
                if (items.size() >= kSellItemsPerBot)
                    items.erase(items.begin());
                items[event.item_guid()] = sold;
            }
        }
        if (!settings.Auction.Enabled)
            return;
        auction::Candidate const item = CandidateOf(event);
        // Goods (AuctionGoodsSell) are listed from white quality up; the rest from uncommon, as before.
        bool const goods = settings.Auction.GoodsSell && item.Goods;
        if (!goods && event.quality() < auction::kUncommon)
            return;
        if (item.Needed || item.QuestItem || item.ItemGuid == 0)
            return;
        auto& items = book->Items[event.player_guid()];
        if (items.size() >= kAuctionItemsPerBot)
            items.erase(items.begin());
        items[item.ItemGuid] = item;
    });
    if (settings.Sell.Enabled)
    {
        extras.Sell = sells;
        std::cerr << "forever-bots: vendor sales are on: a sale visit every " << settings.Sell.EveryMs / 1000 << " s per bot, at most "
                  << settings.Sell.MaxSalesPerVisit << " items per visit, while the bot stands at a vendor\n";
    }
}

// The Auction House: the pump's feed, and a timer that lists the looted items and, with AuctionBuy, buys upgrades.
void SetUpAuction(forever_module::ModuleClient& client, RunContext& run, BotSettings& settings, std::shared_ptr<AuctionBook> const& book, PumpExtras& extras)
{
    auto feed = std::make_shared<AuctionFeed>();
    extras.Auction = feed;
    client.AddTimer(kAuctionInterval, [&run, &client, &settings, feed, book]
    {
        std::vector<AuctionBot> list;
        std::uint64_t fedAt = 0;
        {
            std::lock_guard<std::mutex> lock(feed->Lock);
            list = feed->Bots;
            fedAt = feed->AtMs;
        }
        std::uint64_t const now = nowMs();
        if (fedAt == 0 || now - fedAt > kPresenceStaleMs)
            return;
        auction::CallBudget budget(settings.Auction.MaxCallsPerTick);
        budget.NewTick();
        // AuctionBuy (off by default): when the bot's buy visit is due and it has money to spend, browse the next equipment slot for upgrades
        // (the core scores the listings for this bot) and buy out the best upgrade per copper that the budget allows. One browse per visit; the
        // buyout follows from the browse's reply, so it is not counted again in this tick's budget.
        auto const buyVisit = [&client, &settings, &budget, now](AuctionBot const& bot, auction::Standing& standing)
        {
            // The core browses and buys only for a bot in reach of the auctioneer: a browse from farther spent the pace and the tick's budget for nothing.
            if (!bot.InReach || !settings.Auction.BuyEnabled || !auction::BuyVisitDue(settings.Auction, standing, now))
                return;
            std::uint64_t const spend = auction::BuyBudget(settings.Auction, bot.Copper, bot.Level);
            if (spend == 0 || !budget.Take())
                return;
            standing.LastBuyMs = now;
            forever_module::ModuleClient::AuctionBrowseFilter filter;
            filter.inventoryType = auction::BrowseSlot(standing.BuyCursor++);
            filter.maxLevel = bot.Level;
            filter.maxBuyout = auction::BuyoutBound(spend);
            filter.onlyUpgrades = true;
            std::uint64_t const botGuid = bot.Guid;
            std::uint64_t const auctioneer = bot.AuctioneerGuid;
            client.AuctionBrowseByFilterForBot(botGuid, auctioneer, filter,
                                               [&client, &settings, botGuid, auctioneer, spend](forever_module::ActionReply const& reply)
            {
                if (!reply.success())
                    return;
                std::vector<auction::Offer> offers;
                for (forever_module::AuctionListingEvent const& listing : reply.listings())
                {
                    auction::Offer offer;
                    offer.AuctionGuid = listing.auction_guid();
                    offer.Entry = listing.item_entry();
                    offer.Count = listing.count() ? listing.count() : 1;
                    offer.Bid = listing.bid();
                    offer.Buyout = listing.buyout();
                    offer.Upgrade = listing.upgrade_score();
                    offers.push_back(offer);
                }
                std::optional<auction::Offer> const pick = auction::ChooseBuy(settings.Auction, spend, offers, auction::UpgradeOf);
                if (!pick)
                    return;
                auction::Offer const offer = *pick;
                client.AuctionHouseForBot(botGuid, forever_module::ModuleClient::AuctionHouseOp::kBuyout, auctioneer, 0, offer.AuctionGuid, offer.Entry, 0, 0,
                                          static_cast<std::uint32_t>(offer.Buyout), [botGuid, offer](forever_module::ActionReply const& bought)
                {
                    if (bought.success())
                        std::cerr << "forever-bots: " << auction::BoughtLine(botGuid, offer, bought.copper_spent() ? bought.copper_spent() : offer.Buyout) << "\n";
                    else
                        std::cerr << "forever-bots: " << auction::BuyFailedLine(botGuid, offer, bought.reason()) << "\n";
                });
            });
        };
        for (AuctionBot const& bot : list)
        {
            if (!bot.Idle || bot.AuctioneerGuid == 0)
                continue;
            auction::Standing& standing = book->Standing[bot.Guid];
            std::vector<std::uint64_t>& ends = book->Ends[bot.Guid];
            auction::PruneExpired(ends, now);
            standing.ListedNow = static_cast<std::uint32_t>(ends.size());
            auto& goodsEnds = book->GoodsEnds[bot.Guid];
            goodsEnds.erase(std::remove_if(goodsEnds.begin(), goodsEnds.end(), [now](auto const& g) { return g.first <= now; }), goodsEnds.end());
            standing.GoodsListed.clear();
            for (auto const& [end, entry] : goodsEnds)
                ++standing.GoodsListed[entry];
            auction::Bags const bags{ kBagSlotsAssumed, kBagSlotsAssumed - std::min(bot.FreeSlots, kBagSlotsAssumed) };
            // What may be listed now: equipment waits out its hold (auction::GearReady).
            std::vector<auction::Candidate> const items = auction::ReadyItems(settings.Auction, StampedItems(book->Items[bot.Guid], bot.Level), bot.Level);
            bool const hasListable = auction::HasListable(settings.Auction, items, bot.Level);
            if (!auction::ListingVisitDue(settings.Auction, bags, standing, bot.InReach, hasListable, now))
            {
                buyVisit(bot, standing);
                continue;
            }
            standing.LastVisitMs = now;

            for (auction::Listing const& listing : auction::PlanListings(settings.Auction, bags, standing, items))
            {
                if (!budget.Take())
                    break;
                std::uint64_t const botGuid = bot.Guid;
                client.AuctionHouseForBot(botGuid, forever_module::ModuleClient::AuctionHouseOp::kList, bot.AuctioneerGuid, listing.ItemGuid, 0, listing.Entry, 1,
                                          listing.Bid, listing.Buyout, [book, botGuid, listing](forever_module::ActionReply const& reply)
                {
                    auto& items = book->Items[botGuid];
                    if (reply.success())
                    {
                        book->Ends[botGuid].push_back(nowMs() + std::uint64_t(listing.RunMinutes) * 60000u);
                        book->Standing[botGuid].Attempts[listing.ItemGuid] = listing.Attempt;
                        items.erase(listing.ItemGuid);
                        std::cerr << "forever-bots: " << auction::ListedLine(botGuid, listing, 1) << "\n";
                        return;
                    }
                    std::cerr << "forever-bots: " << auction::ListRefusedLine(botGuid, listing, reply.reason()) << "\n";
                    // A refusal for the place or the money is tried again on a later visit; a refusal about the item drops it.
                    if (auction::ClassifyListFailure(reply.reason()) == auction::ListFailure::Skip)
                        items.erase(listing.ItemGuid);
                });
            }
            if (settings.Auction.GoodsSell)
            {
                for (auction::GoodsListing const& goods : auction::PlanGoods(settings.Auction, standing, items))
                {
                    if (!budget.Take())
                        break;
                    std::uint64_t const botGuid = bot.Guid;
                    client.AuctionHouseForBot(botGuid, forever_module::ModuleClient::AuctionHouseOp::kListCommodity, bot.AuctioneerGuid, goods.ItemGuid, 0,
                                              goods.Entry, goods.Count, goods.UnitPrice, 0, [book, botGuid, goods](forever_module::ActionReply const& reply)
                    {
                        auto& items = book->Items[botGuid];
                        if (reply.success())
                        {
                            book->GoodsEnds[botGuid].emplace_back(nowMs() + 7200000u, goods.Entry); // the core runs every auction 2 hours
                            std::cerr << "forever-bots: " << auction::GoodsListedLine(botGuid, goods) << "\n";
                            items.erase(goods.ItemGuid);
                            return;
                        }
                        auction::Listing const refused{ goods.ItemGuid, goods.Entry, goods.UnitPrice, 0, 120, 1 };
                        std::cerr << "forever-bots: " << auction::ListRefusedLine(botGuid, refused, reply.reason()) << "\n";
                        if (auction::ClassifyListFailure(reply.reason()) == auction::ListFailure::Skip)
                            items.erase(goods.ItemGuid);
                    });
                }
            }
            buyVisit(bot, standing);
        }
    });
    std::cerr << "forever-bots: auction house is on: a listing visit every " << settings.Auction.EveryMs / 1000 << " s per bot, at most "
              << settings.Auction.PerVisit << " items per visit\n";
    if (settings.Auction.BuyEnabled)
        std::cerr << "forever-bots: auction buying is on: an upgrade browse (one equipment slot) every " << settings.Auction.EveryMs / 1000
                  << " s per bot, a purchase costs at most " << settings.Auction.BuyMaxPct << " percent of the money above the reserve\n";
}

// Town errands: the trainer table, the pump's feed, and a timer that starts and ends the errands.
void SetUpErrands(forever_module::ModuleClient& client, BotSettings& settings, std::shared_ptr<CommandQueue> const& commands, std::shared_ptr<AuctionBook> const& book,
                  PumpExtras& extras)
{
    // The trainer table (ErrandTrainerFile): without it no errand can be made, so the errands stay off.
    auto trainers = std::make_shared<std::vector<errand::Trainer>>();
    std::ifstream in(settings.ErrandTrainerFile);
    if (!in)
        std::cerr << "forever-bots: ErrandTrainerFile: cannot read '" << settings.ErrandTrainerFile << "'; errands are off\n";
    else
    {
        std::ostringstream text;
        text << in.rdbuf();
        std::vector<std::string> trainerWarnings;
        *trainers = errand::ParseTrainerTable(text.str(), trainerWarnings);
        for (std::string const& warning : trainerWarnings)
            std::cerr << "forever-bots: " << settings.ErrandTrainerFile << ": " << warning << "\n";
        std::cerr << "forever-bots: errands: " << trainers->size() << " trainers from " << settings.ErrandTrainerFile << "\n";
    }
    // Mounts (MountBuy): the riding trainers join the trainer table (skill 762), the mount vendors and the riding ranks have their own tables. Any of the three
    // missing or empty and the mount trips stay off.
    auto ranks = std::make_shared<std::vector<riding::Rank>>();
    auto mountTable = std::make_shared<mounts::Table>();
    bool mountsOn = false;
    if (settings.MountBuy)
    {
        auto const slurp = [](std::string const& path, std::string& out)
        {
            std::ifstream file(path);
            if (!file)
                return false;
            std::ostringstream text;
            text << file.rdbuf();
            out = text.str();
            return true;
        };
        std::string ridingTrainers, rankText, vendorText;
        if (!slurp(settings.RidingTrainerFile, ridingTrainers) || !slurp(settings.MountVendorFile, vendorText) || (settings.RidingTraining && !slurp(settings.RidingRankFile, rankText)))
            std::cerr << "forever-bots: MountBuy: cannot read RidingTrainerFile, MountVendorFile or RidingRankFile; mount trips are off\n";
        else
        {
            std::vector<std::string> mountWarnings;
            std::vector<errand::Trainer> riders = errand::ParseTrainerTable(ridingTrainers, mountWarnings);
            *ranks = riding::ParseRanks(rankText, mountWarnings);
            *mountTable = mounts::ParseTable(vendorText, mountWarnings);
            for (std::string const& warning : mountWarnings)
                std::cerr << "forever-bots: mounts: " << warning << "\n";
            for (errand::Trainer& rider : riders)
                trainers->push_back(std::move(rider));
            mountsOn = !mountTable->Items.empty() && (!settings.RidingTraining || !ranks->empty());
            std::cerr << "forever-bots: mounts: " << riders.size() << " riding trainers, " << ranks->size() << " ranks, " << mountTable->Vendors.size() << " vendors, "
                      << mountTable->Items.size() << " mounts" << (mountsOn ? "" : "; a table is empty, mount trips are off") << "\n";
        }
    }
    if (!trainers->empty())
    {
        auto feed = std::make_shared<ErrandFeed>();
        extras.Errand = feed;
        auto scheduler = std::make_shared<errand::Scheduler>(settings.Errand);
        auto journal = std::make_shared<errand::Journal>(errand::CityName); // the event lines (errand_policy.h)
        std::shared_ptr<CommandQueue> queue = commands;
        auto mountBook = std::make_shared<MountBook>();
        client.AddTimer(kErrandInterval, [&client, feed, scheduler, journal, trainers, queue, book, ranks, mountTable, mountsOn, mountBook, &settings]
        {
            std::uint64_t const now = nowMs();
            std::vector<ErrandBot> list;
            std::vector<std::pair<std::uint64_t, ErrandOutcome>> outcomes;
            std::vector<ErrandFeed::Buy> buys;
            std::uint64_t fedAt = 0;
            {
                std::lock_guard<std::mutex> lock(feed->Lock);
                list = feed->Bots;
                outcomes.swap(feed->Outcomes);
                buys.swap(feed->Buys);
                fedAt = feed->AtMs;
            }
            // Mount purchases: the Tier 1 call from here; the answer goes back to the pump, which ends the vendor errand with it.
            for (ErrandFeed::Buy const& buy : buys)
            {
                std::uint64_t const botGuid = buy.Bot;
                std::uint32_t const item = buy.Request.Item;
                std::cerr << "forever-bots: event mount_buy: bot=" << botGuid << " item=" << item << "\n";
                client.BuyItemForBot(botGuid, buy.Request.Vendor, item, 1, [feed, botGuid, item](forever_module::ActionReply const& reply)
                {
                    std::cerr << "forever-bots: event mount_buy_" << (reply.success() ? "done" : "refused") << ": bot=" << botGuid << " item=" << item
                              << (reply.success() ? "" : " reason=" + reply.reason()) << "\n";
                    std::lock_guard<std::mutex> lock(feed->Lock);
                    feed->BuyResults.emplace_back(botGuid, reply.success());
                });
            }
            for (auto const& [guid, outcome] : outcomes)
            {
                mountBook->OnOutcome(guid, outcome == ErrandOutcome::kDone, *ranks, now);
                scheduler->Finish(guid, now); // done or failed: the trip is over either way
                if (auto const line = journal->OnEnd(guid, errand::EndReason::kDone, outcome == ErrandOutcome::kFailed, now))
                    std::cerr << "forever-bots: event " << *line << "\n";
            }
            if (fedAt == 0 || now - fedAt > kPresenceStaleMs)
                return;

            std::vector<errand::Bot> bots;
            std::unordered_map<std::uint64_t, ErrandBot const*> byGuid;
            for (ErrandBot const& bot : list)
            {
                byGuid[bot.Guid] = &bot;
                errand::Bot b;
                b.Guid = bot.Guid;
                b.Available = bot.Available;
                b.Called = bot.Called;
                if (bot.TrainDue && bot.Side != 0)
                {
                    // Only the capitals of the bot's faction that have a trainer for its skill: a start always has a trainer to walk to.
                    for (std::uint32_t city = errand::kStormwind; city <= errand::kThunderBluff; ++city)
                    {
                        if (errand::ChooseTrainer(*trainers, bot.Side, bot.Skill, city, bot.Map, bot.X, bot.Y, bot.Z))
                            b.Cities.push_back(city);
                    }
                    b.TrainDue = !b.Cities.empty();
                }
                // Mounts: a bot with no profession to train that the core refused a mount goes to the riding trainer or the mount vendor (mount_plan.h).
                if (mountsOn && !b.TrainDue && !b.AuctionDue && bot.Side != 0 && bot.Level >= 1)
                {
                    mounts::Plan const plan = mountBook->Plan(settings, *ranks, *mountTable, bot, now);
                    if (plan.What == mounts::Step::Train)
                    {
                        for (std::uint32_t city = errand::kStormwind; city <= errand::kThunderBluff; ++city)
                        {
                            auto const pick = errand::ChooseTrainer(*trainers, bot.Side, kRidingSkillLine, city, bot.Map, bot.X, bot.Y, bot.Z);
                            if (pick && (*trainers)[*pick].Map == bot.Map)
                                b.Cities.push_back(city);
                        }
                        b.TrainDue = !b.Cities.empty();
                    }
                    else if (plan.What == mounts::Step::Buy)
                    {
                        b.TrainDue = true;
                        if (bot.Side == errand::kAlliance)
                            b.Cities = { errand::kStormwind, errand::kIronforge, errand::kDarnassus };
                        else
                            b.Cities = { errand::kUndercity, errand::kOrgrimmar, errand::kThunderBluff };
                    }
                    if (b.TrainDue)
                        mountBook->Planned[bot.Guid] = plan;
                    else
                        mountBook->Planned.erase(bot.Guid);
                }
                // Looted items to list (the Auction House, when it is on): the walk goes to the faction's auctioneer on the bot's map, and the
                // visit (the listing timer) lists the items once the bot is there. Off until both the errands and the auction are on.
                // Only items the bot may list now: equipment on its hold (auction::GearReady) is no reason for a trip.
                bool auctionDue = false;
                if (settings.Auction.Enabled && bot.Side != 0 && bot.AuctioneerValid)
                {
                    if (auto const looted = book->Items.find(bot.Guid); looted != book->Items.end() && !looted->second.empty())
                        auctionDue = auction::HasListable(settings.Auction, StampedItems(looted->second, bot.Level), bot.Level);
                }
                if (!b.TrainDue && auctionDue)
                {
                    b.AuctionDue = true;
                    // The faction's capitals, as for training; the city is only the errand count per capital.
                    if (bot.Side == errand::kAlliance)
                        b.Cities = { errand::kStormwind, errand::kIronforge, errand::kDarnassus };
                    else
                        b.Cities = { errand::kUndercity, errand::kOrgrimmar, errand::kThunderBluff };
                }
                bots.push_back(std::move(b));
            }

            errand::Decisions const decisions = scheduler->Tick(now, bots);
            for (errand::End const& end : decisions.Ends) // before the starts: a bot may start again in the same tick
            {
                mountBook->OnEnded(end.Guid, now);
                if (auto const line = journal->OnEnd(end.Guid, end.Why, false, now))
                    std::cerr << "forever-bots: event " << *line << "\n";
            }
            std::vector<CommandQueue::ErrandItem> items;
            for (errand::End const& end : decisions.Ends)
            {
                CommandQueue::ErrandItem item;
                item.Guid = end.Guid;
                item.Cancel = true;
                items.push_back(item);
            }
            for (errand::Start const& start : decisions.Starts)
            {
                auto const found = byGuid.find(start.Guid);
                if (found == byGuid.end())
                    continue;
                ErrandBot const& bot = *found->second;
                if (start.What == errand::Kind::kAuction)
                {
                    CommandQueue::ErrandItem item;
                    item.Guid = start.Guid;
                    item.Order.Map = bot.Map;
                    item.Order.Dest = Point{ bot.AuctioneerX, bot.AuctioneerY, bot.AuctioneerZ };
                    item.Order.Auction = true;
                    items.push_back(item);
                    std::cerr << "forever-bots: errand: bot " << start.Guid << " to the auctioneer (city " << start.City << ")\n";
                    std::cerr << "forever-bots: event " << journal->OnStart(start, now) << "\n";
                    continue;
                }
                if (auto const planned = mountBook->Planned.find(start.Guid); mountsOn && planned != mountBook->Planned.end() && !bot.TrainDue)
                {
                    mounts::Plan const plan = planned->second;
                    mountBook->OnStart(start.Guid, plan, bot, settings, now);
                    if (plan.What == mounts::Step::Buy)
                    {
                        mounts::Vendor const& vendor = mountTable->Vendors[plan.Mount.Vendor];
                        CommandQueue::ErrandItem item;
                        item.Guid = start.Guid;
                        item.Order.Map = vendor.Map;
                        item.Order.Dest = Point{ vendor.X, vendor.Y, vendor.Z };
                        item.Order.Vendor = true;
                        item.Order.Item = mountTable->Items[plan.Mount.Item].Entry;
                        item.Order.Entry = vendor.Entry;
                        items.push_back(item);
                        std::cerr << "forever-bots: errand: bot " << start.Guid << " to the mount vendor " << vendor.Name << " for " << mountTable->Items[plan.Mount.Item].Name << "\n";
                        std::cerr << "forever-bots: event " << journal->OnStart(start, now) << "\n";
                        continue;
                    }
                    auto const rider = errand::ChooseTrainer(*trainers, bot.Side, kRidingSkillLine, start.City, bot.Map, bot.X, bot.Y, bot.Z);
                    if (!rider)
                        continue;
                    errand::Trainer const& trainer = (*trainers)[*rider];
                    CommandQueue::ErrandItem item;
                    item.Guid = start.Guid;
                    item.Order.Map = trainer.Map;
                    item.Order.Dest = Point{ trainer.X, trainer.Y, trainer.Z };
                    item.Order.Skill = trainer.Skill;
                    item.Order.Entry = trainer.Entry;
                    items.push_back(item);
                    std::cerr << "forever-bots: errand: bot " << start.Guid << " to the riding trainer " << trainer.Name << "\n";
                    std::cerr << "forever-bots: event " << journal->OnStart(start, now) << "\n";
                    continue;
                }
                auto const pick = errand::ChooseTrainer(*trainers, bot.Side, bot.Skill, start.City, bot.Map, bot.X, bot.Y, bot.Z);
                if (!pick)
                    continue;
                errand::Trainer const& trainer = (*trainers)[*pick];
                CommandQueue::ErrandItem item;
                item.Guid = start.Guid;
                item.Order.Map = trainer.Map;
                item.Order.Dest = Point{ trainer.X, trainer.Y, trainer.Z };
                item.Order.Skill = trainer.Skill;
                item.Order.Entry = trainer.Entry;
                items.push_back(item);
                std::cerr << "forever-bots: errand: bot " << start.Guid << " to " << trainer.Name << " (" << trainer.Subname << ") in city " << start.City << "\n";
                std::cerr << "forever-bots: event " << journal->OnStart(start, now) << "\n";
            }
            if (!items.empty() && queue)
            {
                std::lock_guard<std::mutex> lock(queue->Lock);
                for (auto& item : items)
                    queue->Errands.push_back(std::move(item));
            }
            for (errand::End const& end : decisions.Ends)
            {
                std::cerr << "forever-bots: errand: bot " << end.Guid << " ended (reason " << int(end.Why) << ")\n";
            }
        });
        std::cerr << "forever-bots: town errands are on: a check every " << kErrandInterval.count() / 1000 << " s\n";
    }
}

} // namespace main_detail
