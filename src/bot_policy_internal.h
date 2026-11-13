// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Private to bot_policy*.cpp: the tuning constants and the declarations of the helpers that were an anonymous namespace in bot_policy.cpp, now shared by its
// split files. The helpers are defined in bot_policy_spells.cpp (spell, class and snapshot facts) and bot_policy_units.cpp (units, distances and places).
#include "bot_policy.h"
#include "heal_rank.h"

#include "forever_module/talent_spec.h"

#include "chat_reply.h"
#include "combat_rotation.h"
#include "route_triage.h"
#include "lfg_fill.h"
#include "talent_respec.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <tuple>

using forever_module::ActionType;
using forever_module::BotAction;
using forever_module::BotActionStatus;
using forever_module::BotSnapshot;
using forever_module::NearbyUnit;
using forever_module::WorldTickSnapshotView;

namespace bot_policy_detail {

// A unit's health and power as whole percent of the maximum; `whenNoMax` for a unit that reports no maximum (a bot not yet in view, a class without that power).
inline std::uint32_t HealthPct(BotSnapshot const& unit, std::uint32_t whenNoMax)
{
    return unit.max_health > 0 ? std::uint32_t(std::uint64_t(unit.health) * 100 / unit.max_health) : whenNoMax;
}
// A rotation row is usable at this level: at or above its MinLevel and not past its MaxLevel (0 = no upper bound). A level of 0 (unknown) takes every row.
inline bool RowInLevelRange(rotation::Row const& row, std::uint32_t level)
{
    return level == 0 || (row.MinLevel <= level && (row.MaxLevel == 0 || level <= row.MaxLevel));
}
inline std::uint32_t PowerPct(BotSnapshot const& unit, std::uint32_t whenNoMax)
{
    return unit.max_power > 0 ? std::uint32_t(std::uint64_t(unit.power) * 100 / unit.max_power) : whenNoMax;
}

constexpr float kDisengagePct = 30.0f;       // below this health the bot stops fighting
constexpr float kRangedKiteYards = 12.0f;    // a ranged bot keeps a melee target this far away
// Fire Nova (spell 20203) reaches 20 yards from the Warder plus the target's combat reach (1.5), and 2 more while the Warder is moving (23.5 in all); the bot stands
// there at the centre-to-centre distance below. 21 yards (the first value) left every kiting bot inside the nova. A tuning value: 24-25 is the range to try.
constexpr float kAoeKiteYards = 24.5f;       // every party member other than the tank keeps an Onyxian Warder this far away
// The stand distance must also be inside the casters' reach: a 30-yard row (Frostbolt, Shadow Bolt, Smite, Lightning Bolt, Wrath) is used to 28 yards (kRangeSlackYards).
// At 5.5 (a stand of 30) no caster ever cast on a Warder: 2026-11-08 16:40-19:35, 126 fight_stalled at distance 28-31 with the Warder at full health, 0 Warders killed.
constexpr float kAoeKiteMarginYards = 1.5f;      // a party member also holds this much beyond kAoeKiteYards, and starts to kite before it is inside the radius, in combat or not
constexpr float kAoeStandYards = kAoeKiteYards + kAoeKiteMarginYards;   // 26: where a kited bot stands, 2.5 outside the nova and 2 inside a 30-yard spell's usable range
constexpr std::uint32_t kOnyxiaMap = 249;              // Onyxia's Lair
constexpr float kOnyxiaEntryX = 30.89f, kOnyxiaEntryY = -54.08f, kOnyxiaEntryZ = -5.03f;   // the lair's entry, the way the party came in (known walkable)
constexpr float kAoeKiteSpreadDegrees = 4.0f;  // each bot takes its own bearing off the Warder, up to 5 steps of this either side of the straight line away
constexpr float kAoeKiteRetryDegrees = 30.0f;  // and a kite point the core found no route to is tried again this much further round
constexpr std::uint64_t kHealLosMoveMs = 4000; // after a heal refused for line of sight a healer may leave its stand-off to find the target, for this long
constexpr std::uint32_t kOnyxianWarderEntry = 12129;
constexpr std::uint32_t kOnyxiaBossEntry = 10184;   // Onyxia herself, the lair's last boss (data/dungeons/249-onyxias-lair.json)
constexpr std::uint32_t kBerserkerRageSpell = 18499; // warrior tank: the fear break in Onyxia's phase 3 (study item 6); 30 s cooldown
constexpr std::uint32_t kTauntSpell = 355;           // warrior tank: re-taunts Onyxia when the raid is on her and the tank is not
constexpr std::uint32_t kGrowlSpell = 6795;                 // feral druid tank: the single taunt
constexpr std::uint32_t kChallengingShoutSpell = 1161;       // warrior: the area taunt, level 26, 10 s channel-free instant, 10 minute cooldown
constexpr std::uint32_t kChallengingRoarSpell = 5209;        // feral druid: the area taunt, level 28
constexpr float kAreaTauntYards = 10.0f;                     // the radius of both area taunts
constexpr std::uint64_t kPickupRetryMs = 3000;               // tank pickup: a single taunt is tried again after this (its cooldown is 10 s, a refusal sets the retry map too)
constexpr std::uint64_t kPickupAoeRetryMs = 15000;           // tank pickup: the area taunt is tried again after this
constexpr std::uint64_t kOnyxiaBerserkerCdMs = 30000;
constexpr std::uint64_t kOnyxiaTauntCdMs = 10000;
constexpr float kOnyxiaTauntYards = 30.0f;           // Taunt's reach: the tank does not cast it at a farther Onyxia
constexpr std::uint64_t kOnyxiaPullHoldMs = 3000;    // the raid's damage dealers hold their attacks this long after the tank first gets Onyxia
// Onyxia's phases by her health (the study's item 3; the snapshot has no aura or spell id on a unit, so the Hover aura and the Deep Breath spell are not read):
// phase 1 on the ground above 65%, phase 2 in the air from 65% to 40%, phase 3 on the ground from 40% down. She lands at 40%: her threat list resets, so the
// damage dealers stop until the tank has her again (study item 2).
constexpr float kOnyxiaFlyPct = 65.0f;
constexpr float kOnyxiaLandPct = 40.0f;
inline int OnyxiaPhaseOf(float pct)
{
    if (pct > kOnyxiaFlyPct) return 1;
    if (pct > kOnyxiaLandPct) return 2;
    return 3;
}
inline bool IsOnyxiaRef(std::uint64_t unitRef)
{
    return unitRef != 0 && forever_module::UnitRefType(unitRef) == 8 && forever_module::UnitRefEntry(unitRef) == kOnyxiaBossEntry;
}
// Explicit stand-off bands from Onyxia (the boss, not the Warder), in yards from her: a bot walks in to Max and kites back out when it is closer than Min. Healers
// 21-25, ranged 23-29, melee 2-9 (the Karrz repo's RaidEncounterData_ONY). The Warder's Fire Nova (kAoeStandYards) is not in these bands: they are for the boss only.
struct StandBand
{
    float Min;
    float Max;
    float Mid() const { return (Min + Max) / 2.0f; }
};
constexpr StandBand kHealerBand{ 21.0f, 25.0f };
constexpr StandBand kRangedBand{ 23.0f, 29.0f };
constexpr StandBand kMeleeBand{ 2.0f, 9.0f };
constexpr float kBandStepDegrees = 15.0f;     // a stand point that a hazard covers is tried this far round the bot's bearing, up to 4 steps either side
// Fire Nova reach from the Warder's centre (see kAoeKiteYards above): a ghost is held only while a live Warder is inside it. The 25-yard hold of 2026-11-08 kept 28 dead bots
// on the floor 25 yards from a Warder that could not reach them (sim, 14:30-14:52).
constexpr float kFireNovaYards = 23.5f;
constexpr float kReviveWarderYards = kFireNovaYards;  // a ghost of a dungeon party is not brought back (entrance teleport, leader's revive, corpse reclaim) while a live Warder is inside the Fire Nova of its corpse or of a living party member
constexpr std::uint64_t kReviveWarderLogMs = 30000;
constexpr float kFleeYards = 15.0f;          // below 30% health: flees while a threat is this close
constexpr std::uint64_t kGlobalCooldownMs = 1500;
constexpr std::uint64_t kDispelRetryMs = 2500;      // a bot sends the same dispel again no sooner than this (raid rules)
constexpr std::uint64_t kRetryMs = 1000;     // out of range, invalid target, refused, not enough power
constexpr std::uint64_t kCooldownRetryMs = 2000;
constexpr float kRangeSlackYards = 2.0f;        // a ranged row is used this far inside its range: the snapshot is a tick old and the units move
constexpr std::uint64_t kPowerRetryMs = 2000;    // a cast refused for lack of mana, rage or energy: power comes back slowly
constexpr std::uint64_t kBuffPowerRetryMs = 30000; // an upkeep buff refused for power: it is cast out of combat, where a warrior has no rage at all
constexpr std::uint64_t kBuffRefusedRetryMs = 5000; // an upkeep buff refused for another reason (not ready, the global cooldown): not worth a try a second
constexpr std::uint64_t kHeldCastMs = 500;       // after a cooldown or generic refusal no other spell is tried for this long (often the global cooldown)
constexpr std::uint64_t kCloserAfterRangeMs = 5000; // after an out-of-range refusal the bot stands closer for this long
constexpr float kCasterCloserYards = 15.0f;      // the closer stand-off of a ranged bot after an out-of-range refusal
constexpr float kMeleeCloserYards = 3.0f;        // and of a melee bot
constexpr std::uint32_t kLosRefusalsAvoid = 3;   // this many line-of-sight refusals in a row and the target is avoided
constexpr std::uint64_t kBusyRetryMs = 300;  // a cast refused because the bot was casting or moving: tried again very soon
constexpr std::uint64_t kNotKnownRetryMs = 60000; // the spell may be learned later
constexpr float kOtherLevelHeight = 12.0f;  // yards: a hostile this far above or below the bot ...
constexpr float kOtherLevelSlope = 0.5f;    // ... and steeper than this (height over flat distance, ~27 degrees) is on another level: a wall walk, a ledge, a floor above or below
constexpr float kPullRadius = 10.0f;         // yards: hostiles this close to a candidate are pulled with it
constexpr std::size_t kMaxDungeonPull = 4;  // a dungeon pack is larger than an outdoor one; the party pulls up to this many together (3 was tried in 0.32.0: no gain, reverted)
constexpr std::size_t kMaxPull = 3;          // a group larger than this is not pulled (over-pull control)
constexpr std::uint32_t kRend = 772;         // warrior rank 1; the bleed lasts 21 seconds
constexpr std::uint64_t kRendDurationMs = 21000;
// kVendor's spell_id bits (the core's ModuleBotActions): sell grey items, repair, equip the best gear.
constexpr std::uint32_t kServiceSell = 1;
constexpr std::uint32_t kServiceRepair = 2;
constexpr std::uint32_t kServiceEquip = 4;
constexpr std::uint32_t kServiceBuyHealthPotion = 64; // layout 16: the core's settings give the counts
constexpr std::uint32_t kServiceBuyManaPotion = 128;
constexpr std::uint32_t kServiceBuyBandage = 256;
constexpr std::uint32_t kServiceBuyReagent = 512;  // the item and the count are the core's
// kSkillUp (SDK 0.17): the skill lines, and how long the policy waits after each answer.
constexpr std::uint32_t kSkillFirstAid = 129;
constexpr std::uint32_t kSkillCooking = 185;
constexpr std::uint64_t kMountFailBaseMs = 120000;      // no mount learned: the first pause is two minutes, doubling up to kMountFailMaxMs
constexpr std::uint64_t kMountFailMaxMs = 3600000;
constexpr std::uint64_t kQuestWindowMs = 300000;        // quest refusals are counted over five minutes
constexpr std::uint32_t kQuestRefusalsPause = 4;        // this many in a window: no quest actions for kQuestPauseMs
constexpr std::uint64_t kQuestPauseMs = 600000;
constexpr std::uint64_t kBuffFailBaseMs = 5000;         // an upkeep buff (or a spell not known) that keeps failing: 5 s, doubling to kSpellFailMaxMs
constexpr std::uint64_t kSpellFailMaxMs = 600000;
constexpr std::uint32_t kGatherFailDoublings = 5;       // nothing to gather near the bot, again: 90 s, 3, 6, 12, 24, 48 minutes at most
constexpr std::uint64_t kNoRoutePauseMs = 5000;        // the first pause after a move got "no route"; it doubles with each refusal in a row (up to 6 doublings is 2.7 minutes)
constexpr float kPartialMinYards = 150.0f;              // a walk this long that got "no route" may be a partial path (the mesh path is capped at about 74 polygons), not a missing route
constexpr float kPartialProgressYards = 25.0f;          // a leg that brought the bot this much nearer keeps the chain going; less and the route is really missing
constexpr float kPartialArrivedYards = 15.0f;           // this near the destination the chain is over
constexpr std::uint64_t kPartialSettleMs = 3000;        // after a leg is sent the bot is judged no sooner than this (it must have started to move)
constexpr std::uint64_t kPartialLegMaxMs = 180000;      // one leg that lasts longer than this ends the chain
constexpr std::uint32_t kPartialLegsMax = 40;           // legs chained for one destination
constexpr std::uint64_t kRefusedPlaceMs = 20000;        // a place the core found no route to is not tried again by the Warder kite for this long
constexpr std::size_t kRefusedPlacesKept = 8;           // how many such places a bot remembers
constexpr float kNoRouteSameYards = 12.0f;               // a destination this close to the refused one is the same place
constexpr std::uint64_t kSkillUpCastMs = 4000;        // after a craft was sent: the cast takes a few seconds, the answer decides what comes next
constexpr std::uint64_t kSkillUpNextMs = 3500;        // a craft that started: the next one is tried when the cast is done
constexpr std::uint64_t kSkillUpBusyMs = 1500;        // the bot was casting or moving: soon
constexpr std::uint64_t kSkillUpRefusedMs = 300000;   // no materials, no recipe, or the skill is too high for the recipes: five minutes (varied per bot)
// SDK 0.18: the lines a bot crafts (the two secondary skills for every bot, the others when the bot has that profession; 186 is smelting, Mining's recipes) and
// gathers with. A skill the bot does not have is never asked for.
constexpr std::size_t kCraftLineCount = 8;
constexpr std::uint32_t kCraftLines[kCraftLineCount] = { 129, 185, 164, 165, 171, 197, 202, 186 };
constexpr std::size_t kGatherLineCount = 3;
constexpr std::uint32_t kGatherLines[kGatherLineCount] = { 186, 182, 393 };
constexpr std::uint64_t kGatherNothingMs = 90000;     // nothing to gather near the bot: ask again in a minute and a half (varied per bot)
constexpr std::uint64_t kGatherRefusedMs = 300000;    // any other refusal (no spell, no skill): five minutes
constexpr std::uint64_t kCraftWalkTimeoutMs = 150000; // a walk to an anvil, forge or fire that has not arrived by now is over
constexpr std::uint64_t kCraftWalkFailMs = 1800000;   // after a walk that failed, or when no object of the focus is on the map or near enough: half an hour
constexpr float kCraftWalkMaxYards = 900.0f;          // farther than this a bot does not walk for a recipe's focus
constexpr std::uint64_t kGatherWalkTimeoutMs = 150000; // a walk to a gathering node that has not arrived by now is over
constexpr float kGatherWalkMaxYards = 900.0f;        // farther than this a bot does not walk for a node
constexpr float kGatherReachYards = 25.0f;           // the core gathers within 30 yards: a bot within 25 of the node has arrived

// Whether the bot has the profession of a craft or gather line: First Aid and Cooking every bot may learn, the others are its two primary professions.
bool HasLine(BotSnapshot const& bot, std::uint32_t line);
constexpr std::uint32_t kServiceSellJunk = 1024;   // SDK 0.16: also sell white junk (trade goods, spare bags, miscellaneous items)
constexpr std::uint32_t kServiceSellGear = 2048;   // SDK 0.28 (protocol 27): sell the bag gear the worn gear outclasses; bits 12-14 carry the gear profile
constexpr std::uint32_t kServiceGearProfileShift = 12;
constexpr std::uint32_t kBagsLowSlots = 4;         // a vendor visit of a bot with this many free slots or fewer also sells the white junk
constexpr std::uint32_t kServiceBuyAmmo = 8;     // layout 15: buy ammunition (dest_x = the count to carry), food (dest_y), drink (dest_z)
constexpr std::uint32_t kServiceBuyFood = 16;
constexpr std::uint32_t kServiceBuyDrink = 32;
constexpr std::uint64_t kSupplyJudgeAfterMs = 7000; // a supply trip is judged this long after the buy: the core counts the bags every 2 seconds
// Combo points: the rogue's Sinister Strike spell and the spend, and the points a finisher needs.
constexpr std::uint32_t kSinisterStrike = 1752;
constexpr std::uint32_t kEviscerate = 2098;
constexpr std::uint32_t kAutoShot = 75;
constexpr std::uint32_t kCallPet = 883;
constexpr std::uint32_t kRevivePet = 982;
constexpr std::uint32_t kMendPet = 136;
constexpr std::uint32_t kHunterClass = 3;
constexpr std::uint32_t kTameBeast = 1515;           // Tame Beast: a 20 second channel; the core creates the pet when it ends (it needs Call Pet, 883, known)
constexpr std::uint32_t kTameMinLevel = 10;          // the level the hunter pet quest is given at
constexpr float kTameSearchYards = 60.0f;            // a beast farther than this is not looked for
constexpr float kTameCastYards = 18.0f;              // the cast range of Tame Beast is 20 yards; walk to within this
constexpr std::uint64_t kTameHoldMs = 22000;         // the channel (20 s) and a margin: no move, no other cast
constexpr std::uint64_t kTameRetryMs = 20000;        // after a cast that gave no pet, or a search that found nothing
constexpr std::uint32_t kTameMaxFailures = 4;        // casts with no pet before the hunter stops for kTameGiveUpMs
constexpr std::uint64_t kTameGiveUpMs = 600000;
constexpr std::uint64_t kTameNotKnownMs = 600000;    // Tame Beast is not learned: no search or cast for this long (a trainer visit or the core may teach it meanwhile)
constexpr std::uint64_t kTamePetFirstMs = 90000;      // Call Pet / Revive Pet get this long to deal with a stored pet before taming is tried again
constexpr std::uint64_t kPetBackoffMs = 30000;       // first wait after a refused revive; doubles per failure
constexpr std::uint64_t kPetBackoffMaxMs = 300000;
constexpr std::uint64_t kPetProbeMs = 120000;        // below level 12 a Call Pet check after a fight is sent at most this often
constexpr std::uint64_t kPetMendEveryMs = 30000;     // Mend Pet in a fight at most this often
constexpr std::uint64_t kPetMendAfterMs = 15000;     // and only once the fight has lasted this long
constexpr std::uint32_t kPetMendManaPct = 50;         // and with at least this much mana
constexpr std::uint32_t kPetMendHealthPct = 85;        // layout 13: a pet below this health is healed
// Layout 13: ActionType::kPetCommand spell_id (the pet is in the snapshot now, so it can be commanded, not just called).
constexpr std::uint32_t kPetAttack = 1;               // attack target_guid
constexpr std::uint32_t kPetFollow = 2;
constexpr std::uint32_t kPetStay = 3;                 // hold position
constexpr std::uint32_t kPetStance = 4;               // passive, defensive or aggressive, in dest_x
constexpr std::uint64_t kTrainReadKey = 0x7261696E00000000ull;   // the reading-delay gate key of a trainer visit (realism.h)
constexpr std::uint32_t kMaxComboPoints = 5;
// Saber Slash (Forever): like Sinister Strike it gives one combo point (SpellEffect 30 energize, power type 4, 1 point, in the client data).
constexpr std::uint32_t kSaberSlash = 424785;

// Mutilate (Assassination, level 40) gives two combo points and Hemorrhage (Subtlety, level 30) one: the builders of the other rogue specs (docs/102 in forever-ops).
constexpr std::uint32_t kMutilate = 399956;
constexpr std::uint32_t kHemorrhage = 16511;
constexpr std::uint32_t kRupture = 1943;
constexpr std::uint32_t kSliceAndDice = 5171;

// The cat-form druid's builders (data/rotations.txt, class 11 spec feral_cat), one combo point each like Sinister Strike:
// Claw (rank 1 1082), Rake (1822) and Shred (5221). Ferocious Bite (22568) is their finisher, like the rogue's Eviscerate.
// Previously only rogue builders were counted, so a feral_cat bot never gained combo points and Ferocious Bite's `combo 5`
// row could never fire (2026-11-10 report, "Feral Ferocious Bite combo 5 can never fire").
constexpr std::uint32_t kClaw = 1082;
constexpr std::uint32_t kRake = 1822;
constexpr std::uint32_t kShred = 5221;
constexpr std::uint32_t kFerociousBite = 22568;

// Combo points a rogue spell adds on its target when it lands, 0 for any other spell.
std::uint32_t ComboPointsGained(std::uint32_t spell);
// A ranged bot below this share of its power walks to melee range and fights there, since its spells are spent.
constexpr std::uint32_t kOutOfPowerPct = 15;
constexpr float kMeleeApproachYards = 5.0f;
constexpr std::uint32_t kRestBelowPct = 90;  // an idle bot below this health sits down to rest
// Parties (M8). kGroup's spell_id (the core's ModuleBotActions): invite the bot named by target_guid, accept the bot's
// pending invite, leave the party.
constexpr std::uint32_t kGroupInvite = 1;
constexpr std::uint32_t kGroupAccept = 2;
constexpr std::uint32_t kGroupLeave = 3;
// 40-man raid support (doc 108): convert the bot's own party to a raid (no target), and move the player named by
// target_guid into the subgroup number given in dest_x (0-7). Both reuse kGroup's existing generic fields -- no new
// ActionType, no SDK protocol bump (ModuleBotActions.cpp, Trinity-Forever-Modules).
constexpr std::uint32_t kGroupConvertRaid = 4;
constexpr std::uint32_t kGroupAssignSubgroup = 5;
constexpr std::uint64_t kRaidConvertRetryMs = 10000;  // ConvertToRaid's success is not visible in the snapshot; retried at this pace
constexpr std::uint64_t kRaidAssignRetryMs = 10000;   // likewise for a subgroup assignment
constexpr std::uint32_t kRaidSubgroupSize = 5;        // TrinityCore's MAX_GROUP_SIZE: bots per raid subgroup
constexpr std::uint32_t kWarriorClass = 1;
constexpr std::uint32_t kPlayerType = 2;     // unit reference type of a player (MakeUnitRef's highType)
constexpr std::uint64_t kInviteRetryMs = 15000; // a member is invited again this long after the last invite
constexpr std::uint64_t kPartyRetryMs = 2000;   // a party action refused, or just sent, is not repeated before this
constexpr float kFollowLeaderYards = 8.0f;      // a member this close to its leader stays where it is
constexpr std::uint64_t kApproachTimeoutMs = 30000; // walking to a target this long without reaching it: avoid the target
constexpr std::uint32_t kMaxLevelAbove = 2;      // a bot does not pull a creature more than this many levels above it
constexpr float kLootRangeYards = 25.0f;         // the core loots a corpse within 30 yards; the bot asks within 25
constexpr std::uint64_t kLootWindowMs = 120000;  // a corpse is looted within this long after the fight, or forgotten
constexpr std::uint64_t kAvoidMs = 30000;        // a target that refused an attack or a cast is skipped for this long
constexpr std::uint32_t kMaxAvoidDoublings = 2;  // each repeat avoid doubles the time: 30 s, 60 s, then 120 s at most
constexpr std::uint64_t kWanderIdleMs = 30000;   // idle for this long, seeing nothing, before walking to a hub
constexpr std::uint64_t kWanderTimeoutMs = 180000; // a walk to a hub longer than this is dropped
// After a hub walk that timed out (a hub in rock, or past what the pathing allows), the next hub is tried this much sooner than a
// full idle interval: going round the list must not cost the bot 30 s of standing for every unreachable hub.
constexpr std::uint64_t kHubRetryIdleMs = 5000;
constexpr float kHubReachYards = 10.0f;          // a bot this close to its hub has arrived
constexpr float kVendorReachYards = 4.0f;       // a bot this close to its vendor can use it (the core allows 5)
constexpr std::uint64_t kBossSweepMaxMs = 300000;  // 0.44.17: at the end of the route the leader goes back to the boss points whose boss is not dead; all of it takes at most this long
constexpr std::uint64_t kBossHoldMs = 180000;     // how long the leader waits at a boss point for the boss to die before it goes on
// A member this far from its leader (2D) has lost it ... The same as kRouteWaitYards: the leader waits for a member farther than that, so a member that stays
// farther (stuck below a ramp the leader climbed) held the party for good when this was 60 (Ragefire Chasm and the Deadmines, 2026-10-26).
constexpr float kLostLeaderYards = 30.0f;
constexpr float kLostLeaderHeight = 25.0f;        // ... or this far above or below it (another level of the dungeon)
constexpr std::uint64_t kLostLeaderMs = 15000;    // for this long, then it is put next to the leader (DungeonTeleport). Longer than kRegroupStuckMs: the leader has
                                                  // flagged the member and walked on by then, so the teleport is not in the same second as the regroup_stuck line (0.36.2)
constexpr float kRouteFarYards = 150.0f;           // a leader this far from a waypoint it gets no route to is not at the point: it is put back on the route instead of skipping
constexpr std::uint32_t kRouteNoRouteSkip = 3;     // refusals in a row (no route) after which a dungeon waypoint is skipped
constexpr std::uint32_t kRepairBelowPct = 50;    // worn gear below this durability sends the bot to a repairman, not just any vendor
// The fights draw the leader past route points: when it is this close to the next point, the one it was heading for (not a boss point) is dropped, so it never walks back
// for it (it did, through the whole dungeon by the pathfinder's only route, with the party standing behind).
constexpr float kRouteSkipAheadYards = 25.0f;
// ... and only when it is on the same floor as that point (2026-11-12): the route's X/Y test alone let a leader on the roof level of Shadowfang Keep (z 100, over
// Rethilgore's room, z 82) count as having reached Rethilgore's point 5 yards away on the map, pass the three points before it, and stand there for kBossHoldMs
// under the boss (boss_missed, then Rethilgore, Silverlaine and Springvale left out in 4 of 14 overnight runs). Reaching a point takes kRouteReachMaxZYards, passing
// one kRouteSkipMaxZYards (the route climbs 7 yards between its stair points, so this is wider than the other; the two floors above are 17 to 19 yards apart).
constexpr float kRouteReachMaxZYards = 10.0f;
constexpr float kRouteSkipMaxZYards = 12.0f;
// A party member in a fight does not chase a target farther than this (a creature that flees at low health runs on for hundreds of yards and takes the chaser away from
// its party, onto the next packs): the target is avoided for a while and the bot stands.
constexpr float kPartyChaseYards = 45.0f;
constexpr float kRoutePointReachYards = 6.0f;    // the leader has arrived at a route waypoint this close to it
constexpr float kFocusPackYards = 12.0f;         // a unit this close to the tank's target is of the same pack: the damage dealers may take it first
// Ragefire Chasm's Searing Blade camp (2026-10-27 soak): kMaxDungeonPull caps the tank's own pull, but nothing capped what the pack-focus scan below
// reached for afterwards. kFocusPackYards is wider than kPullRadius, and the scan has no size limit of its own, so as a fight went on it kept
// reaching a little further for whichever unlisted hostile happened to stand nearby next -- a party ended up fighting the Enforcer, then some
// unrelated trash nobody had told it about, then another, three and four distinct adds deep before anyone died. This caps that: past this many
// distinct hostiles already engaged, the scan stops reaching for an *unlisted* one (not in the dungeon file's kill_priority, rank 1000 below); it
// still reaches for any of the dungeon's own kill_priority entries whatever the count already engaged, since that list is the dungeon author's own
// "kill this one first no matter what" (the first version of this cap did not exempt it, and a same-day re-run of the soak showed why it must: it
// silenced the one tool meant to keep the Searing Blade Warlock -- kill_priority rank 0 -- focused down once two lower-priority trash had already
// claimed the two engaged slots first). 2026-10-27 follow-up: the Warlock does not summon the Voidwalker Minion (entry 8996) mid-fight as first
// assumed -- its only "Summon Voidwalker" cast is out of combat, once, before the pull (see 389-ragefire-chasm.json's kill_priority comment); the
// Voidwalker is really a fourth, passive pack member that just joins the melee late. The same cap-exemption this comment describes is why that
// dungeon file now lists 8996 in kill_priority too: once the Voidwalker attacks, it must stay reachable past the cap the same way the Warlock does.
constexpr std::size_t kMaxDungeonEngaged = 2;
// The cc tactic: Sap (rogue), Polymorph (mage) and Banish (warlock) hold one extra member of the tank's pack. Rank 1 ids; none of
// the three has a second rank on this server (data/spell_costs.txt). kCcRefreshBeforeMs: the hold is recast once this close to
// running out, not only once it has already dropped, so a short gap in the snapshot tick does not let a held unit go free.
constexpr std::uint32_t kSapSpell = 6770;
constexpr std::uint32_t kPolymorphSpell = 118;
constexpr std::uint32_t kBanishSpell = 710;
constexpr std::uint64_t kCcRefreshBeforeMs = 3000;
constexpr float kHealerFetchYards = 45.0f;       // a leader below kRouteMinHealthPct whose nearest healer is farther than this (2D) walks back to it after kHealerFetchAfterMs
constexpr std::uint64_t kHealerFetchAfterMs = 45000;  // 2026-11-12 sim: parties stood 10 minutes in route_hold why=health with the healer 200 to 350 yards behind, nobody to heal the leader
constexpr float kRouteWaitYards = 30.0f;         // the leader waits for a member farther behind than this
constexpr std::uint32_t kRouteMinHealthPct = 80; // and for its own health to be at least this
constexpr std::uint32_t kRouteMinManaPct = 70;   // the leader walks on only when every mana user of the party has at least this much mana (the healer walked into fights at 3%)
constexpr std::uint64_t kRouteManaAbsMaxMs = 180000; // 0.44.14: the wait is cut off here even while mana still rises (a full priest needs about 90 s of drinking from 3%)
constexpr std::uint64_t kRouteManaMaxMs = 35000;  // and never waits longer than this for mana (no drink left, or a member that cannot sit)
constexpr std::uint32_t kRouteTopUpPct = 90;     // out of a fight, the party's healer heals the route leader up to this (above kRouteMinHealthPct, so it walks on)
// A leader walking the route that has not moved this far in this long, out of combat, has a move that went nowhere (sent once, no answer; dm#6 of 2026-10-26 stood
// 53 minutes so): the move is sent again and counted as a refusal (kRouteNoRouteSkip), so the point is recovered or skipped as for a "no route".
constexpr std::uint64_t kRouteStallMs = 30000;
constexpr float kRouteStallYards = 3.0f;
constexpr std::uint64_t kRouteHoldLogMs = 30000; // a leader that does not walk on says why (route_hold), at most this often
// Dungeon pull control: the tank (the party leader) pulls only when the party is ready, and the others wait for the tank to hold the target.
constexpr std::uint32_t kPartyReadyHealthOldPct = 80, kPartyReadyPowerOldPct = 60; // with the Rest tactic off
constexpr std::uint32_t kPartyReadyHealthPct = 90;  // every living member has at least this much health
constexpr std::uint32_t kPartyReadyPowerPct = 85;   // and every mana user at least this much mana (matches drink cap)
constexpr std::uint32_t kPartyReadySitHealthPct = 95; // a sitting member rests up to this much
constexpr std::uint64_t kPullHoldClearMs = 10000;  // the party must be ready this long in a row before a pull hold ends
constexpr std::uint32_t kRecoveryRisePct = 8;        // a rise counts as recovery when it is this much of the summed maximum within kRecoveryWindowMs (drinking gives about 14 % in 10 s, plain regeneration about 3 %)
constexpr std::uint64_t kRecoveryWindowMs = 10000;
constexpr std::uint64_t kPullHoldAbsMaxMs = 180000;  // 0.44.14: the hold is cut off here even while the party still recovers (the 45 s cap below counts from the last rise)
constexpr std::uint64_t kPullHoldMaxMs = 45000;     // the party never waits longer than this for itself
constexpr std::uint32_t kTankRestPct = 80;          // a dungeon party leader (the tank) below this health pulls nothing until it is back above, up to kPullHoldAbsMaxMs


// Recovery tracking for the mana and pull waits: true when the summed resource `sum` (of `alive` living members, `maxSum` at most) has risen by
// kRecoveryRisePct of maxSum since the baseline, which is re-taken every kRecoveryWindowMs, whenever it falls, and whenever the number of living
// members changes (a death lowers the sum and a revive raises it, neither is recovery). Zero `baseMs` means no baseline yet.
inline bool RecoveryProgress(std::uint64_t sum, std::uint64_t maxSum, std::uint32_t alive, std::uint64_t nowMs, std::uint64_t& baseSum,
                             std::uint64_t& baseMs, std::uint32_t& baseAlive)
{
    if (baseMs == 0 || alive != baseAlive || sum < baseSum)
    {
        baseSum = sum, baseMs = nowMs, baseAlive = alive;
        return false;
    }
    if (sum >= baseSum + maxSum * kRecoveryRisePct / 100)
    {
        baseSum = sum, baseMs = nowMs;
        return true;
    }
    if (nowMs - baseMs >= kRecoveryWindowMs)
        baseSum = sum, baseMs = nowMs;
    return false;
}constexpr float kRegroupClearYards = 20.0f;               // a stuck member counts as back only inside this (hysteresis: it trails the leader at about kRouteWaitYards)
constexpr std::uint64_t kRegroupStuckMs = 10000;    // a member out of range this long is stuck: the party stops waiting for it (regroup, 0.35.0)
constexpr std::uint64_t kConsumeLogMs = 30000;     // consume_check: one line per held bot per 30 s
constexpr std::uint64_t kRegroupLogMs = 15000;      // a regroup logs where every member stands this often (regroup_members, 0.44.5)
constexpr std::uint64_t kTankHeadStartMs = 2500;    // the others join a fight this long after the tank started it, so the tank has the threat
constexpr std::uint64_t kThreatDropRetryMs = 20000;  // a bot tries its threat-drop ability (Fade, Feign Death, Feint, Cower) at most this often
// Healing: a healer class heals the party member with the lowest health below this share, within this range.
constexpr std::uint32_t kHealBelowPct = 60;
// The tank (the party leader) is kept alive first while it holds a target, even a little below this.
// Emergency fast heal threshold: Flash Heal / Lesser Heal for someone in danger
constexpr std::uint32_t kEmergencyHealBelowPct = 20;
// Power Word: Shield preemptive threshold: shield a member that has taken damage recently and is under this
constexpr std::uint32_t kShieldBelowPct = 50;
// A healer of a dungeon party must drink at this mana level (out of combat).
constexpr std::uint32_t kDungeonDrinkBelowPct = 50;  // drink at 50% mana (was 30%)
// A healer of a dungeon party drinks up to this.
constexpr std::uint32_t kDungeonDrinkUpToPct = 95;   // drink up to 95% (was 85%)
// How long a heal claim on a member lasts: one global cooldown and the cast, so a second healer looks elsewhere meanwhile.
constexpr std::uint64_t kHealClaimMs = 2500;
// How long an interrupt claim on a hostile lasts: one global cooldown and the cast, the same reasoning as a heal claim. A kick
// that actually lands ends the cast, so Cond::TargetCasting stops being true on its own well before the claim would run out;
// this only covers the gap between the bot sending the cast and the next snapshot showing the result.
constexpr std::uint64_t kInterruptClaimMs = 2500;
// A cast with less than this left (NearbyUnit::cast_remaining_ms, Bot.Snapshot.CastInfo) ends before an interrupt sent now lands (a tick plus the
// core's latency): the row is skipped and the cooldown kept for the next cast. 0 left means "not reported", never "about to end".
constexpr std::uint32_t kInterruptMinRemainingMs = 300;
constexpr float kHealRangeYards = 30.0f;
constexpr float kBuffRangeYards = 30.0f;            // a party buff reaches members this close
constexpr std::size_t kGrindMemory = 2;           // grind spots remembered as just visited
constexpr std::size_t kGrindPickAmong = 4;        // with randomness on, a bot takes one of this many of the best grind spots, not always the first
constexpr double kStandJitterYards = 6.0;         // with randomness on, a bot stands up to this far from the exact point of its spot
constexpr std::uint32_t kAcceptRefusalsMax = 3;       // resurrect answers refused this often in a row: the offer is left alone for a while
constexpr std::uint64_t kAcceptIgnoreMs = 60000;
constexpr std::uint64_t kFightStallMs = 45000;       // a fight in which neither side's health went down this long is given up (the telemetry calls 60 s "stuck")
constexpr std::uint64_t kStallAvoidMs = 300000;      // a target the fight stalled against is left alone 5 minutes, 10, 20, then 40 at most
constexpr std::uint32_t kMaxStallDoublings = 3;
// Pull churn watchdog (2026-11-12 sfk run: a leader started a fight 8300 times in an hour on two mobs on a lower floor, never advancing, so the
// fight watchdog, which needs one target for kFightStallMs, never fired): this many fights started within the window, inside the radius,
// with no kill, and the targets are left alone as for a stalled fight.
constexpr std::uint32_t kPullChurnCount = 8;
constexpr std::uint64_t kPullChurnWindowMs = 60000;
constexpr float kPullChurnYards = 12.0f;
constexpr std::uint32_t kRestManaBelowPct = 40;      // an idle mana user below this sits down to drink in the open
constexpr std::uint32_t kRestManaUntilPct = 90;      // and stays sat until it has this much
constexpr std::uint64_t kManaRestMaxMs = 120000;     // but never longer than this at a time
constexpr std::uint64_t kManaRestBlockMs = 300000;   // and not again for this long after a rest that did not refill the mana
constexpr std::uint32_t kHubFailuresMax = 2;         // hub walks in a row that failed before the bot explores on foot instead
constexpr std::uint64_t kStuckCheckMs = 300000;       // every 5 minutes a bot that has not moved reports why it is not doing anything
constexpr std::uint64_t kGrindRetryMs = 60000;        // a bot that found no grind spot explores on foot this long before it looks again
constexpr std::uint64_t kExploreIdleMs = 10000;      // an idle bot with nowhere to go takes a short walk after this long
constexpr std::uint64_t kExploreTimeoutMs = 60000;
constexpr std::uint32_t kExploreRetryRefusals = 2; // refused explore points retried nearer (half, then a quarter of the radius) before the bot waits again
constexpr float kExploreReachYards = 6.0f;
constexpr std::uint64_t kAcceptInviteRetryMs = 3000; // an invite from a player is answered again after this long if it is still pending
constexpr std::uint64_t kGotoTimeoutMs = 90000;     // a goto order is dropped after this long
constexpr std::uint64_t kPullOrderMs = 30000;      // a pull order is dropped after this long, or when the target is gone
constexpr std::uint64_t kCloserAfterLosMs = 5000;   // after a line-of-sight refusal the bot closes to melee range for this long
// Gear a bot carries (traded, looted, a quest reward) is worn when it is better. The core compares item levels; asking is cheap, so every
// bot asks this often, spread by guid so a few hundred bots do not all ask in one tick.
constexpr std::uint64_t kBagGearEveryMs = 300000;
constexpr std::uint64_t kBagGearSpreadMs = 60000;
constexpr std::uint64_t kTravelTimeoutMs = 180000; // a walk to the vendor longer than this is dropped for now
constexpr float kTrainReachYards = 5.0f;          // a bot this close to its errand trainer sends the train order (the core allows about 5)
constexpr std::uint64_t kErrandTrainRetryMs = 3000; // the next train order of an errand, while the core has not answered
constexpr std::uint32_t kErrandTrainRefusalMax = 3; // train refusals in a row that end the errand as failed
constexpr std::uint64_t kAvoidKeepMs = 3000;          // a walk out of a ground effect keeps its end point this long
constexpr float kMountMinDistanceYards = 100.0f;   // a walk this long: mount up before it
constexpr float kMountDismountDistanceYards = 15.0f; // arrived or close: dismount
// A quest walk that keeps the bot within kQuestWalkStuckYards for kQuestWalkStuckMs has no route to its giver (seen on the sim: 11
// level 1 bots idle for hours, 110 to 8000 yards from the giver). With QuestTeleport the bot is sent there once; a refusal, or a second
// stuck period, drops the start (kNoRouteBlockMs) so the bot grinds. With QuestTeleport off the start is dropped at once: the core
// refuses the teleport while Bot.Actions.Teleport is 0, and retrying it never ends the walk.
constexpr std::uint64_t kQuestWalkStuckMs = 120000;
constexpr float kQuestWalkStuckYards = 5.0f;
// Death and recovery. Spell ids and levels checked against the fork's SpellLevels.db2: Resurrection 2006 level 10,
// Redemption 7328 level 12, Ancestral Spirit 2008 level 12, Rebirth 20484 level 20.
constexpr std::uint32_t kResurrection = 2006;
constexpr std::uint32_t kRedemption = 7328;
constexpr std::uint32_t kAncestralSpirit = 2008;
// Priest fast heals and shield (the core upranks these to the highest known rank).
constexpr std::uint32_t kFlashHeal = 2061;       // rank 1, upranked by HighestKnownRank
constexpr std::uint32_t kLesserHeal = 2050;      // rank 1, upranked
constexpr std::uint32_t kPowerWordShield = 17;   // rank 1, upranked (30 s duration, self or ally)
// The Holy priest of the Wowhead level 30 guide (docs/102 in forever-ops): Renew on the tank, Binding Heal when the priest and the tank are both hurt, Prayer of Healing only
// when most of the party is low. Ids are the first ranks of the client data; the core casts the highest rank the bot knows, and a spell it has not learned is refused and left alone for a minute.
constexpr std::uint32_t kRenew = 139;                // level 8
constexpr std::uint32_t kBindingHeal = 1240770;      // level 32 (the first rank a trainer teaches; later ranks to 1240774)
constexpr std::uint32_t kPrayerOfHealing = 596;      // level 30
constexpr std::uint32_t kRenewLevel = 8;
constexpr std::uint32_t kBindingHealLevel = 32;
constexpr std::uint32_t kPrayerOfHealingLevel = 30;
constexpr std::uint64_t kShieldLockMs = 15000;       // Weakened Soul: the same member cannot take another Power Word: Shield for 15 seconds
constexpr std::uint64_t kRenewMs = 15000;            // the Renew on the tank is cast again when it has run out
constexpr std::uint32_t kPrayerBelowPct = 50;        // a member this low counts toward Prayer of Healing
constexpr std::uint32_t kPrayerMinLow = 3;           // this many members low: Prayer of Healing instead of single heals
constexpr std::uint32_t kBindingBelowPct = 70;       // Binding Heal when both the priest and the tank are below this
constexpr std::uint32_t kWarriorClass_ = 1;
constexpr std::uint32_t kDruidClass_ = 11;
constexpr float kRezRangeYards = 30.0f;              // a rezzer resurrects a corpse within this range
constexpr std::uint32_t kRezManaPct = 35;            // and starts the 10 s cast with at least this much mana
constexpr std::uint64_t kRezCastMs = 11000;          // after a resurrect cast started, the same target is left alone this long
constexpr std::uint64_t kRezRetryMs = 5000;          // after a refusal, this long
constexpr std::uint32_t kRezRefusalsLogged = 3;      // refusals in a row before rez_refused is logged
constexpr std::uint64_t kReleaseRetryMs = 3000;      // a release not yet seen to work is not sent again before this
constexpr std::uint64_t kAcceptRetryMs = 2000;
// Deadmines Foundry (Gilnid): entries checked against this fork's creature_template/smart_scripts (not hardcoded from memory).
constexpr std::uint32_t kGoblinEngineerEntry = 622;           // casts Summon Remote-Controlled Golem (spell 3605, interruptible)
constexpr std::uint32_t kRemoteControlledGolemEntry = 2520;   // the add it summons: immune to most abilities, despawns when its engineer dies
// Ragefire Chasm, the Searing Blade camp (entries checked against this fork's world database on 2026-10-28): the Warlock (11324) summons a Voidwalker Minion
// (8996) once, out of combat (spell 12746, a guardian owned by the Warlock). That minion is level 47 (the pack is 13 to 15; creature_classic_level) with
// 1592 health, 2776 armor and 110 melee damage per hit, against the Enforcer's 900 health, 619 armor and 40: a party of level 18 bots needs several times as
// long to kill it as to kill any other mob of the camp, and it kills the tank. It is a guardian, so it is unsummoned the moment its owner dies
// (Unit::RemoveAllControlled in the core's setDeathState). Killing the Warlock is therefore the cheapest way to end it. But the Warlock stands back (it
// casts Shadow Bolt from range) while the fight is at the tank's target, so the 12 yard pack reach (kFocusPackYards) rarely offered it: the 2026-10-27/28
// journal has the focus event on the Voidwalker 295 times and on the Warlock 66.
constexpr std::uint32_t kSearingBladeWarlockEntry = 11324;
constexpr std::uint32_t kVoidwalkerMinionEntry = 8996;
constexpr float kSummonerReachYards = 35.0f; // how far from the tank's target the Warlock is still taken first (the snapshot's nearby range is 40)
constexpr std::uint64_t kReclaimRetryMs = 3000;
constexpr std::uint64_t kDeathLoopMs = 420000;         // three deaths within this time are a death loop: the bot is moved to a grind spot for its level (prod: a bot dying every ~62 s, so three deaths span ~125 s)
constexpr std::uint64_t kDeathLoopCooldownMs = 120000; // after an escape the bot is not moved again before this
constexpr std::uint32_t kRecoverPullPct = 75;           // a solo bot out of combat below this health starts no fight and no quest walk: it backs off and rests first (prod 2026-11-12: bots revived at 49% pulled the creature that had killed them, 812 deaths in 90 minutes)
constexpr std::uint64_t kRecoverHoldMs = 150000;       // ... for at most this long, so a bot that cannot regenerate does not stand for good
constexpr float kRecoverSafeYards = 30.0f;              // ... and it sits down only with no live creature this close (it walks away from one that is)
constexpr float kDeathLoopAwayYards = 400.0f;           // ... one at least this far from where it keeps dying
constexpr std::uint64_t kCorpseTeleportRetryMs = 15000; // a ghost off its corpse's map with no portal is teleported to the corpse, not again before this
constexpr std::uint64_t kOutOfCombatReviveRetryMs = 10000; // a ghost of a dungeon party is revived by its leader out of combat, not again before this
constexpr std::uint64_t kCorpseReviveDelayMs = 8000;       // the Recover tactic: a dead member of a dungeon party whose leader is up and quiet is revived by the leader this long after the fight ended
constexpr float kReviveHostileYards = 30.0f;               // revive-loop guard: no corpse revive next to a member while a live hostile is this close to it (the stockade, rfd and gnomer 2026-11-07 runs revived a bot every 8 s beside its killer: 106-127 deaths)
constexpr std::uint64_t kReviveWindowMs = 120000;          // and at most kReviveWindowMax (the three tries of one death) corpse revives per bot in this window
constexpr std::uint32_t kReviveWindowMax = 3;
constexpr std::uint64_t kGhostHoldLogMs = 10000;           // ghost_teleport_hold: one line per bot in this time
constexpr std::uint32_t kOutOfCombatReviveTries = 3;       // and after this many tries the ghost walks back to its corpse as before
constexpr std::uint64_t kLeaderGhostWaitMs = 60000;        // DungeonLeaderRejoin: a dead leader's ghost waits this long (from its release) for its party's fight to end, then goes to the entrance as before
constexpr std::uint64_t kDungeonEntryWaitMs = 90000;       // a ghost of a dungeon party waits this long at the instance entrance for its revive, then walks to its corpse as before
constexpr std::uint64_t kEntryTeleportRetryMs = 5000;      // the next try of the teleport to the entrance, while it is refused or the ghost is not there yet
constexpr float kEntryYards = 15.0f;                        // a ghost this close to the entrance is at it
constexpr std::uint64_t kLeaderRecoverCombatMs = 15000;   // a leader at the entrance this long goes to a member who is in a fight too (0.44.8)
constexpr std::uint32_t kLeaderRecoverMinHealthPct = 60;    // 0.44.17: a revived leader at the entrance goes to its party at this health (never below: tanks that came in at 30 % died again) when nobody of the party is in a fight
constexpr std::uint64_t kLeaderRecoverMaxWaitMs = 600000;  // a revived leader rests at the entrance this long at most before it goes to its party (kRouteMinHealthPct first; 120 s sent a tank in at 49 %: 275 of 568 deaths in the 2026-11-08 sim followed a pull at 40-59 %)
// A ghost cannot be attacked; the body it reclaims can: it comes back alive at about half health, on the spot where it died, next to whatever killed it (on the sim a tank was
// killed seven times in a row by the same scout, each time minutes after its reclaim). A ghost therefore waits, up to this long, while a live hostile stands this close to it.
constexpr float kReclaimDangerYards = 30.0f;
constexpr std::uint64_t kReclaimDangerWaitMs = 120000;
constexpr float kReclaimYards = 35.0f;
// Protocol 28: a ghost that would have to reclaim next to a live hostile takes the spirit healer instead (kResurrect spell_id 2: 50% health, resurrection sickness,
// durability loss, but at the graveyard and not next to the camping mob). SpiritHealerAfterMs is how long the hostile must have stood near the corpse; a second death
// to the same creature entry within kSpiritHealerRepeatWindowMs takes it at once. A refused or failed answer sends the ghost back to the old hold and reclaim for
// kSpiritHealerRefusedMs.
constexpr std::uint64_t kSpiritHealerRepeatWindowMs = 300000;
constexpr std::uint64_t kSpiritHealerRetryMs = 3000;
constexpr std::uint64_t kSpiritHealerRefusedMs = 120000;
constexpr std::uint64_t kHealDownrankPauseMs = 600000;   // after a kNotKnown for an exact heal rank (the level said the bot has it, the core says it has not)
constexpr std::uint32_t kSpiritHealerSpell = 2;     // kResurrect spell_id: take the spirit healer
// The pure rule: does a ghost at its corpse, with a live hostile near it since dangerSinceMs (0 = none), take the spirit healer now?
inline bool SpiritHealerWanted(std::uint64_t afterMs, bool onRepeat, std::uint64_t nowMs, std::uint64_t dangerSinceMs, bool killerRepeat,
                               std::uint64_t previousDeathMs, std::uint64_t lastDeathMs, std::uint64_t refusedUntilMs)
{
    if (dangerSinceMs == 0 || nowMs < refusedUntilMs)
        return false;
    if (afterMs != 0 && nowMs >= dangerSinceMs && nowMs - dangerSinceMs >= afterMs)
        return true;
    return onRepeat && killerRepeat && previousDeathMs != 0 && lastDeathMs >= previousDeathMs && lastDeathMs - previousDeathMs <= kSpiritHealerRepeatWindowMs;
}               // the core allows 39 yards; the ghost reclaims from this close

// A cast or channel is in progress. cast_remaining_ms is no test: it is 0 while a spell is in flight (a Fireball after the cast, state
// "delayed"), which the core still counts as casting and refuses every other cast for ("bot is casting spell N").
bool IsCasting(BotSnapshot const& bot);

// Spells with a cast time (or a channel), which cannot be started while moving. The core's refusals add to the bot's own list, so a
// spell missing here is learned after its first refusal.
bool KnownCastTimeSpell(std::uint32_t spell);

// The spell a healer class heals an ally with (rank 1 id, or the next spell once the bot has the level), 0 for a class that does not heal.
std::uint32_t HealSpellFor(std::uint32_t classId, std::uint32_t level);

// The spell a class resurrects a dead ally with, 0 for a class that cannot or whose level is too low. A druid's Rebirth (20484, level 20)
// needs a reagent the bots are not given, so a druid is not a rezzer. A level of 0 is unknown and not judged.
std::uint32_t ResurrectSpellFor(std::uint32_t classId, std::uint32_t level);

bool IsResurrectSpell(std::uint32_t spell);

// The creature type the cc tactic needs (world database's creature_template.type) to pick a spell that can affect a pack member.
// The snapshot has no such field (forever_module::NearbyUnit, tier2_layout.h): the core does not report a unit's creature type at
// all, so this is a small built-in table of the entries the two shipped dungeon files can put in a tank's pack (data/dungeons/36-
// the-deadmines.json and 389-ragefire-chasm.json, kill_priority and bosses; verified against creature_template on 2026-10-27, the
// Forever test world database). An entry not listed here is of unknown type to the policy, so a type-restricted cc spell is never
// tried on it: a wider table for a dungeon added later belongs here, the same way a new dungeon's kill_priority needs its own list.
enum class CreatureType : std::uint8_t { kUnknown = 0, kHumanoid, kDemon, kElemental, kMechanical };

CreatureType CreatureTypeOf(std::uint32_t entry);

// The cc spell a class has at all, whatever the target: 0 for a class with none. Sap (rogue), Polymorph (mage), Banish (warlock);
// no other class has a single-target hold in this module's scope today (a priest's Shackle Undead, a hunter's Freezing Trap and a
// druid's Entangling Roots are not: this change is scoped to the three the task names).
std::uint32_t CcSpellForClass(std::uint32_t classId);
// The class's dispel spell for a kind (1 magic, 2 curse, 3 disease, 4 poison; the SDK's dispel_type), 0 when the class has none at that level (level 0 = unknown: any).
std::uint32_t DispelSpellFor(std::uint32_t classId, std::uint32_t level, std::uint8_t kind);
// The class's threat-drop spell (priest Fade, hunter Feign Death, rogue Feint, druid Cower), 0 when the class has none at this level. onTarget is set when the cast goes at the target.
std::uint32_t ThreatDropSpellFor(std::uint32_t classId, std::uint32_t level, bool& onTarget);

// The level the Forever trainers teach a cc spell at (trainer_spell, verified 2026-10-27 against the test world database; the same
// levels as this file's SpellCostFile dump, data/spell_costs.txt). Sap and Polymorph are both well below either shipped dungeon's
// spawn level (Deadmines 23, Ragefire Chasm 18); Banish is not reached by either (it needs 28) -- kept here regardless, since cc is a
// general, dungeon-wide feature and a later, higher-level dungeon with a Demon or Elemental pack can use it once its roster is.
std::uint32_t CcLearnLevel(std::uint32_t spell);

// Whether a cc spell can affect this creature type at all (classic's type restrictions: Sap and Polymorph are Humanoid only,
// Banish is Demon or Elemental only). kUnknown never matches: the policy does not try a type-restricted spell on a creature it
// cannot place (see CreatureTypeOf above).
bool CcAffects(std::uint32_t spell, CreatureType type);

// Layout 13: whether the bot carries this aura right now. The aura list replaces the module's own buff timers, so a buff
// another player cast over the bot, or one it lost to a death, is the fact it is. An aura with no duration reports a large
// remaining_ms and runs for ever; a reported aura with 0 left is one that just expired.
bool AuraRunning(BotSnapshot const& bot, std::uint32_t spell);
// True when the bot carries the aura of the spell or of any rank in `ranks` (a buff row's Ranks). The core casts the highest rank known, so the aura id is that rank's.
bool AuraRunning(BotSnapshot const& bot, std::uint32_t spell, std::vector<std::uint32_t> const& ranks);

// Layout 13: the bot's pet, from the snapshot, instead of what refused casts imply. pet_state is 0 none, 1 alive, 2 dead.
// A core that does not report pets leaves it 0, and the inference from refused casts below is the fallback.
enum class PetKnown : std::uint8_t { kNone, kOut, kDead };

PetKnown PetOf(BotSnapshot const& bot);

// Duration of the damage-over-time effect a spell leaves on its target, or 0 for spells the policy does not track. Rank 1
// durations: while the effect runs, the policy does not recast the spell on the same target.
std::uint64_t DotDurationMs(std::uint32_t spell);

float DistanceBetween(NearbyUnit const& a, NearbyUnit const& b);


// The level difference at which a creature gives no experience at all (the classic "grey" rule: GetZeroDifference). Whether this fork
// changes it is not known; the vanilla table is used.
std::uint32_t ZeroExpDifference(std::uint32_t level);

// A creature far enough below the bot that killing it gives no experience: not worth pulling. (A level of 0 is unknown and not judged.)
bool IsGrey(BotSnapshot const& bot, NearbyUnit const& unit);

// A unit's share of its health (0..1), or 1 when its maximum is unknown. A pull prefers the weakest, so a kill comes sooner.
float HealthShare(NearbyUnit const& unit);

// The live hostile this bot should pull, or nullptr. A group is the hostiles within kPullRadius of the candidate, the candidate
// included. Units already attacking the bot are not chosen here; the policy fights its threat target whatever the group size.
// The weakest target wins and the closest breaks the tie: finishing one creature before the next is more experience per hour
// than walking to whichever one happens to stand nearest first.
// A creature standing high above or far below the bot while almost on top of it in the plane (a patrol on a wall walk, a ledge, the floor above) cannot be
// walked to from here: the flat distance says "in reach" while the way there is somewhere else, so the bot stands under it, its follow order finds no path
// and its spells have no line of sight. The 2026-10-28 soak: the Tormented Officers (entry 3873) patrol 20 yards above Shadowfang Keep's courtyard, the
// route's first point is right under them, and 34 stuck episodes (about 77 bot-minutes, fights of up to 12 minutes) all came from pulling them.
bool OnOtherLevel(BotSnapshot const& bot, NearbyUnit const& unit);

// `leashed` (a dungeon party's leash, DungeonPullLeashYards / DungeonChaseLeashYards; empty = none): a candidate it says is outside is not chosen, defending or not.
NearbyUnit const* PullTarget(BotSnapshot const& bot, std::span<NearbyUnit const> units,
                            std::function<bool(std::uint64_t)> const& avoided, bool defending,
                            std::vector<std::uint32_t> const& onlyEntries = {}, bool dungeon = false,
                            std::function<bool(NearbyUnit const&)> const& leashed = {});

// The flat distance from (x, y) to the leg of a dungeon route its leader is walking: the segment from point index - 1 to point index (at index 0, from `start`, the
// instance entry, when known, else point 0 alone). Infinity for an empty route or an index past its end (the boss sweep after the route goes anywhere).
float RouteLegDistance(std::vector<Point> const& route, std::size_t index, std::optional<Point> const& start, float x, float y);

// The closest unit in `units` that satisfies `wanted`, or nullptr.
NearbyUnit const* Closest(BotSnapshot const& bot, std::span<NearbyUnit const> units, bool (*wanted)(NearbyUnit const&));

// A live creature the bot may pick as its own target. Players are never picked: bots of the other faction would otherwise
// fight each other, and a player that attacks the bot still comes in as the threat target.
bool IsLiveMob(NearbyUnit const& unit);

// A friendly player the bot may follow. A hostile player is not followed: it would lead the bot into a fight.
bool IsPlayer(NearbyUnit const& unit);

// Avoids a unit for kAvoidMs, doubled for each earlier time it was avoided, so a unit that keeps coming back (a trainer the
// bot cannot attack) is left alone for longer each time.
void AvoidUnit(std::unordered_map<std::uint64_t, std::uint64_t>& avoidUntil, std::unordered_map<std::uint64_t, std::uint32_t>& count,
               std::uint64_t ref, std::uint64_t nowMs);

// A stalled fight (nothing lost on either side): the target is left alone much longer than a refused one, more each time, because it is
// out of reach or bugged, and the bot has nothing to gain by coming back to it every two minutes (a mage did that for an hour and a half).
void AvoidStalledUnit(std::unordered_map<std::uint64_t, std::uint64_t>& avoidUntil, std::unordered_map<std::uint64_t, std::uint32_t>& count,
                      std::uint64_t ref, std::uint64_t nowMs);

// Notes a fight start at (x, y) on `target` in the churn window. True once kPullChurnCount fights started within kPullChurnWindowMs and
// kPullChurnYards: `targets` then holds the distinct targets to leave alone and the window is cleared. A start outside the radius or after
// the window begins a new window.
bool NotePullChurn(std::uint64_t& sinceMs, std::uint32_t& count, float& windowX, float& windowY, std::vector<std::uint64_t>& targets,
                   std::uint64_t nowMs, float x, float y, std::uint64_t target);

// The live creature in `units` with this reference, or nullptr.
NearbyUnit const* FindLiveMob(std::span<NearbyUnit const> units, std::uint64_t guid);

// The point the kite goes to. The module has no navmesh or path query of its own (the core finds the path, and refuses a move with "no route"), so a point on a raw circle
// round the Warder may be a wall: 5,800 such refusals on 2026-10-29. When the dungeon's entry is known, retry 0 is therefore the retreat along the entry route: the
// point kAoeStandYards from the Warder on the line toward the entry the party walked in by, on the bot's own bearing (a few degrees either side, so a raid does not queue
// up on one point). Only when the core refuses that point (or the entry is unknown or nearer than the stand distance) does the circle come: the point kAoeStandYards from
// the Warder on the bot's own bearing from it, turned round by retry steps (0, +1, -1, +2, -2 times kAoeKiteRetryDegrees).
Point AoeKitePoint(BotSnapshot const& bot, NearbyUnit const& warder, int retry, std::optional<Point> const& entry = std::nullopt);

// The ground effects that harm a bot on this map: the snapshot's hostile ones, the avoid table's spells, the dungeon's own spells, and its zones. The same circles as the
// walk out of a harmful area (AvoidGroundEffects); a stand point inside one of them is not a place to move to.
std::vector<ground_avoid::Circle> HazardCircles(PolicyOptions const& options, BotSnapshot const& bot);

// A stand point `yards` from the target, on the bot's own bearing from it. A point a hazard covers is not moved to: the bearing is turned round by up to four steps
// of kBandStepDegrees either side (0, +1, -1, +2, ...), and nullopt when every step is covered.
std::optional<Point> BandStandPoint(BotSnapshot const& bot, NearbyUnit const& target, float yards, std::vector<ground_avoid::Circle> const& hazards);

// A player's unit reference, from its character guid counter (the same reference the core's party actions take).
std::uint64_t PlayerRef(std::uint64_t counter);

float Distance2d(BotSnapshot const& a, BotSnapshot const& b);

float DistanceTo(BotSnapshot const& bot, Point const& point);

// The class of a bot: its own from the class table, else the configured class (0 = none).
std::uint32_t ClassFor(PolicyOptions const& options, std::uint64_t guid);

// A hunter whose ranged weapon uses ammunition and who carries none (layout 15: the core fills ammo_entry and ammo_count).
bool NoAmmo(BotSnapshot const& bot, std::uint32_t classId);

// False when the cost file says the spell costs more power than the bot has now: the core would refuse the cast, so it is not tried. A spell the file
// does not price (no file, an unknown spell, a percent cost) or a cost in another power than the bot's main one (a druid in a form) is always tried.
bool CanPay(spell_cost::SpellCostData const* costs, std::uint32_t spell, BotSnapshot const& bot);

// The snapshot facts the supply logic reads (supply_policy.h).
supply::Facts FactsOf(BotSnapshot const& bot);

// Whether `value` is below `pct` percent of `max` (the comparison is done in 64 bits, no division).
inline bool BelowPct(std::uint32_t value, std::uint32_t max, std::uint32_t pct)
{
    return std::uint64_t(value) * 100 < std::uint64_t(max) * pct;
}

} // namespace bot_policy_detail

using namespace bot_policy_detail;

// Decide's shared state (bot_policy.h: DecideBot and the Turn* steps). DecideTick is what every bot's turn of one Decide call shares: the view, the
// clock, the actions sent, and the parties. ForParty is a template, so it is defined here.
struct BotPolicy::DecideTick
{
    WorldTickSnapshotView const& view;
    std::uint64_t nowMs;
    std::vector<BotAction>& actions;
    std::unordered_set<std::uint64_t>& present;
    std::unordered_map<std::uint64_t, std::size_t> const& indexOf;            // bot guid -> index in view.bots
    std::unordered_map<std::uint64_t, std::uint64_t>& leaderOf;                // bot guid -> its party's leader
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>>& membersOf;  // leader guid -> the other members
    std::unordered_set<std::uint64_t> const& tankLed;                         // the tanks leading their group ("tank lead")
    std::unordered_map<std::uint64_t, party_planner::Order> const* partyOrders = nullptr; // each party's plan (null with the planner off)

    party_planner::Order const* PlanOf(std::uint64_t leaderGuid) const
    {
        if (!partyOrders)
            return nullptr;
        auto const at = partyOrders->find(leaderGuid);
        return at != partyOrders->end() ? &at->second : nullptr;
    }

    // Layout 25: the core's shared view of a bot's group (null when the bot has none, the core's Bot.Snapshot.PartyView is off, or the bot is not in
    // this tick's view). The planner adapter (party_view_adapter.h) turns it into planner inputs; nothing reads it yet.
    forever_module::PartyView const* PartyViewOf(std::uint64_t botGuid) const
    {
        auto const at = indexOf.find(botGuid);
        return at != indexOf.end() ? view.PartyViewOf(at->second) : nullptr;
    }

    // The nearby list of a party's leader when it is online and on the given map, else empty. A member's own list (16 slots: 4 players, then the closest creatures)
    // can be full of the raid's pets and totems, so a Warder the tank is fighting a few yards away is not in it (2026-11-08 20:03-20:05: 39 members logged no
    // warder_near while the tank fought one, and none assisted it).
    std::span<NearbyUnit const> LeaderNearby(std::uint64_t leaderGuid, std::uint32_t mapId) const
    {
        if (leaderGuid == 0)
            return {};
        auto const at = indexOf.find(leaderGuid);
        if (at == indexOf.end() || view.bots[at->second].map_id != mapId)
            return {};
        return view.NearbyOf(at->second);
    }

    // A leader or inviter reference that is one of this module's bots, as against a human player (both are players to the core).
    bool IsOurBot(std::uint64_t ref) const;

    // The members of a leader's party that are online (not the leader).
    template <typename Fn>
    void ForEachPartyMember(std::uint64_t leaderGuid, Fn&& fn) const
    {
        auto const members = membersOf.find(leaderGuid);
        if (members == membersOf.end())
            return;
        for (std::uint64_t member : members->second)
            if (auto const at = indexOf.find(member); at != indexOf.end())
                fn(view.bots[at->second]);
    }

    // The party of a leader: the leader and its members that are online.
    template <typename Fn>
    void ForParty(std::uint64_t leaderGuid, Fn&& fn) const
    {
        if (auto const at = indexOf.find(leaderGuid); at != indexOf.end())
            fn(view.bots[at->second]);
        auto const members = membersOf.find(leaderGuid);
        if (members == membersOf.end())
            return;
        for (std::uint64_t member : members->second)
            if (auto const at = indexOf.find(member); at != indexOf.end())
                fn(view.bots[at->second]);
    }
};

// One bot's turn: the bot, its state, and what one step works out for the steps after it (the locals of Decide's per-bot loop, in the order that
// loop declared them). A field is set by the step named beside it before any later step reads it.
struct BotPolicy::BotTurn
{
    std::size_t const i;                                  // the bot's index in the view
    BotSnapshot const& bot;
    BotState& state;
    std::uint64_t const nowMs;

    // Whether the bot leaves this unit alone for now (its AvoidUntilMs), and the same as a function for PullTarget.
    bool Avoided(std::uint64_t ref) const;
    std::function<bool(std::uint64_t)> AvoidedFn() const;

    bool stagingHold = false;                             // TurnBegin
    std::span<NearbyUnit const> nearby{};                 // TurnTargetMemory
    std::uint64_t deadParty = 0;
    bool pullRest = false;                              // TurnTargeting: a hurt member of a route party (PullMinHpPct) that rests instead of fighting or following
    bool hurtRest = false;                              // TurnRevivedAndRoles: hurt and solo, out of combat: rests before it fights (kRecoverPullPct)
    bool lowHealth = false;                               // TurnRevivedAndRoles
    std::uint32_t classId = 0;
    std::string spec{};
    std::uint64_t partyLeader = 0;
    bool partyLeaderBot = false;
    bool partyMember = false;
    bool routeOn = false;
    PolicyOptions::ScenarioRoute const* scenario = nullptr;
    bool returningToDungeon = false;
    std::uint64_t leaderRef = 0;
    BotSnapshot const* leaderSnap = nullptr;
    std::uint64_t followRef = 0;
    std::uint8_t deadWaitKind = 0;
    QuestStep questStep{};                                // TurnQuestsAndHold
    bool routeBusy = false;
    std::vector<std::uint32_t> const noEntries{};
    std::vector<std::uint32_t> const* huntEntries = nullptr;
    bool dungeonHold = false;
    bool tankHeadStart = false;
    NearbyUnit const* targetUnit = nullptr;               // TurnTargeting
    std::uint64_t targetRef = 0;
    bool noSelfPull = false;
    bool hasTarget = false;                               // TurnTaming
    float distance = 0.0f;                                // TurnFightWatch
    Decision wanted{};                                    // TurnMoveSetup
    char const* moveWhy = "";
    int kiteRetry = -1;
    NearbyUnit const* aoeWarder = nullptr;
    std::optional<Point> healLosPoint{};
    std::optional<Point> kiteEntry{};
    bool enemyNear = false;                               // TurnIdle
    std::size_t actionsAtStart = 0;                       // the tick's action count when this bot's turn began (DecideBot)
    bool transportHold = false;                           // TurnTransport: a passenger (or one about to board): no world-coordinate move
    bool idle = false;
    supply::Facts supplyFacts{};                          // TurnShopping
    bool manaUser = false;                                // TurnWander
    bool manaResting = false;
    bool healthy = false;
    bool grindMode = false;
};

// The dungeon route leader's walk in one tick (MoveWithoutTarget and the Route* parts, bot_policy_turn_move.cpp): the route, its boss points and
// events, the bosses dead, and what one part works out for the next.
struct BotPolicy::RouteLeg
{
    std::vector<Point> const* routePoints;
    std::map<std::size_t, std::uint32_t> const* routeBoss;
    std::vector<dungeon_events::Event> const* routeEvents;
    std::set<std::uint32_t> const* killedNow;
    bool together = true;                                 // RouteReadiness
    bool strong = false;
    bool manaReady = false;
    bool pullReady = true;                                // RouteReadiness: the ready gate (members above PullMinHpPct and PullReadyManaPct, no wipe hold)
    bool recovering = false;                              // RouteReadiness, then RouteLeaderRecover
    bool eventBusy = false;                               // RouteEvents
};
