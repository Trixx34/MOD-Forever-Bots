#include "trade_policy.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace trade {

namespace {

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

std::string Trim(std::string const& text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return text.substr(begin, end - begin);
}

// "Linen Cloth x20" (the count only when there is more than one in the stack).
std::string EntryText(std::size_t number, BagItem const& item)
{
    std::string name = item.Name.size() > 80 ? item.Name.substr(0, 80) : item.Name;
    std::string text = std::to_string(number) + ") " + name;
    if (item.Count > 1)
        text += " x" + std::to_string(item.Count);
    return text;
}

Step Whisper(std::uint64_t bot, std::uint64_t partner, std::string text)
{
    Step step;
    step.What = Step::Kind::Whisper;
    step.Bot = bot;
    step.Partner = partner;
    step.Text = std::move(text);
    return step;
}

Step Plain(Step::Kind kind, std::uint64_t bot)
{
    Step step;
    step.What = kind;
    step.Bot = bot;
    return step;
}

std::string Join(std::vector<std::string> const& words, char const* separator)
{
    std::string out;
    for (std::string const& word : words)
        out += (out.empty() ? "" : separator) + word;
    return out;
}

} // namespace

std::vector<std::string> ListWhispers(std::vector<BagItem> const& items, std::size_t maxBytes)
{
    std::vector<std::string> out;
    if (items.empty())
    {
        out.push_back("I have nothing I can trade. You can put items in the window for me.");
        return out;
    }

    // "[99/99] " is the longest page prefix; the entries of each page fit in what is left.
    std::size_t const budget = maxBytes > 8 ? maxBytes - 8 : maxBytes;
    std::vector<std::string> pages;
    std::string page;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        std::string const entry = EntryText(i + 1, items[i]);
        if (!page.empty() && page.size() + 2 + entry.size() > budget)
        {
            pages.push_back(page);
            page.clear();
        }
        page += (page.empty() ? "" : "; ") + entry;
    }
    if (!page.empty())
        pages.push_back(page);

    out.push_back("I can give whole stacks. Whisper me the numbers or names you want:");
    for (std::size_t p = 0; p < pages.size(); ++p)
        out.push_back("[" + std::to_string(p + 1) + "/" + std::to_string(pages.size()) + "] " + pages[p]);
    return out;
}

Reply ParseReply(std::string const& text, std::vector<BagItem> const& items)
{
    Reply reply;
    std::string lowered = Lower(Trim(text));
    if (lowered == "cancel")
    {
        reply.Cancel = true;
        return reply;
    }

    // "and" between pieces counts as a comma.
    for (std::size_t at = lowered.find(" and "); at != std::string::npos; at = lowered.find(" and ", at + 1))
        lowered.replace(at, 5, ",");

    std::vector<std::string> pieces;
    {
        std::string piece;
        for (char c : lowered)
        {
            if (c == ',' || c == ';')
            {
                pieces.push_back(Trim(piece));
                piece.clear();
            }
            else
                piece += c;
        }
        pieces.push_back(Trim(piece));
    }

    static std::regex const numberOnly(R"(^(\d+)\s*(?:x\s*(\d+))?$)");
    static std::regex const countThenName(R"(^(\d+)\s*(?:x\s+)?(.+)$)");
    static std::regex const nameThenCount(R"(^(.+?)\s*x\s*(\d+)$)");

    std::set<std::size_t> taken;
    auto choose = [&](std::size_t index, std::uint32_t count)
    {
        if (taken.insert(index).second)
            reply.Chosen.push_back({ index, count });
    };

    for (std::string const& piece : pieces)
    {
        if (piece.empty())
            continue;
        if (piece == "all")
        {
            reply.All = true;
            continue;
        }

        std::smatch m;
        std::string name = piece;
        std::uint32_t count = 0;
        if (std::regex_match(piece, m, numberOnly))
        {
            std::size_t const number = std::stoul(m[1].str());
            if (number >= 1 && number <= items.size())
                choose(number - 1, m[2].matched ? std::uint32_t(std::stoul(m[2].str())) : 0);
            else
                reply.NotFound.push_back(piece);
            continue;
        }
        if (std::regex_match(piece, m, countThenName))
        {
            count = std::uint32_t(std::stoul(m[1].str()));
            name = Trim(m[2].str());
        }
        else if (std::regex_match(piece, m, nameThenCount))
        {
            name = Trim(m[1].str());
            count = std::uint32_t(std::stoul(m[2].str()));
        }

        // Exact, then the start of the name, then anywhere in it.
        std::vector<std::size_t> matches;
        for (int pass = 0; pass < 3 && matches.empty(); ++pass)
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                std::string const itemName = Lower(items[i].Name);
                bool const hit = pass == 0 ? itemName == name : pass == 1 ? itemName.compare(0, name.size(), name) == 0
                                                                           : itemName.find(name) != std::string::npos;
                if (hit)
                    matches.push_back(i);
            }
        if (matches.empty())
        {
            reply.NotFound.push_back(piece);
            continue;
        }

        std::set<std::uint32_t> entries;
        for (std::size_t i : matches)
            entries.insert(items[i].Entry);
        if (entries.size() > 1)
        {
            std::vector<std::string> names;
            for (std::size_t i : matches)
                if (std::find(names.begin(), names.end(), items[i].Name) == names.end())
                    names.push_back(items[i].Name);
            reply.Ambiguous.push_back(piece + " (" + Join(names, ", ") + ")");
            continue;
        }

        // Several stacks of one item: each mention takes the next stack not yet chosen.
        for (std::size_t i : matches)
            if (!taken.count(i))
            {
                choose(i, count);
                break;
            }
    }
    return reply;
}

std::uint64_t TradeBook::PartnerOf(std::uint64_t bot) const
{
    auto const it = _sessions.find(bot);
    return it == _sessions.end() ? 0 : it->second.Partner;
}

std::vector<Step> TradeBook::OnEvent(std::uint64_t bot, std::uint64_t partner, Status status, std::uint64_t nowMs)
{
    std::vector<Step> steps;
    switch (status)
    {
        case Status::Requested:
        {
            Session session;
            session.Partner = partner;
            session.LastMs = nowMs;
            _sessions[bot] = std::move(session);
            steps.push_back(Plain(Step::Kind::Begin, bot));
            steps.push_back(Plain(Step::Kind::ListBags, bot));
            break;
        }
        case Status::Opened:
        case Status::Changed:
        {
            auto const it = _sessions.find(bot);
            if (it != _sessions.end())
                it->second.LastMs = nowMs;
            break;
        }
        case Status::PartnerAccepted:
        {
            auto const it = _sessions.find(bot);
            if (it != _sessions.end())
            {
                it->second.LastMs = nowMs;
                steps.push_back(Plain(Step::Kind::Accept, bot));
            }
            break;
        }
        case Status::Cancelled:
            _sessions.erase(bot);
            break;
        case Status::Completed:
        {
            auto const it = _sessions.find(bot);
            if (it != _sessions.end())
            {
                steps.push_back(Whisper(bot, it->second.Partner, "Trade complete."));
                _sessions.erase(it);
            }
            break;
        }
    }
    return steps;
}

std::vector<Step> TradeBook::OnBagList(std::uint64_t bot, std::vector<BagItem> items, std::uint64_t nowMs)
{
    std::vector<Step> steps;
    auto const it = _sessions.find(bot);
    if (it == _sessions.end())
        return steps;
    Session& session = it->second;
    session.Items = std::move(items);
    session.Listed = true;
    session.LastMs = nowMs;
    for (std::string const& line : ListWhispers(session.Items))
        steps.push_back(Whisper(bot, session.Partner, line));
    return steps;
}

std::vector<Step> TradeBook::OnWhisper(std::uint64_t speaker, std::uint64_t bot, std::string const& text, std::uint64_t nowMs)
{
    std::vector<Step> steps;
    auto const it = _sessions.find(bot);
    if (it == _sessions.end() || !it->second.Listed || it->second.Partner != speaker)
        return steps;
    Session& session = it->second;
    session.LastMs = nowMs;

    Reply const reply = ParseReply(text, session.Items);
    if (reply.Cancel)
    {
        steps.push_back(Plain(Step::Kind::Cancel, bot));
        _sessions.erase(it);
        return steps;
    }

    std::vector<std::size_t> wanted;
    std::vector<std::string> partial;
    for (Choice const& choice : reply.Chosen)
    {
        BagItem const& item = session.Items[choice.Index];
        if (choice.Count != 0 && choice.Count < item.Count)
            partial.push_back(item.Name + " (x" + std::to_string(item.Count) + ")");
        else
            wanted.push_back(choice.Index);
    }
    if (reply.All)
        for (std::size_t i = 0; i < session.Items.size(); ++i)
            if (std::find(wanted.begin(), wanted.end(), i) == wanted.end())
                wanted.push_back(i);

    std::size_t overflow = 0;
    for (std::size_t index : wanted)
    {
        if (session.Placed.count(index))
            continue;
        if (session.NextSlot >= kTradeSlots)
        {
            ++overflow;
            continue;
        }
        Step step;
        step.What = Step::Kind::SetItem;
        step.Bot = bot;
        step.Partner = session.Partner;
        step.Slot = session.NextSlot++;
        step.ItemGuid = session.Items[index].ItemGuid;
        step.Text = session.Items[index].Name;
        session.Placed.insert(index);
        steps.push_back(std::move(step));
    }

    if (overflow > 0)
        steps.push_back(Whisper(bot, session.Partner,
                                "The window holds " + std::to_string(kTradeSlots) + " items. Ask again in the next trade for the other " +
                                    std::to_string(overflow) + "."));
    if (!partial.empty())
        steps.push_back(Whisper(bot, session.Partner, "I can only give whole stacks: " + Join(partial, ", ") + "."));
    if (!reply.Ambiguous.empty())
        steps.push_back(Whisper(bot, session.Partner, "Which do you mean? " + Join(reply.Ambiguous, "; ") + "."));
    if (!reply.NotFound.empty())
        steps.push_back(Whisper(bot, session.Partner, "I have nothing matching: " + Join(reply.NotFound, ", ") + "."));
    return steps;
}

std::vector<Step> TradeBook::OnItemRefused(std::uint64_t bot, std::string const& itemName)
{
    std::vector<Step> steps;
    auto const it = _sessions.find(bot);
    if (it != _sessions.end())
        steps.push_back(Whisper(bot, it->second.Partner, "I couldn't put " + itemName + " in the trade."));
    return steps;
}

std::vector<Step> TradeBook::OnIdle(std::uint64_t nowMs)
{
    std::vector<Step> steps;
    for (auto it = _sessions.begin(); it != _sessions.end();)
    {
        if (nowMs - it->second.LastMs > _idleMs)
        {
            steps.push_back(Whisper(it->first, it->second.Partner, "The trade timed out."));
            steps.push_back(Plain(Step::Kind::Cancel, it->first));
            it = _sessions.erase(it);
        }
        else
            ++it;
    }
    return steps;
}

} // namespace trade
