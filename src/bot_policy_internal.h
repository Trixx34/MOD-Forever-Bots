// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Private to bot_policy*.cpp: the helpers and tuning constants that were an anonymous namespace in bot_policy.cpp, now shared by its split files.
#include "bot_policy.h"

#include "forever_module/talent_spec.h"

#include "combat_rotation.h"

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

constexpr float kDisengagePct = 30.0f;       // below this health the bot stops fighting
constexpr float kRangedKiteYards = 12.0f;    // a ranged bot keeps a melee target this far away
// Fire Nova (spell 20203) reaches 20 yards from the Warder plus the target's combat reach (1.5), and 2 more while the Warder is moving (23.5 in all); the bot stands
// there at the centre-to-centre distance below. 21 yards (the first value) left every kiting bot inside the nova. A tuning value: 24-25 is the range to try.
constexpr float kAoeKiteYards = 24.5f;       // every party member other than the tank keeps an Onyxian Warder this far away
constexpr float kAoeKiteMarginYards = 5.5f;      // a party member also holds this much beyond kAoeKiteYards, and starts to kite before it is inside the radius, in combat or not
constexpr float kAoeStandYards = kAoeKiteYards + kAoeKiteMarginYards;   // 30: where a kited bot stands, so a step of the Warder or the bot does not put it back in the nova
constexpr std::uint32_t kOnyxiaMap = 249;              // Onyxia's Lair
constexpr float kOnyxiaEntryX = 30.89f, kOnyxiaEntryY = -54.08f, kOnyxiaEntryZ = -5.03f;   // the lair's entry, the way the party came in (known walkable)
constexpr float kAoeKiteSpreadDegrees = 4.0f;  // each bot takes its own bearing off the Warder, up to 5 steps of this either side of the straight line away
constexpr float kAoeKiteRetryDegrees = 30.0f;  // and a kite point the core found no route to is tried again this much further round
constexpr std::uint64_t kHealLosMoveMs = 4000; // after a heal refused for line of sight a healer may leave its stand-off to find the target, for this long
constexpr std::uint32_t kOnyxianWarderEntry = 12129;
constexpr std::uint32_t kOnyxiaBossEntry = 10184;   // Onyxia herself, the lair's last boss (data/dungeons/249-onyxias-lair.json)
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
constexpr float kReviveWarderYards = 25.0f;  // a ghost of a dungeon party is not brought back (entrance teleport, leader's revive, corpse reclaim) while a live Warder is this close to its corpse or to a living party member
constexpr std::uint64_t kReviveWarderLogMs = 30000;
constexpr float kFleeYards = 15.0f;          // below 30% health: flees while a threat is this close
constexpr std::uint64_t kGlobalCooldownMs = 1500;
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

// Whether the bot has the profession of a craft or gather line: First Aid and Cooking every bot may learn, the others are its two primary professions.
inline bool HasLine(BotSnapshot const& bot, std::uint32_t line)
{
    if (line == 129 || line == 185)
        return true;
    return (bot.profession_skill[0] == line && bot.profession_value[0] != 0) || (bot.profession_skill[1] == line && bot.profession_value[1] != 0);
}
constexpr std::uint32_t kServiceSellJunk = 1024;   // SDK 0.16: also sell white junk (trade goods, spare bags, miscellaneous items)
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
constexpr std::uint32_t kMaxComboPoints = 5;
// Saber Slash (Forever): like Sinister Strike it gives one combo point (SpellEffect 30 energize, power type 4, 1 point, in the client data).
constexpr std::uint32_t kSaberSlash = 424785;

// Mutilate (Assassination, level 40) gives two combo points and Hemorrhage (Subtlety, level 30) one: the builders of the other rogue specs (docs/102 in forever-ops).
constexpr std::uint32_t kMutilate = 399956;
constexpr std::uint32_t kHemorrhage = 16511;
constexpr std::uint32_t kRupture = 1943;
constexpr std::uint32_t kSliceAndDice = 5171;

// Combo points a rogue spell adds on its target when it lands, 0 for any other spell.
inline std::uint32_t ComboPointsGained(std::uint32_t spell)
{
    if (spell == kMutilate)
        return 2;
    return spell == kSinisterStrike || spell == kSaberSlash || spell == kHemorrhage ? 1 : 0;
}
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
constexpr std::uint64_t kBossHoldMs = 180000;     // how long the leader waits at a boss point for the boss to die before it goes on
// A member this far from its leader (2D) has lost it ... The same as kRouteWaitYards: the leader waits for a member farther than that, so a member that stays
// farther (stuck below a ramp the leader climbed) held the party for good when this was 60 (Ragefire Chasm and the Deadmines, 2026-10-26).
constexpr float kLostLeaderYards = 30.0f;
constexpr float kLostLeaderHeight = 25.0f;        // ... or this far above or below it (another level of the dungeon)
constexpr std::uint64_t kLostLeaderMs = 30000;    // for this long, then it is put next to the leader (DungeonTeleport). Longer than kRegroupStuckMs: the leader has
                                                  // flagged the member and walked on by then, so the teleport is not in the same second as the regroup_stuck line (0.36.2)
constexpr float kRouteFarYards = 150.0f;           // a leader this far from a waypoint it gets no route to is not at the point: it is put back on the route instead of skipping
constexpr std::uint32_t kRouteNoRouteSkip = 3;     // refusals in a row (no route) after which a dungeon waypoint is skipped
constexpr std::uint32_t kRepairBelowPct = 50;    // worn gear below this durability sends the bot to a repairman, not just any vendor
// The fights draw the leader past route points: when it is this close to the next point, the one it was heading for (not a boss point) is dropped, so it never walks back
// for it (it did, through the whole dungeon by the pathfinder's only route, with the party standing behind).
constexpr float kRouteSkipAheadYards = 25.0f;
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
constexpr float kRouteWaitYards = 30.0f;         // the leader waits for a member farther behind than this
constexpr std::uint32_t kRouteMinHealthPct = 80; // and for its own health to be at least this
constexpr std::uint32_t kRouteMinManaPct = 80;   // the leader walks on only when every mana user of the party has at least this much mana (the healer walked into fights at 3%)
constexpr std::uint64_t kRouteManaMaxMs = 150000; // and never waits longer than this for mana (no drink left, or a member that cannot sit)
constexpr std::uint32_t kRouteTopUpPct = 90;     // out of a fight, the party's healer heals the route leader up to this (above kRouteMinHealthPct, so it walks on)
// A leader walking the route that has not moved this far in this long, out of combat, has a move that went nowhere (sent once, no answer; dm#6 of 2026-10-26 stood
// 53 minutes so): the move is sent again and counted as a refusal (kRouteNoRouteSkip), so the point is recovered or skipped as for a "no route".
constexpr std::uint64_t kRouteStallMs = 30000;
constexpr float kRouteStallYards = 3.0f;
constexpr std::uint64_t kRouteHoldLogMs = 30000; // a leader that does not walk on says why (route_hold), at most this often
// Dungeon pull control: the tank (the party leader) pulls only when the party is ready, and the others wait for the tank to hold the target.
constexpr std::uint32_t kPartyReadyHealthOldPct = 80, kPartyReadyPowerOldPct = 60; // with the Rest tactic off
constexpr std::uint32_t kPartyReadyHealthPct = 90;  // every living member has at least this much health
constexpr std::uint32_t kPartyReadyPowerPct = 95;   // and every mana user at least this much mana (matches drink cap)
constexpr std::uint32_t kPartyReadySitHealthPct = 95; // a sitting member rests up to this much
constexpr std::uint64_t kPullHoldClearMs = 10000;  // the party must be ready this long in a row before a pull hold ends
constexpr std::uint64_t kPullHoldMaxMs = 120000;    // the party never waits longer than this for itself
constexpr float kRegroupClearYards = 20.0f;               // a stuck member counts as back only inside this (hysteresis: it trails the leader at about kRouteWaitYards)
constexpr std::uint64_t kRegroupStuckMs = 20000;    // a member out of range this long is stuck: the party stops waiting for it (regroup, 0.35.0)
constexpr std::uint64_t kTankHeadStartMs = 2500;    // the others join a fight this long after the tank started it, so the tank has the threat
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
constexpr std::uint32_t kRestManaBelowPct = 40;      // an idle mana user below this sits down to drink in the open
constexpr std::uint32_t kRestManaUntilPct = 90;      // and stays sat until it has this much
constexpr std::uint64_t kManaRestMaxMs = 120000;     // but never longer than this at a time
constexpr std::uint64_t kManaRestBlockMs = 300000;   // and not again for this long after a rest that did not refill the mana
constexpr std::uint32_t kHubFailuresMax = 2;         // hub walks in a row that failed before the bot explores on foot instead
constexpr std::uint64_t kStuckCheckMs = 300000;       // every 5 minutes a bot that has not moved reports why it is not doing anything
constexpr std::uint64_t kGrindRetryMs = 60000;        // a bot that found no grind spot explores on foot this long before it looks again
constexpr std::uint64_t kExploreIdleMs = 10000;      // an idle bot with nowhere to go takes a short walk after this long
constexpr std::uint64_t kExploreTimeoutMs = 60000;
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
constexpr std::uint64_t kDeathLoopMs = 120000;         // three deaths within this time are a death loop: the bot is moved to a grind spot for its level
constexpr float kDeathLoopAwayYards = 400.0f;           // ... one at least this far from where it keeps dying
constexpr std::uint64_t kCorpseTeleportRetryMs = 15000; // a ghost off its corpse's map with no portal is teleported to the corpse, not again before this
constexpr std::uint64_t kOutOfCombatReviveRetryMs = 10000; // a ghost of a dungeon party is revived by its leader out of combat, not again before this
constexpr std::uint64_t kCorpseReviveDelayMs = 8000;       // the Recover tactic: a dead member of a dungeon party whose leader is up and quiet is revived by the leader this long after the fight ended
constexpr std::uint32_t kOutOfCombatReviveTries = 3;       // and after this many tries the ghost walks back to its corpse as before
constexpr std::uint64_t kDungeonEntryWaitMs = 90000;       // a ghost of a dungeon party waits this long at the instance entrance for its revive, then walks to its corpse as before
constexpr std::uint64_t kEntryTeleportRetryMs = 5000;      // the next try of the teleport to the entrance, while it is refused or the ghost is not there yet
constexpr float kEntryYards = 15.0f;                        // a ghost this close to the entrance is at it
constexpr std::uint64_t kLeaderRecoverMaxWaitMs = 120000;  // a revived leader rests at the entrance this long at most before it goes to its party (kRouteMinHealthPct first)
// A ghost cannot be attacked; the body it reclaims can: it comes back alive at about half health, on the spot where it died, next to whatever killed it (on the sim a tank was
// killed seven times in a row by the same scout, each time minutes after its reclaim). A ghost therefore waits, up to this long, while a live hostile stands this close to it.
constexpr float kReclaimDangerYards = 30.0f;
constexpr std::uint64_t kReclaimDangerWaitMs = 120000;
constexpr float kReclaimYards = 35.0f;               // the core allows 39 yards; the ghost reclaims from this close

// A cast or channel is in progress. cast_remaining_ms is no test: it is 0 while a spell is in flight (a Fireball after the cast, state
// "delayed"), which the core still counts as casting and refuses every other cast for ("bot is casting spell N").
inline bool IsCasting(BotSnapshot const& bot)
{
    return bot.casting_spell != 0;
}

// Spells with a cast time (or a channel), which cannot be started while moving. The core's refusals add to the bot's own list, so a
// spell missing here is learned after its first refusal.
inline bool KnownCastTimeSpell(std::uint32_t spell)
{
    switch (spell)
    {
        case 686: case 585: case 133: case 116: case 5176: case 5185: case 403: case 331: case 2050: case 2054: case 2060: case 635:
        case 8092: case 348: case 707: case 172: case 1120: case 689: case 5676: case 5143: case 8004: case 421: case 1064: case 14914:
        case 15407: case 2948: case 5019: case 8921: case 19750: case 2006: case 7328: case 2008:
            return true;
        default:
            return false;
    }
}

// The spell a healer class heals an ally with (rank 1 id, or the next spell once the bot has the level), 0 for a class that does not heal.
inline std::uint32_t HealSpellFor(std::uint32_t classId, std::uint32_t level)
{
    switch (classId)
    {
        case 5: return level >= 16 ? 2054 : 2050; // priest: Heal, or Lesser Heal
        case 2: return 635;                       // paladin: Holy Light
        case 7: return 331;                       // shaman: Healing Wave
        case 11: return 5185;                     // druid: Healing Touch
        default: return 0;
    }
}

// The spell a class resurrects a dead ally with, 0 for a class that cannot or whose level is too low. A druid's Rebirth (20484, level 20)
// needs a reagent the bots are not given, so a druid is not a rezzer. A level of 0 is unknown and not judged.
inline std::uint32_t ResurrectSpellFor(std::uint32_t classId, std::uint32_t level)
{
    switch (classId)
    {
        case 5: return level == 0 || level >= 10 ? kResurrection : 0;    // priest
        case 2: return level == 0 || level >= 12 ? kRedemption : 0;      // paladin
        case 7: return level == 0 || level >= 12 ? kAncestralSpirit : 0; // shaman
        default: return 0;
    }
}

inline bool IsResurrectSpell(std::uint32_t spell)
{
    return spell == kResurrection || spell == kRedemption || spell == kAncestralSpirit;
}

// The spells that stop a cast in progress (rotation rows with Cond::TargetCasting). When several party members know one, only
// one may use it on the same cast: a second kick lands on nothing and both casts are wasted.
inline bool IsInterruptSpell(std::uint32_t spell)
{
    return spell == 853 || spell == 1766 || spell == 8042; // Hammer of Justice, Kick, Earth Shock
}

// The creature type the cc tactic needs (world database's creature_template.type) to pick a spell that can affect a pack member.
// The snapshot has no such field (forever_module::NearbyUnit, tier2_layout.h): the core does not report a unit's creature type at
// all, so this is a small built-in table of the entries the two shipped dungeon files can put in a tank's pack (data/dungeons/36-
// the-deadmines.json and 389-ragefire-chasm.json, kill_priority and bosses; verified against creature_template on 2026-10-27, the
// Forever test world database). An entry not listed here is of unknown type to the policy, so a type-restricted cc spell is never
// tried on it: a wider table for a dungeon added later belongs here, the same way a new dungeon's kill_priority needs its own list.
enum class CreatureType : std::uint8_t { kUnknown = 0, kHumanoid, kDemon, kElemental, kMechanical };

inline CreatureType CreatureTypeOf(std::uint32_t entry)
{
    switch (entry)
    {
        // Deadmines (36): the Defias Brotherhood and its goblins, all Humanoid.
        case 622:    // Goblin Engineer
        case 634:    // Defias Overseer
        case 639:    // Edwin VanCleef
        case 644:    // Rhahk'Zor
        case 645:    // Cookie
        case 646:    // Mr. Smite
        case 647:    // Captain Greenskin
        case 1729:   // Defias Evoker
        case 1731:   // Goblin Craftsman
        case 1732:   // Defias Squallshaper
        case 1763:   // Gilnid
        case 4418:   // Defias Wizard
        // Ragefire Chasm (389): orcs, trolls and goblins of the Searing Blade camp and the bosses, all Humanoid.
        case 11322:  // Searing Blade Cultist
        case 11323:  // Searing Blade Enforcer
        case 11324:  // Searing Blade Warlock
        case 11517:  // Oggleflint
        case 11518:  // Jergosh the Invoker
        case 11519:  // Bazzalan
        case 11520:  // Taragaman the Hungerer
            return CreatureType::kHumanoid;
        case 642:    // Sneed's Shredder: Mechanical (and a boss: never a cc target anyway, see the elite check below)
        case 2520:   // Remote-Controlled Golem: Mechanical
            return CreatureType::kMechanical;
        default:
            return CreatureType::kUnknown;
    }
}

// The cc spell a class has at all, whatever the target: 0 for a class with none. Sap (rogue), Polymorph (mage), Banish (warlock);
// no other class has a single-target hold in this module's scope today (a priest's Shackle Undead, a hunter's Freezing Trap and a
// druid's Entangling Roots are not: this change is scoped to the three the task names).
inline std::uint32_t CcSpellForClass(std::uint32_t classId)
{
    switch (classId)
    {
        case 4: return kSapSpell;        // rogue
        case 8: return kPolymorphSpell;  // mage
        case 9: return kBanishSpell;     // warlock
        default: return 0;
    }
}

// The level the Forever trainers teach a cc spell at (trainer_spell, verified 2026-10-27 against the test world database; the same
// levels as this file's SpellCostFile dump, data/spell_costs.txt). Sap and Polymorph are both well below either shipped dungeon's
// spawn level (Deadmines 23, Ragefire Chasm 18); Banish is not reached by either (it needs 28) -- kept here regardless, since cc is a
// general, dungeon-wide feature and a later, higher-level dungeon with a Demon or Elemental pack can use it once its roster is.
inline std::uint32_t CcLearnLevel(std::uint32_t spell)
{
    switch (spell)
    {
        case kSapSpell: return 10;
        case kPolymorphSpell: return 8;
        case kBanishSpell: return 28;
        default: return 0;
    }
}

// Whether a cc spell can affect this creature type at all (classic's type restrictions: Sap and Polymorph are Humanoid only,
// Banish is Demon or Elemental only). kUnknown never matches: the policy does not try a type-restricted spell on a creature it
// cannot place (see CreatureTypeOf above).
inline bool CcAffects(std::uint32_t spell, CreatureType type)
{
    switch (spell)
    {
        case kSapSpell:
        case kPolymorphSpell: return type == CreatureType::kHumanoid;
        case kBanishSpell: return type == CreatureType::kDemon || type == CreatureType::kElemental;
        default: return false;
    }
}

// Layout 13: whether the bot carries this aura right now. The aura list replaces the module's own buff timers, so a buff
// another player cast over the bot, or one it lost to a death, is the fact it is. An aura with no duration reports a large
// remaining_ms and runs for ever; a reported aura with 0 left is one that just expired.
inline bool AuraRunning(BotSnapshot const& bot, std::uint32_t spell)
{
    for (std::uint32_t i = 0; i < bot.aura_count && i < forever_module::kMaxAuras; ++i)
        if (bot.auras[i].spell_id == spell)
            return bot.auras[i].remaining_ms != 0;
    return false;
}

// Layout 13: the bot's pet, from the snapshot, instead of what refused casts imply. pet_state is 0 none, 1 alive, 2 dead.
// A core that does not report pets leaves it 0, and the inference from refused casts below is the fallback.
enum class PetKnown : std::uint8_t { kNone, kOut, kDead };

inline PetKnown PetOf(BotSnapshot const& bot)
{
    switch (bot.pet_state)
    {
        case 1: return PetKnown::kOut;
        case 2: return PetKnown::kDead;
        default: return PetKnown::kNone;
    }
}

// Duration of the damage-over-time effect a spell leaves on its target, or 0 for spells the policy does not track. Rank 1
// durations: while the effect runs, the policy does not recast the spell on the same target.
inline std::uint64_t DotDurationMs(std::uint32_t spell)
{
    switch (spell)
    {
        case kRend: return kRendDurationMs;
        case 172: return 18000;  // Corruption
        case 707: return 15000;  // Immolate
        case 589: return 18000;  // Shadow Word: Pain
        case 1978: return 15000; // Serpent Sting
        case 8050: return 12000; // Flame Shock
        case 8921: return 12000; // Moonfire
        case 21084: return 30000; // Seal of Righteousness (self)
        case 17: return 30000;    // Power Word: Shield (self)
        // The cc tactic's holds (SpellDuration.db2 of this server's own client data, read 2026-10-27): the same record keeps the
        // policy from recasting a hold that is still running, same as a dot.
        case kSapSpell: return 25000;        // Sap
        case kPolymorphSpell: return 20000;  // Polymorph
        case kBanishSpell: return 20000;     // Banish
        default: return 0;
    }
}

inline float DistanceBetween(NearbyUnit const& a, NearbyUnit const& b)
{
    float const dx = a.pos_x - b.pos_x;
    float const dy = a.pos_y - b.pos_y;
    return std::sqrt(dx * dx + dy * dy);
}

inline bool IsLiveMob(NearbyUnit const& unit);

// The level difference at which a creature gives no experience at all (the classic "grey" rule: GetZeroDifference). Whether this fork
// changes it is not known; the vanilla table is used.
inline std::uint32_t ZeroExpDifference(std::uint32_t level)
{
    if (level < 8)
        return 5;
    if (level < 10)
        return 6;
    if (level < 12)
        return 7;
    if (level < 16)
        return 8;
    if (level < 20)
        return 9;
    if (level < 30)
        return 11;
    if (level < 40)
        return 12;
    if (level < 45)
        return 13;
    if (level < 50)
        return 14;
    if (level < 55)
        return 15;
    if (level < 60)
        return 16;
    return 17;
}

// A creature far enough below the bot that killing it gives no experience: not worth pulling. (A level of 0 is unknown and not judged.)
inline bool IsGrey(BotSnapshot const& bot, NearbyUnit const& unit)
{
    return bot.level != 0 && unit.level != 0 && unit.level < bot.level && bot.level - unit.level >= ZeroExpDifference(bot.level);
}

// A unit's share of its health (0..1), or 1 when its maximum is unknown. A pull prefers the weakest, so a kill comes sooner.
inline float HealthShare(NearbyUnit const& unit)
{
    return unit.max_health ? float(unit.health) / float(unit.max_health) : 1.0f;
}

// The live hostile this bot should pull, or nullptr. A group is the hostiles within kPullRadius of the candidate, the candidate
// included. Units already attacking the bot are not chosen here; the policy fights its threat target whatever the group size.
// The weakest target wins and the closest breaks the tie: finishing one creature before the next is more experience per hour
// than walking to whichever one happens to stand nearest first.
// A creature standing high above or far below the bot while almost on top of it in the plane (a patrol on a wall walk, a ledge, the floor above) cannot be
// walked to from here: the flat distance says "in reach" while the way there is somewhere else, so the bot stands under it, its follow order finds no path
// and its spells have no line of sight. The 2026-10-28 soak: the Tormented Officers (entry 3873) patrol 20 yards above Shadowfang Keep's courtyard, the
// route's first point is right under them, and 34 stuck episodes (about 77 bot-minutes, fights of up to 12 minutes) all came from pulling them.
inline bool OnOtherLevel(BotSnapshot const& bot, NearbyUnit const& unit)
{
    float const height = std::fabs(unit.pos_z - bot.pos_z);
    return height >= kOtherLevelHeight && height > kOtherLevelSlope * rotation::DistanceYards(bot, unit);
}

inline NearbyUnit const* PullTarget(BotSnapshot const& bot, std::span<NearbyUnit const> units,
                            std::function<bool(std::uint64_t)> const& avoided, bool defending,
                            std::vector<std::uint32_t> const& onlyEntries = {}, bool dungeon = false)
{
    NearbyUnit const* best = nullptr;
    float bestHealth = 0.0f;
    float bestDist = 0.0f;
    for (NearbyUnit const& candidate : units)
    {
        if (!IsLiveMob(candidate) || avoided(candidate.guid))
            continue;
        // A creature on another level is not pulled (one that attacks the bot is: `defending`, and the stall watchdog deals with it if it cannot be reached).
        if (!defending && OnOtherLevel(bot, candidate))
            continue;
        // A bot hunting for a quest pulls only the creatures the quest wants (unless it is already being attacked).
        if (!defending && !onlyEntries.empty() &&
            std::find(onlyEntries.begin(), onlyEntries.end(), forever_module::UnitRefEntry(candidate.guid)) == onlyEntries.end())
            continue;
        // Nothing to gain from a grey creature (no experience): left alone, so the bot goes looking for something that levels it. Not when
        // hunting for a quest (the quest wants it), nor in a dungeon, nor when the bot is already being attacked.
        if (!defending && !dungeon && onlyEntries.empty() && IsGrey(bot, candidate))
            continue;
        // Not worth pulling: well above the bot's level, or an elite. (A level of 0 is unknown and not judged.) A bot that is already
        // in combat is being attacked, and the snapshot does not say by what: it fights the closest creature, whatever its level, since
        // standing still until it dies is worse. The same goes for the pack-size rule below.
        // In a dungeon (the route is active) elites, bosses and higher levels are the content: the party fights what it meets.
        if (!defending && !dungeon && bot.level != 0 &&
            ((candidate.level != 0 && candidate.level > bot.level + kMaxLevelAbove) || candidate.elite))
            continue;
        float const health = HealthShare(candidate);
        float const dist = rotation::DistanceYards(bot, candidate);
        if (best && !(health < bestHealth || (health == bestHealth && dist < bestDist)))
            continue;

        std::size_t group = 0;
        for (NearbyUnit const& other : units)
            if (IsLiveMob(other) && DistanceBetween(candidate, other) <= kPullRadius)
                ++group;
        if (!defending && group > (dungeon ? kMaxDungeonPull : kMaxPull))
            continue;

        best = &candidate;
        bestHealth = health;
        bestDist = dist;
    }
    return best;
}

// The closest unit in `units` that satisfies `wanted`, or nullptr.
inline NearbyUnit const* Closest(BotSnapshot const& bot, std::span<NearbyUnit const> units, bool (*wanted)(NearbyUnit const&))
{
    NearbyUnit const* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    for (NearbyUnit const& unit : units)
    {
        if (!wanted(unit))
            continue;
        float const dist = rotation::DistanceYards(bot, unit);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = &unit;
        }
    }
    return best;
}

// A live creature the bot may pick as its own target. Players are never picked: bots of the other faction would otherwise
// fight each other, and a player that attacks the bot still comes in as the threat target.
inline bool IsLiveMob(NearbyUnit const& unit)
{
    return unit.is_hostile && unit.health > 0 && forever_module::UnitRefType(unit.guid) == 8;
}

// A friendly player the bot may follow. A hostile player is not followed: it would lead the bot into a fight.
inline bool IsPlayer(NearbyUnit const& unit)
{
    return forever_module::UnitRefType(unit.guid) == 2 && unit.health > 0 && !unit.is_hostile;
}

// Avoids a unit for kAvoidMs, doubled for each earlier time it was avoided, so a unit that keeps coming back (a trainer the
// bot cannot attack) is left alone for longer each time.
inline void AvoidUnit(std::unordered_map<std::uint64_t, std::uint64_t>& avoidUntil, std::unordered_map<std::uint64_t, std::uint32_t>& count,
               std::uint64_t ref, std::uint64_t nowMs)
{
    std::uint32_t const times = ++count[ref];
    std::uint32_t const doublings = std::min<std::uint32_t>(times - 1, kMaxAvoidDoublings);
    avoidUntil[ref] = nowMs + (kAvoidMs << doublings);
}

// A stalled fight (nothing lost on either side): the target is left alone much longer than a refused one, more each time, because it is
// out of reach or bugged, and the bot has nothing to gain by coming back to it every two minutes (a mage did that for an hour and a half).
inline void AvoidStalledUnit(std::unordered_map<std::uint64_t, std::uint64_t>& avoidUntil, std::unordered_map<std::uint64_t, std::uint32_t>& count,
                      std::uint64_t ref, std::uint64_t nowMs)
{
    std::uint32_t const times = ++count[ref];
    std::uint32_t const doublings = std::min<std::uint32_t>(times - 1, kMaxStallDoublings);
    avoidUntil[ref] = nowMs + (kStallAvoidMs << doublings);
}

// The live creature in `units` with this reference, or nullptr.
inline NearbyUnit const* FindLiveMob(std::span<NearbyUnit const> units, std::uint64_t guid)
{
    for (NearbyUnit const& unit : units)
        if (unit.guid == guid && IsLiveMob(unit))
            return &unit;
    return nullptr;
}

// The point the kite goes to. The module has no navmesh or path query of its own (the core finds the path, and refuses a move with "no route"), so a point on a raw circle
// round the Warder may be a wall: 5,800 such refusals on 2026-10-29. When the dungeon's entry is known, retry 0 is therefore the retreat along the entry route: the
// point kAoeStandYards from the Warder on the line toward the entry the party walked in by, on the bot's own bearing (a few degrees either side, so a raid does not queue
// up on one point). Only when the core refuses that point (or the entry is unknown or nearer than the stand distance) does the circle come: the point kAoeStandYards from
// the Warder on the bot's own bearing from it, turned round by retry steps (0, +1, -1, +2, -2 times kAoeKiteRetryDegrees).
inline Point AoeKitePoint(BotSnapshot const& bot, NearbyUnit const& warder, int retry, std::optional<Point> const& entry = std::nullopt)
{
    if (entry && std::hypot(entry->X - warder.pos_x, entry->Y - warder.pos_y) > kAoeStandYards)
    {
        if (retry == 0)
        {
            float ex = entry->X - warder.pos_x;
            float ey = entry->Y - warder.pos_y;
            float const elength = std::hypot(ex, ey);
            ex /= elength;
            ey /= elength;
            float const slotAngle = float(int((bot.guid * 7) % 11) - 5) * kAoeKiteSpreadDegrees * 3.14159265f / 180.0f;
            float const ec = std::cos(slotAngle);
            float const es = std::sin(slotAngle);
            return Point{ warder.pos_x + (ex * ec - ey * es) * kAoeStandYards, warder.pos_y + (ex * es + ey * ec) * kAoeStandYards, bot.pos_z };
        }
        --retry;
    }
    float dx = bot.pos_x - warder.pos_x;
    float dy = bot.pos_y - warder.pos_y;
    float const length = std::sqrt(dx * dx + dy * dy);
    if (length > 0.01f)
    {
        dx /= length;
        dy /= length;
    }
    else
    {
        dx = 1.0f;
        dy = 0.0f;
    }
    int const slot = int((bot.guid * 7) % 11) - 5;   // -5..5, fixed per bot
    int const turn = retry == 0 ? 0 : (retry % 2 == 1 ? (retry + 1) / 2 : -(retry / 2));
    float const angle = (float(slot) * kAoeKiteSpreadDegrees + float(turn) * kAoeKiteRetryDegrees) * 3.14159265f / 180.0f;
    float const c = std::cos(angle);
    float const s = std::sin(angle);
    return Point{ warder.pos_x + (dx * c - dy * s) * kAoeStandYards, warder.pos_y + (dx * s + dy * c) * kAoeStandYards, bot.pos_z };
}

// The ground effects that harm a bot on this map: the snapshot's hostile ones, the avoid table's spells, the dungeon's own spells, and its zones. The same circles as the
// walk out of a harmful area (AvoidGroundEffects); a stand point inside one of them is not a place to move to.
inline std::vector<ground_avoid::Circle> HazardCircles(PolicyOptions const& options, BotSnapshot const& bot)
{
    std::vector<ground_avoid::Circle> circles;
    auto const dungeon = options.DungeonsByMap.find(bot.map_id);
    bool const hasDungeon = dungeon != options.DungeonsByMap.end();
    for (std::size_t i = 0; i < bot.ground_effect_count && i < forever_module::kMaxGroundEffects; ++i)
    {
        forever_module::GroundEffect const& effect = bot.ground_effects[i];
        bool harmful = effect.hostile != 0;
        float radius = effect.radius;
        if (options.AvoidSpells && options.AvoidSpells->Contains(effect.spell_id))
        {
            harmful = true;
            if (radius <= 0.0f)
                radius = options.AvoidSpells->Radius(effect.spell_id);
        }
        if (!harmful && hasDungeon)
            harmful = std::find(dungeon->second.AvoidSpells.begin(), dungeon->second.AvoidSpells.end(), effect.spell_id) != dungeon->second.AvoidSpells.end();
        if (!harmful)
            continue;
        circles.push_back(ground_avoid::Circle{ effect.x, effect.y, radius > 0.0f ? radius : ground_avoid::kDefaultRadiusYards });
    }
    if (hasDungeon)
        circles.insert(circles.end(), dungeon->second.AvoidZones.begin(), dungeon->second.AvoidZones.end());
    return circles;
}

// A stand point `yards` from the target, on the bot's own bearing from it. A point a hazard covers is not moved to: the bearing is turned round by up to four steps
// of kBandStepDegrees either side (0, +1, -1, +2, ...), and nullopt when every step is covered.
inline std::optional<Point> BandStandPoint(BotSnapshot const& bot, NearbyUnit const& target, float yards, std::vector<ground_avoid::Circle> const& hazards)
{
    float dx = bot.pos_x - target.pos_x;
    float dy = bot.pos_y - target.pos_y;
    float const length = std::sqrt(dx * dx + dy * dy);
    if (length > 0.01f)
    {
        dx /= length;
        dy /= length;
    }
    else
    {
        dx = 1.0f;
        dy = 0.0f;
    }
    for (int step = 0; step <= 8; ++step)
    {
        int const turn = step == 0 ? 0 : (step % 2 == 1 ? (step + 1) / 2 : -(step / 2));
        float const angle = float(turn) * kBandStepDegrees * 3.14159265f / 180.0f;
        float const c = std::cos(angle);
        float const s = std::sin(angle);
        Point const point{ target.pos_x + (dx * c - dy * s) * yards, target.pos_y + (dx * s + dy * c) * yards, bot.pos_z };
        if (!ground_avoid::InsideAny(point.X, point.Y, hazards))
            return point;
    }
    return std::nullopt;
}

// A player's unit reference, from its character guid counter (the same reference the core's party actions take).
inline std::uint64_t PlayerRef(std::uint64_t counter)
{
    return forever_module::MakeUnitRef(static_cast<std::uint8_t>(kPlayerType), 0, counter);
}

inline float Distance2d(BotSnapshot const& a, BotSnapshot const& b)
{
    float const dx = a.pos_x - b.pos_x;
    float const dy = a.pos_y - b.pos_y;
    return std::sqrt(dx * dx + dy * dy);
}

inline float DistanceTo(BotSnapshot const& bot, Point const& point)
{
    float const dx = bot.pos_x - point.X;
    float const dy = bot.pos_y - point.Y;
    return std::sqrt(dx * dx + dy * dy);
}

// The class of a bot: its own from the class table, else the configured class (0 = none).
inline std::uint32_t ClassFor(PolicyOptions const& options, std::uint64_t guid)
{
    if (options.ClassOf)
        if (std::uint32_t const own = options.ClassOf(guid))
            return own;
    return options.ClassId;
}

// A hunter whose ranged weapon uses ammunition and who carries none (layout 15: the core fills ammo_entry and ammo_count).
inline bool NoAmmo(BotSnapshot const& bot, std::uint32_t classId)
{
    return classId == kHunterClass && bot.ammo_entry != 0 && bot.ammo_count == 0;
}

// False when the cost file says the spell costs more power than the bot has now: the core would refuse the cast, so it is not tried. A spell the file
// does not price (no file, an unknown spell, a percent cost) or a cost in another power than the bot's main one (a druid in a form) is always tried.
inline bool CanPay(spell_cost::SpellCostData const* costs, std::uint32_t spell, BotSnapshot const& bot)
{
    if (!costs)
        return true;
    auto const payment = costs->CostAtLevel(spell, bot.level);
    if (!payment || payment->PowerType != std::int32_t(bot.power_type))
        return true;
    return bot.power >= payment->Cost;
}

// The snapshot facts the supply logic reads (supply_policy.h).
inline supply::Facts FactsOf(BotSnapshot const& bot)
{
    supply::Facts facts;
    facts.AmmoEntry = bot.ammo_entry;
    facts.AmmoCount = bot.ammo_count;
    facts.FoodCount = bot.food_count;
    facts.DrinkCount = bot.drink_count;
    facts.ManaUser = bot.power_type == 0 && bot.max_power > 0;
    facts.Copper = bot.copper;
    facts.FreeSlots = bot.bag_free_slots;
    facts.AmmoVendor = bot.ammo_vendor_valid != 0;
    facts.FoodVendor = bot.food_vendor_valid != 0;
    facts.HealthPotions = bot.health_potion_count;
    facts.ManaPotions = bot.mana_potion_count;
    facts.Bandages = bot.bandage_count;
    facts.GoodsVendor = bot.goods_vendor_valid != 0;
    facts.ReagentEntry = bot.reagent_entry;
    facts.ReagentCount = bot.reagent_count;
    facts.ReagentVendor = bot.reagent_vendor_valid != 0;
    return facts;
}

} // namespace bot_policy_detail

using namespace bot_policy_detail;
