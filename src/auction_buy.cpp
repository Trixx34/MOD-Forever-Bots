// SPDX-License-Identifier: GPL-2.0-or-later
// The buying side of the auction rules (auction_policy.h): the budget, the choice among offers and when a buy visit is due.
#include "auction_policy.h"

#include <optional>

namespace auction {

std::uint64_t BuyBudget(Settings const& settings, std::uint64_t copper, std::uint32_t level)
{
    std::uint64_t const reserve = std::uint64_t(level) * settings.ReserveCopperPerLevel;
    std::uint64_t const spendable = copper > reserve ? copper - reserve : 0;
    return spendable * settings.BuyMaxPct / 100;
}

std::optional<Offer> ChooseBuy(Settings const& settings, std::uint64_t budget, std::vector<Offer> const& offers, UpgradeScore const& score)
{
    if (!settings.Enabled || budget == 0 || !score)
        return std::nullopt;
    std::optional<Offer> best;
    double bestValue = 0.0;
    for (Offer const& offer : offers)
    {
        if (offer.OwnListing || offer.Buyout == 0 || offer.Buyout > budget)
            continue;
        double const gain = score(offer);
        if (gain <= 0.0)
            continue;
        double const value = gain / double(offer.Buyout);
        if (!best || value > bestValue || (value == bestValue && offer.Buyout < best->Buyout))
        {
            best = offer;
            bestValue = value;
        }
    }
    return best;
}

double UpgradeOf(Offer const& offer)
{
    return double(offer.Upgrade);
}

bool BuyVisitDue(Settings const& settings, Standing const& standing, std::uint64_t nowMs)
{
    if (!settings.Enabled || !settings.BuyEnabled)
        return false;
    return standing.LastBuyMs == 0 || nowMs - standing.LastBuyMs >= settings.EveryMs;
}

} // namespace auction
