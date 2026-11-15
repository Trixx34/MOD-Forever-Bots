// SPDX-License-Identifier: GPL-2.0-or-later
#include "bot_mail.h"

#include <algorithm>

namespace botmail {

namespace {

struct Material { std::uint32_t Entry; std::uint16_t A; std::uint16_t B; };

// Entry, then up to two consuming professions (0 = none). Classic gathering goods only.
constexpr Material kMaterials[] = {
    // ore: smelted by miners, used raw by engineers
    { 2770, kMining, kEngineering }, { 2771, kMining, kEngineering }, { 2772, kMining, kEngineering }, { 3858, kMining, kEngineering }, { 10620, kMining, kEngineering },
    // bars: blacksmiths and engineers
    { 2840, kBlacksmithing, kEngineering }, { 2841, kBlacksmithing, kEngineering }, { 3576, kBlacksmithing, kEngineering }, { 3575, kBlacksmithing, kEngineering },
    { 3859, kBlacksmithing, kEngineering }, { 3860, kBlacksmithing, kEngineering },
    // herbs: alchemists (and enchanters for none)
    { 2447, kAlchemy, 0 }, { 765, kAlchemy, 0 }, { 2449, kAlchemy, 0 }, { 785, kAlchemy, 0 }, { 2450, kAlchemy, 0 }, { 2453, kAlchemy, 0 }, { 3820, kAlchemy, 0 },
    { 3356, kAlchemy, 0 }, { 3357, kAlchemy, 0 }, { 3818, kAlchemy, 0 }, { 3821, kAlchemy, 0 }, { 3355, kAlchemy, 0 }, { 3369, kAlchemy, 0 }, { 3358, kAlchemy, 0 },
    // leather: leatherworkers
    { 2318, kLeatherworking, 0 }, { 2319, kLeatherworking, 0 }, { 4234, kLeatherworking, 0 }, { 4304, kLeatherworking, 0 }, { 8170, kLeatherworking, 0 },
    // cloth: tailors and first aiders
    { 2589, kTailoring, kFirstAid }, { 2592, kTailoring, kFirstAid }, { 4306, kTailoring, kFirstAid }, { 4338, kTailoring, kFirstAid }, { 14047, kTailoring, kFirstAid },
};

bool Uses(std::array<std::uint16_t, 2> const& skill, std::uint16_t line)
{
    return line != 0 && (skill[0] == line || skill[1] == line);
}

void Roll(Count& count, std::uint64_t nowMs)
{
    if (count.Today == 0 || nowMs - count.DayStartMs >= kDayMs)
    {
        count.DayStartMs = nowMs;
        count.Today = 0;
    }
}

std::uint32_t Today(std::map<std::uint64_t, Count>& counts, std::uint64_t guid, std::uint64_t nowMs)
{
    auto const it = counts.find(guid);
    if (it == counts.end())
        return 0;
    Roll(it->second, nowMs);
    return it->second.Today;
}

void Bump(std::map<std::uint64_t, Count>& counts, std::uint64_t guid, std::uint64_t nowMs)
{
    Count& count = counts[guid];
    Roll(count, nowMs);
    ++count.Today;
}

}  // namespace

std::vector<std::uint16_t> ConsumersOf(std::uint32_t entry)
{
    std::vector<std::uint16_t> out;
    for (Material const& m : kMaterials)
        if (m.Entry == entry)
        {
            if (m.A)
                out.push_back(m.A);
            if (m.B)
                out.push_back(m.B);
        }
    return out;
}

int PickMaterial(Settings const& settings, std::vector<Stack> const& stacks, std::array<std::uint16_t, 2> const& recipientSkill,
                 std::array<std::uint16_t, 2> const& senderSkill)
{
    int best = -1;
    for (std::size_t i = 0; i < stacks.size(); ++i)
    {
        Stack const& s = stacks[i];
        if (s.ItemGuid == 0 || s.Count < settings.MaterialMinCount || s.Count > settings.MaterialMaxCount)
            continue;
        std::vector<std::uint16_t> const users = ConsumersOf(s.Entry);
        bool theirs = false;
        bool mine = false;
        for (std::uint16_t line : users)
        {
            theirs = theirs || Uses(recipientSkill, line);
            mine = mine || Uses(senderSkill, line);
        }
        if (!theirs || mine)
            continue;
        if (best < 0 || s.Count > stacks[std::size_t(best)].Count || (s.Count == stacks[std::size_t(best)].Count && s.Entry < stacks[std::size_t(best)].Entry))
            best = int(i);
    }
    return best;
}

std::vector<Offer> Plan(Settings const& settings, State& state, std::vector<Member> const& members, std::uint64_t nowMs)
{
    std::vector<Offer> out;
    if (!settings.Enabled || settings.DailyCap == 0 || settings.MaxPerPlan == 0 || nowMs < state.PlanAtMs)
        return out;
    state.PlanAtMs = nowMs + std::uint64_t(settings.PlanEverySec) * 1000;
    if (members.size() < 2)
        return out;

    std::vector<std::size_t> order(members.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return members[a].Guid < members[b].Guid; });
    std::size_t const start = std::size_t(state.Cursor % order.size());
    ++state.Cursor;

    std::uint64_t const cooldownMs = std::uint64_t(settings.CooldownSec) * 1000;
    for (std::size_t step = 0; step < order.size() && out.size() < settings.MaxPerPlan; ++step)
    {
        Member const& from = members[order[(start + step) % order.size()]];
        if (!from.Free || from.Guild == 0)
            continue;
        if (auto const last = state.LastSentMs.find(from.Guid); last != state.LastSentMs.end() && nowMs - last->second < cooldownMs)
            continue;
        if (Today(state.Sent, from.Guid, nowMs) >= settings.DailyCap)
            continue;

        // The guildmate to help: gold goes to the poorest needy one, materials to the first whose professions use what the sender may carry.
        Member const* needy = nullptr;
        Member const* crafter = nullptr;
        std::uint64_t const surplus = from.Copper > settings.GoldKeepCopper ? from.Copper - settings.GoldKeepCopper : 0;
        for (Member const& to : members)
        {
            if (to.Guid == from.Guid || to.Guild != from.Guild || to.Side != from.Side)
                continue;
            if (Today(state.Received, to.Guid, nowMs) >= settings.RecipientDailyCap)
                continue;
            if (settings.Gold && surplus >= settings.GoldMinCopper && to.Copper < settings.NeedyBelowCopper &&
                (!needy || to.Copper < needy->Copper || (to.Copper == needy->Copper && to.Guid < needy->Guid)))
                needy = &to;
            if (settings.Materials && !crafter && (to.Skill[0] != 0 || to.Skill[1] != 0))
            {
                bool wanted = false;
                for (std::uint16_t line : { to.Skill[0], to.Skill[1] })
                    if (line == kAlchemy || line == kBlacksmithing || line == kEngineering || line == kLeatherworking || line == kTailoring)
                        wanted = true;
                bool const sameUse = (to.Skill[0] != 0 && Uses(from.Skill, to.Skill[0])) || (to.Skill[1] != 0 && Uses(from.Skill, to.Skill[1]));
                if (wanted && !sameUse)
                    crafter = &to;
            }
        }

        Offer offer;
        offer.Sender = from.Guid;
        if (needy)
        {
            offer.What = Kind::Gold;
            offer.Recipient = needy->Guid;
            std::uint64_t const target = settings.NeedyBelowCopper * 2;
            std::uint64_t want = target > needy->Copper ? target - needy->Copper : 0;
            offer.Copper = std::min({ want, surplus, settings.GoldMaxCopper });
            if (offer.Copper < settings.GoldMinCopper)
                offer.Copper = 0;
        }
        if (offer.Copper == 0 && crafter)
        {
            offer.What = Kind::Materials;
            offer.Recipient = crafter->Guid;
            offer.RecipientSkill = crafter->Skill;
            offer.SenderSkill = from.Skill;
        }
        else if (offer.Copper == 0)
            continue;

        state.LastSentMs[from.Guid] = nowMs;
        Bump(state.Sent, from.Guid, nowMs);
        Bump(state.Received, offer.Recipient, nowMs);
        out.push_back(offer);
    }
    return out;
}

void NoteFailed(State& state, std::uint64_t sender, std::uint64_t recipient)
{
    if (auto const it = state.Sent.find(sender); it != state.Sent.end() && it->second.Today > 0)
        --it->second.Today;
    if (auto const it = state.Received.find(recipient); it != state.Received.end() && it->second.Today > 0)
        --it->second.Today;
}

std::string GoldSubject(std::uint64_t copper)
{
    std::string text = "Some coin for you: ";
    if (copper >= 10000)
        text += std::to_string(copper / 10000) + "g ";
    if (copper % 10000 >= 100)
        text += std::to_string(copper % 10000 / 100) + "s ";
    if (copper % 100 || copper == 0)
        text += std::to_string(copper % 100) + "c";
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    return text;
}

namespace {
std::string Clip(std::string text)
{
    if (text.size() > 64)
        text.resize(64);
    return text;
}
}  // namespace

std::string MaterialSubject(std::string const& itemName) { return Clip("Materials for your craft: " + itemName); }
std::string GearSubject(std::string const& itemName) { return Clip("This suits you better: " + itemName); }

std::uint32_t BestArmorSubclass(std::uint32_t classId, std::uint32_t level)
{
    switch (classId)
    {
        case 1: case 2: return level >= 40 ? 4 : 3;   // warrior, paladin
        case 3: case 7: return level >= 40 ? 3 : 2;   // hunter, shaman
        case 4: case 11: return 2;                    // rogue, druid
        case 5: case 8: case 9: return 1;             // priest, mage, warlock
        default: return 0;
    }
}

int PickGearRecipient(gear_score::Item const& item, bool bound, bool senderWouldWear, std::vector<GearCandidate> const& candidates)
{
    if (bound || senderWouldWear || item.QuestItem || item.InventoryType == 0)
        return -1;
    int best = -1;
    std::int32_t bestGain = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i)
    {
        GearCandidate const& c = candidates[i];
        if (c.Level < item.RequiredLevel)
            continue;
        if (item.ArmorSubclass != 0 && item.ArmorSubclass > BestArmorSubclass(c.ClassId, c.Level))
            continue;
        if (c.WornSlots == 0)
            continue;
        gear_score::Weights const weights = gear_score::WeightsOf(gear_score::ArchetypeOf(c.ClassId, c.Role));
        std::int32_t const score = gear_score::Score(weights, item);
        // The worn item it would replace: the weakest of the slots, an empty slot scoring nothing.
        std::int32_t weakest = kEmptySlotScore;
        if (c.Worn.size() >= c.WornSlots)
        {
            weakest = gear_score::Score(weights, c.Worn[0]);
            for (gear_score::Item const& w : c.Worn)
                weakest = std::min(weakest, gear_score::Score(weights, w));
        }
        std::int32_t const gain = score - weakest;
        if (c.Worn.size() >= c.WornSlots && gain < gear_score::kSellMarginScore)
            continue;
        if (gain <= 0)
            continue;
        if (best < 0 || gain > bestGain || (gain == bestGain && c.Guid < candidates[std::size_t(best)].Guid))
        {
            best = int(i);
            bestGain = gain;
        }
    }
    return best;
}

}  // namespace botmail
