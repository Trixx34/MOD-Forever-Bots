// SPDX-License-Identifier: GPL-2.0-or-later
#include "lfg_queue_fill.h"

namespace lfg_queue_fill {

std::uint32_t RoleToFill(Settings const& settings, Queue const& queue)
{
    if (!settings.Enabled || queue.QueuedSec < settings.WaitSec)
        return 0;
    std::uint32_t open = 0;
    for (std::uint32_t const flags : queue.OpenRoles)
        open |= flags;
    for (std::uint32_t const role : { kTank, kHealer, kDps })
        if (open & role)
            return role;
    return 0;
}

bool Limiter::Ready(std::uint32_t map, std::uint64_t nowMs, std::uint32_t cooldownSec) const
{
    auto const it = _last.find(map);
    return it == _last.end() || nowMs - it->second >= std::uint64_t(cooldownSec) * 1000;
}

}  // namespace lfg_queue_fill
