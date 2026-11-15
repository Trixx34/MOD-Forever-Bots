# Flexible tank and healer roles (`FlexRoles`)

Default 0 (off): only a warrior tanks in `Role::Auto`, as before, and a roster entry with a spec sets its role. With `FlexRoles = 1` the roles are chosen when a party is cut (`BuildParties`), by `src/flex_roles.h`.

- Tank candidates, best first: warrior (stays tank, nothing changes for him), paladin (protection rows: Righteous Fury, Holy Shield, Consecration, Judgement), druid (feral_tank rows: Bear Form, Growl, Maul, Swipe, Demoralizing Roar, Enrage). A paladin or druid needs level 10 (Righteous Fury and Bear Form); level 0 (unknown) passes. A paladin has no taunt (Hand of Reckoning is not in the rotation data, so it is not cast); the druid taunts with Growl through the existing tank_loose code.
- Healer candidates, best first: priest, shaman, druid (resto), paladin (holy).
- The chosen tank gets `Role::Tank`: the existing tank paths apply (leads the party and the route, pulls, tank rotation by default spec). The chosen healer gets `Role::Healer`. Everyone else stays Auto (a surplus healer still heals on its own, a surplus paladin or druid is not made a tank).
- Explicit roles (a chat `role` order, a roster spec such as `:ret`, `:prot`) are never changed; they fill their slot.
- Order: with no warrior and no explicit tank, every (tank, healer) pair is tried; the pair that fills both roles wins, then the best tank rank, then the best healer rank, then the lowest guids. Same party, same answer. If the only tank candidate is also the only healer, the tank wins (`reason=tank_over_healer`). The choice is logged once per change: `flex_roles: party=<first guid> tank=<guid>(class n) healer=<guid>(class n) size=5 no_tank=0 no_healer=0`.
- Pull cap: `flex_roles::DungeonPullCap`. A paladin or druid tank pulls at most `kMaxDungeonPull - 1` (3) mobs, a warrior 4.
- Roles are re-decided each tick; a bot that loses its pick (the party changed) goes back to Auto. A chat `role` order makes the role explicit.

Unsupported: a party of only hunter, rogue, mage, warlock (`tank=none healer=none`); a party with no healer class (warrior or paladin plus four damage classes); a paladin or druid below level 10; a lone paladin/druid with no other healer gets tank, not healer.
