# Planned healing

Status: built and unit-tested (`src/heal_plan.*`, `src/bot_policy_heal_plan.cpp`, `tests/heal_plan_test.cpp`, three policy tests in `tests/policy_test_04.cpp`). **Not run on a sim yet.**
On by default (`Module.forever-bots.HealPlan = 0` turns it off); needs `PartyPlanner` on, because the plan is built in the planner's pass.

Before, every healer scanned the whole group, took the neediest member and relied on a 1.5 s claim to keep two healers off one member. In a raid that
gives no tank priority and no spread. The idea (ours, not code of another project) is the one in the todo's "Raid brain ideas": a standing split of the heal duty.

## The assignment

Built each tick per party (a raid is one party here), `heal_plan::Update`; kept while nothing changes.

- Healers: living members that can heal (a heal spell, not playing tank or damage).
- Main healer: kept while it lives, otherwise the lowest healer guid. It takes the tanks (members playing the tank; the leader when nobody does).
- Everyone else goes to the healer with the fewest members so far (a tank counts 2). A healer is preferably another healer's member, not its own.
- An assignment is kept while its healer lives and the set of healers is unchanged; a new member takes the least loaded healer; dead members keep theirs. When a healer dies or returns, the members are dealt again.
- One healer: it has everyone and nothing changes compared to before.

## What a healer does with it (`PartyHeal`)

1. Its own members need a heal (below the heal mark, the tank below its mark): it considers only those, plus anyone under the emergency mark (20 %), which is everybody's.
2. Its own members are fine: it considers the others too, the lowest health first (the normal chain), but leaves a member whose own healer is alive, in range and off its global cooldown (that healer takes it in its own turn). A taken member logs `heal_assign_switch`.
3. Pre-heal: the main healer uses `HealPlanPreHealPct` (96) instead of the tank mark (75) for the tank during the first `HealPlanPreHealMs` (4000) of a fight (the leader's fight start), so the tank has full health when the pack arrives. Not later in the fight.

With a plan the planner's reactive "nearest healer" pick (`party_planner` `Heals`) is not used by `PartyHeal`; the plan and the claims decide. Prayer of Healing, Renew, Binding Heal, shields and
the mana rules are unchanged.

## Events

- `heal_plan: leader=<guid> healers=<n> main=<guid> <healer>:[<members>] ...` when an assignment changes (member lists cut at 10).
- `heal_assign_switch: healer=<guid> target=<guid> assigned_healer=<guid> target_hp_pct=<n>` when a healer casts on a member that is not its own.

## Settings

`HealPlan` (1), `HealPlanPreHealMs` (4000, 0 = no pre-heal), `HealPlanPreHealPct` (96).

## What only the sim shows

Whether the split lowers deaths in ZG and Onyxia against `HealPlan = 0`, that the pre-heal costs no mana trouble, healers out of line of sight of their assigned members (the switch covers range, not sight), and the event rate. Compare with the heal-related counts (`heal:` lines, deaths, `healer_fetch`) of a run with the plan off.
