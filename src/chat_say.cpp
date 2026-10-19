#include "chat_say.h"

#include <algorithm>
#include <array>

namespace {

// Stable and well spread, so consecutive guids do not answer in a row.
std::uint64_t Mix(std::uint64_t guid, ChatVerb verb)
{
    std::uint64_t x = guid * 0x9E3779B97F4A7C15ull + (static_cast<std::uint64_t>(verb) + 1) * 0xBF58476D1CE4E5B9ull;
    x ^= x >> 30;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 27;
    return x;
}

template <std::size_t N>
std::string Pick(std::array<char const*, N> const& lines, std::uint64_t guid, ChatVerb verb)
{
    return lines[Mix(guid, verb) % N];
}

} // namespace

std::string AckLine(ChatVerb verb, std::uint64_t botGuid)
{
    switch (verb)
    {
        case ChatVerb::Follow: return Pick<3>({ "On my way.", "Right behind you.", "Following you." }, botGuid, verb);
        case ChatVerb::FollowOff: return Pick<2>({ "Okay, I'll stay put.", "No longer following." }, botGuid, verb);
        case ChatVerb::Stay: return Pick<3>({ "Holding here.", "Staying put.", "I'll wait here." }, botGuid, verb);
        case ChatVerb::StayOff: return Pick<2>({ "Moving again.", "Free to move." }, botGuid, verb);
        case ChatVerb::Stop: return Pick<2>({ "Stopping.", "Standing down." }, botGuid, verb);
        case ChatVerb::Lead: return Pick<3>({ "I'll take the lead.", "Stay behind me.", "Follow me, I know the way." }, botGuid, verb);
        case ChatVerb::LeadOff: return Pick<2>({ "All yours.", "I'll follow you now." }, botGuid, verb);
        case ChatVerb::GotoHere: return Pick<3>({ "Coming.", "On my way over.", "Be right there." }, botGuid, verb);
        case ChatVerb::Release: return Pick<2>({ "Releasing.", "Back to the graveyard." }, botGuid, verb);
        case ChatVerb::Pull: return Pick<2>({ "Pulling!", "Here it comes." }, botGuid, verb);
        case ChatVerb::Heal: return Pick<2>({ "Healing.", "I've got you." }, botGuid, verb);
        case ChatVerb::Passive: return Pick<2>({ "Staying passive.", "I'll only defend myself." }, botGuid, verb);
        case ChatVerb::Aggressive: return Pick<2>({ "Back to normal.", "Ready to fight." }, botGuid, verb);
        case ChatVerb::Summon: return Pick<2>({ "Here I am.", "Summoned." }, botGuid, verb);
        case ChatVerb::Revive: return Pick<2>({ "Thanks!", "Back on my feet." }, botGuid, verb);
        case ChatVerb::Mount: return Pick<2>({ "Mounting up.", "Hop on." }, botGuid, verb);
        case ChatVerb::Dismount: return Pick<2>({ "Getting off.", "On foot again." }, botGuid, verb);
        case ChatVerb::Role: return Pick<2>({ "Role set.", "Got it, new role." }, botGuid, verb);
        case ChatVerb::Stance: return Pick<2>({ "Stance set.", "Understood." }, botGuid, verb);
        case ChatVerb::Focus: return Pick<2>({ "Focusing it.", "Target noted." }, botGuid, verb);
        case ChatVerb::FocusOff: return Pick<2>({ "Focus cleared.", "No focus now." }, botGuid, verb);
        case ChatVerb::Distance: return Pick<2>({ "Distance set.", "I'll keep that distance." }, botGuid, verb);
        case ChatVerb::Status:
        case ChatVerb::None:
            break;
    }
    return {};
}

std::vector<std::uint64_t> PickSpeakers(std::vector<std::uint64_t> const& accepted, ChatVerb verb, bool addressed, std::uint32_t maxSpeakers)
{
    if (addressed)
        return accepted;
    if (maxSpeakers == 0)
        return {};
    std::vector<std::uint64_t> speakers = accepted;
    if (speakers.size() > maxSpeakers)
    {
        std::sort(speakers.begin(), speakers.end(), [verb](std::uint64_t a, std::uint64_t b)
        {
            std::uint64_t const ha = Mix(a, verb);
            std::uint64_t const hb = Mix(b, verb);
            return ha != hb ? ha < hb : a < b;
        });
        speakers.resize(maxSpeakers);
    }
    return speakers;
}
