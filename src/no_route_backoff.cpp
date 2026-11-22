// SPDX-License-Identifier: GPL-2.0-or-later
#include "no_route_backoff.h"

#include <algorithm>
#include <cmath>

namespace no_route_backoff
{

namespace
{
constexpr std::uint32_t kMaxDoublings = 20;

Entry* Find(Options const& options, Book& book, std::uint32_t map, float x, float y)
{
    for (Entry& entry : book.Entries)
        if (entry.Map == map && std::hypot(entry.X - x, entry.Y - y) < options.SameYards)
            return &entry;
    return nullptr;
}

void Prune(Options const& options, Book& book, std::uint64_t nowMs)
{
    std::erase_if(book.Entries, [&](Entry const& entry) { return nowMs >= entry.UntilMs && nowMs - entry.LastMs > options.ForgetMs; });
}
} // namespace

std::uint64_t WaitMs(Options const& options, std::uint32_t fails)
{
    if (fails == 0)
        return 0;
    std::uint32_t const doublings = std::min(fails - 1, kMaxDoublings);
    std::uint64_t const wait = options.BaseMs << doublings;
    return std::min(wait, std::max(options.MaxMs, options.BaseMs));
}

Entry const* NoteRefused(Options const& options, Book& book, std::uint32_t map, float x, float y, float z, std::uint64_t nowMs)
{
    if (!options.Enabled)
        return nullptr;
    Prune(options, book, nowMs);
    Entry* entry = Find(options, book, map, x, y);
    if (!entry)
    {
        if (options.MaxEntries == 0)
            return nullptr;
        if (book.Entries.size() >= options.MaxEntries)
            book.Entries.erase(std::min_element(book.Entries.begin(), book.Entries.end(),
                                                [](Entry const& a, Entry const& b) { return a.LastMs < b.LastMs; }));
        book.Entries.push_back(Entry{ map, x, y, z, 0, 0, 0, false });
        entry = &book.Entries.back();
    }
    ++entry->Fails;
    entry->LastMs = nowMs;
    entry->UntilMs = nowMs + WaitMs(options, entry->Fails);
    entry->HeldLogged = false;
    return entry;
}

Entry* Blocked(Options const& options, Book& book, std::uint32_t map, float x, float y, std::uint64_t nowMs)
{
    if (!options.Enabled || book.Entries.empty())
        return nullptr;
    Prune(options, book, nowMs);
    Entry* entry = Find(options, book, map, x, y);
    return entry && nowMs < entry->UntilMs ? entry : nullptr;
}

void NoteReached(Options const& options, Book& book, std::uint32_t map, float x, float y)
{
    if (!options.Enabled)
        return;
    std::erase_if(book.Entries, [&](Entry const& entry) { return entry.Map == map && std::hypot(entry.X - x, entry.Y - y) < options.SameYards; });
}

} // namespace no_route_backoff
