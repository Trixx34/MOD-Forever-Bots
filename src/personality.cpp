// SPDX-License-Identifier: GPL-2.0-or-later
#include "personality.h"

#include "bot_random.h"

#include <algorithm>

namespace personality {

namespace {

constexpr std::uint32_t kSaltPace = 0xA0;
constexpr std::uint32_t kSaltCaution = 0xA1;
constexpr std::uint32_t kSaltSocial = 0xA2;
constexpr std::uint32_t kSaltSession = 0xA3;
constexpr std::uint32_t kSaltPatience = 0xA4;
constexpr std::uint32_t kSaltReaction = 0xA5;
constexpr std::uint32_t kSaltMistake = 0xA6;
constexpr std::uint32_t kSaltJitter = 0xA7;
constexpr std::uint32_t kSaltForget = 0xA8;
constexpr std::uint32_t kSaltForgetFor = 0xA9;

std::uint32_t Trait(std::uint64_t seed, std::uint64_t bot, std::uint32_t salt)
{
    return bot_random::Below(bot_random::Draw(seed, bot, salt, 0), 101);
}

// A trait as -1..+1 around the middle.
double Centered(std::uint32_t trait)
{
    return (double(std::min<std::uint32_t>(trait, 100)) - 50.0) / 50.0;
}

double Strength(std::uint32_t pct)
{
    return double(std::min<std::uint32_t>(pct, 100)) / 100.0;
}

} // namespace

Traits TraitsFor(std::uint64_t seed, std::uint64_t bot)
{
    Traits traits;
    traits.Pace = Trait(seed, bot, kSaltPace);
    traits.Caution = Trait(seed, bot, kSaltCaution);
    traits.Sociability = Trait(seed, bot, kSaltSocial);
    traits.Session = Trait(seed, bot, kSaltSession);
    traits.Patience = Trait(seed, bot, kSaltPatience);
    return traits;
}

std::uint32_t RestPct(Settings const& settings, Traits const& traits, std::uint32_t base)
{
    if (!settings.Enabled || settings.RestStrength == 0)
        return base;
    double const shifted = double(base) + Centered(traits.Caution) * 8.0 * Strength(settings.RestStrength);
    return std::clamp<std::uint32_t>(std::uint32_t(shifted + 0.5), kRestFloorPct, kRestCeilPct);
}

std::uint64_t IdleDwellMs(Settings const& settings, Traits const& traits, std::uint64_t baseMs)
{
    if (!settings.Enabled || settings.IdleStrength == 0 || baseMs == 0)
        return baseMs;
    double const slow = (Centered(traits.Patience) + Centered(100 - std::min<std::uint32_t>(traits.Pace, 100))) / 2.0;
    double const factor = std::clamp(1.0 + slow * 0.6 * Strength(settings.IdleStrength), 0.4, 1.6);
    return std::uint64_t(double(baseMs) * factor);
}

std::uint64_t ReactionDelayMs(Settings const& settings, Traits const& traits, std::uint64_t seed, std::uint64_t bot, std::uint64_t counter)
{
    if (!settings.Enabled || settings.ReactionMaxMs == 0)
        return 0;
    std::uint64_t const top = std::uint64_t(std::min(settings.ReactionMaxMs, kMaxReactionMs)) * (100 - std::min<std::uint32_t>(traits.Pace, 100)) / 100;
    return top == 0 ? 0 : bot_random::Draw(seed, bot, kSaltReaction, counter) % (top + 1);
}

std::size_t PackCap(Settings const& settings, Traits const& traits, std::size_t base)
{
    if (!settings.Enabled || settings.CautionStrength == 0 || base <= 1)
        return base;
    std::uint32_t const wary = 100 - std::min<std::uint32_t>(settings.CautionStrength, 100) / 2;
    return traits.Caution >= wary ? base - 1 : base;
}

bool MakesMistake(Settings const& settings, Traits const& traits, std::uint64_t seed, std::uint64_t bot, std::uint64_t counter)
{
    if (!settings.Enabled || settings.MistakeMaxPct == 0)
        return false;
    // In per-mille: MistakeMaxPct * 10 * (100 - Caution) / 100.
    std::uint32_t const chance = std::min<std::uint32_t>(settings.MistakeMaxPct, 30) * (100 - std::min<std::uint32_t>(traits.Caution, 100)) / 10;
    return bot_random::Below(bot_random::Draw(seed, bot, kSaltMistake, counter), 1000) < chance;
}

std::uint32_t AfkChancePct(Settings const& settings, Traits const& traits, std::uint32_t basePct)
{
    if (!settings.Enabled || settings.SocialStrength == 0)
        return basePct;
    double const value = double(basePct) * (1.0 + Centered(traits.Sociability) * Strength(settings.SocialStrength));
    return std::min<std::uint32_t>(std::uint32_t(std::max(0.0, value) + 0.5), 100);
}

std::uint64_t AfkStayMs(Settings const& settings, Traits const& traits, std::uint64_t baseMs)
{
    if (!settings.Enabled || settings.SocialStrength == 0)
        return baseMs;
    return std::uint64_t(double(baseMs) * std::max(0.1, 1.0 + Centered(traits.Session) * Strength(settings.SocialStrength)));
}

std::uint32_t GreetChancePct(Settings const& settings, Traits const& traits, std::uint32_t basePct)
{
    return AfkChancePct(settings, traits, basePct);
}

std::uint64_t GuildPollMs(Settings const& settings, Traits const& traits, std::uint64_t baseMs)
{
    if (!settings.Enabled || settings.SocialStrength == 0 || baseMs == 0)
        return baseMs;
    double const factor = std::clamp(1.0 - Centered(traits.Sociability) * 0.5 * Strength(settings.SocialStrength), 0.5, 1.5);
    return std::uint64_t(double(baseMs) * factor);
}

std::uint64_t SessionMs(Settings const& settings, Traits const& traits, std::uint64_t baseMs)
{
    if (!settings.Enabled || settings.SessionStrength == 0 || baseMs == 0)
        return baseMs;
    double const factor = std::clamp(1.0 + Centered(traits.Session) * 0.6 * Strength(settings.SessionStrength), 0.4, 1.6);
    return std::uint64_t(double(baseMs) * factor);
}

std::uint32_t SessionPct(Settings const& settings, Traits const& traits)
{
    return std::uint32_t(SessionMs(settings, traits, 100000) / 1000);
}

bool ImperfectionAllowed(Settings const& settings, bool instanced)
{
    return settings.Enabled && (!instanced || settings.Instances);
}

std::uint64_t JitterMs(Settings const& settings, Traits const& traits, bool instanced, std::uint64_t seed, std::uint64_t bot, std::uint64_t counter)
{
    if (!ImperfectionAllowed(settings, instanced) || settings.JitterMaxMs == 0)
        return 0;
    std::uint64_t const top = std::uint64_t(std::min(settings.JitterMaxMs, kMaxJitterMs)) * (100 - std::min<std::uint32_t>(traits.Pace, 100)) / 100;
    return top == 0 ? 0 : bot_random::Draw(seed, bot, kSaltJitter, counter) % (top + 1);
}

bool ForgetsBuff(Settings const& settings, Traits const& traits, bool instanced, std::uint64_t seed, std::uint64_t bot, std::uint64_t counter)
{
    if (!ImperfectionAllowed(settings, instanced) || settings.ForgetBuffPct == 0)
        return false;
    // In per-mille: ForgetBuffPct * 10 * (100 - Caution) / 100.
    std::uint32_t const chance = std::min<std::uint32_t>(settings.ForgetBuffPct, 30) * (100 - std::min<std::uint32_t>(traits.Caution, 100)) / 10;
    return bot_random::Below(bot_random::Draw(seed, bot, kSaltForget, counter), 1000) < chance;
}

std::uint64_t ForgetMs(std::uint64_t seed, std::uint64_t bot, std::uint64_t counter)
{
    return kForgetMinMs + bot_random::Draw(seed, bot, kSaltForgetFor, counter) % (kForgetMaxMs - kForgetMinMs + 1);
}

} // namespace personality
