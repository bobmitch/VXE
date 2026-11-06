# World design — gameplay and how it drives generation

**Status: working draft.** Sections are tagged **[decided]**, **[proposed]** or **[open]**. Only the
tagged-decided parts are settled; everything else is a starting position to argue with. Nothing here
is implemented yet.

This document exists because world generation is about to become the long pole, and the generator
cannot be designed without knowing what the player is supposed to meet. Every section below ends in
a concrete demand on the generator.

---

## 1. The world

**[decided]**

| | |
|---|---|
| Extent | 22,624 m × 22,624 m (`kWorldHalfExtentXZ` = 11,312), y from −512 to 1,536 |
| Spawn | A single central point, near (0, 0) |
| Sea level | y = 0 (`TerrainParams::seaLevel`) |
| Distance to a cardinal edge | 11.3 km |
| Distance to a corner | 16.0 km |

Two global gradients shape everything:

* **Latitude (north ↔ south) sets climate.** Cold in the north, hot in the south.
* **Radius from spawn sets danger.** The further you travel from the centre, the more hostile and
  the more exotic the world becomes.

Biomes other than ocean/deep sea may occur anywhere. They are not gated to islands or to separate
landmasses — the world is continuous land and water, and a biome is a region of it, Valheim-style in
tone: grounded, believable landscape, not floating fantasy geometry.

Ocean is the exception: it is not placed on the biome map at all. It is simply where terrain falls
below sea level, which is already how the generator works.

### Scale and pacing

**[proposed]** Nothing so far fixes how *big* a biome is or how long the world takes to cross, and
almost every other number depends on those two. Working backwards from the loop:

| Quantity | Proposed | Why |
|---|---|---|
| Effective danger bands, centre to edge | ~5 | Enough for a progression to have stages; more and they stop being distinguishable |
| Band width | ~2.3 km | 11.3 km ÷ 5 |
| **Biome patch diameter** | **1–2 km** | Several biomes per band, each still large enough to have an identity |
| Climate field wavelength | 2–4 km | A patch is roughly half a wavelength |
| Biome patches crossed, centre to edge | ~8 | Variety without patchwork |
| Unobstructed run, centre to edge | ~38 min | 11,312 m at 5 m/s |
| Realistically, with relief, water and fighting | 1–1.5 h | |
| **Band to band — one frontier move** | **15–20 min** | 2.3 km with terrain. This is the trip the loop is actually built around |

**This settles the coarse tier cell size.** A 1.5 km biome patch is 23 cells across at 64 m and six
at 256 m. Six cells cannot describe a believable border — you get a hexagon. **64 m**, and §8's other
clients all benefit from the same choice.

**The vertical axis is badly underused.** y spans 2,048 m — 1,536 above sea level and 512 below. If
creature danger increases with depth (§2), then 500 m down near spawn can be as dangerous as the
surface several kilometres out. That hands an early player a high-risk option close to home, makes
the near-spawn rare-biome exclusion (§3) a *surface* rule rather than a blanket one, and gives the
Hollows something to owe. It is a second progression axis that costs nothing, because the range is
already there.

**[open]** Is the map edge ocean, an impassable barrier, or just an edge? A surrounding ocean is the
cheapest believable boundary and falls out of the existing sea-level rule, but it needs deciding
because it determines whether the corners are reachable land at all.

---

## 2. The gameplay loop

**[decided] The pillars are exploration and combat.** Everything else earns its place by serving one
of them.

That is the sharpest constraint in this document, and it is worth applying backwards immediately,
because the engine's feature list and the pillar list are not the same list:

| Engine feature | Serves a pillar? | Consequence |
|---|---|---|
| **Terraforming** — 1 m CSG editing | Both | Routes and access for exploration; shaping the ground you fight on for combat (below) |
| **Caves and tunnels** — full 3D voxels | Both | A second map to explore, and the sharpest creature danger in the game |
| **Building** — `BuildingSystem` structural integrity | **Not directly** | Must stay subordinate: forward bases, watchtowers, defensible ground |
| **Logistics and hauling** | Only while it is dangerous | A haul that is a walk fails both pillars |

**Building is the one to watch.** `BuildingSystem` is substantial engineering and structural
integrity is genuinely distinctive, but building is not a pillar. It serves exploration by making
forward positions possible and reconnaissance towers worth raising (§7), and it serves combat by
letting a player manufacture defensible ground. Depth beyond that is scope that buys nothing, and it
is the most likely place for this project to quietly become a different game.

**And the haul now has a pass/fail criterion** rather than a worry. If hauling is walking, it fails
both pillars and should be cut. It survives only as a *gauntlet* — a return trip through territory
that is still hostile, with something to lose. That is a much harder target than "add weight", and
it is the right one.

### Terraforming as a combat verb

**[proposed]** If combat is a pillar and terraforming is the engine's signature, the two should meet.
Almost nothing does this, because almost nothing has persistent destructible terrain at this
resolution.

Concretely: dig a pit and fight from the far side of it. Cut a choke so a pack has to come single
file. Collapse a cave ceiling onto something. Undermine the ground a heavy creature is standing on.
Wall a tunnel behind you and listen to it being dug out.

Three reasons this is worth taking seriously rather than treating as flavour:

* **It is nearly free.** `Terraformer`, `BuildingSystem`'s support propagation and Jolt are already
  there. The missing piece is creature AI that reads terrain as a tactical surface, and that work is
  already on the critical path for navigation (§6).
* **It gives terraforming a reason to exist in the moment**, not only across a session. Right now
  every justification for digging is infrastructural and slow — roads, shelter, access. This makes it
  a thing you do under pressure, which is where the engine actually shows off.
* **It makes permanence a storyteller.** The pit you dug to survive something is still there when you
  come back, and so is the collapsed tunnel. The world keeps a record of the fights that happened.

**[open]** It also hands players the obvious exploit: dig a hole nothing can reach you in. The
counter-design — things that climb, dig, ranged attackers, structural collapse under load — is real
work, and it should be scoped before this is promised.

### The test

§5 says a biome earns its place by demanding something from the engine. The same test applies one
level up, and it bites harder:

> **If the game could be played on a heightmap with no digging, the loop is wrong.**

Terraforming and structural building are the two things this engine does that a conventional terrain
renderer cannot. A loop where digging is a convenience — a shortcut, some landscaping — wastes both.
A loop where digging is how you travel, survive or hold ground is the one worth building.

### Two constraints the engine already imposes

**1. Edits are permanent, which pushes hard toward a frontier.**

`DeltaStore` records every edit relative to regenerated base density. There is no healing, no
regrowth, no reset. A region you have mined out stays mined out, in that save, forever.

In Minecraft that is neutral. Here, combined with the radial threat gradient, it *is* a loop: the
world behind you is spent and safe, the world ahead is intact and dangerous. The pressure to move
outward does not have to be invented — it falls out of two systems that already exist. A loop built
on one permanent base fights this; a loop built on a moving frontier is handed it for free.

**2. Danger is creatures, so navigation is the critical path.**

**[decided]** Danger is mostly creatures. Environmental hazard still matters a great deal, but as a
*gate* rather than as the threat — see **Progression and gating** below. The threat itself walks
around, and it has to be able to find you.

This knowingly picks the expensive option. Navigation over destructible voxel terrain is unstarted,
and `DeltaStore` makes every edit permanent, so static navmesh baking is off the table. Nothing else
in this document is blocked on a problem that size, and it moves to the top of §9.

### Candidates

| | Shape | Engine fit | Costs |
|---|---|---|---|
| **A. Delving** | Surface is a hub; the real map is below. Threat scales with depth as well as radius. Dig to open routes and to escape; fortify to hold ground | Best of the three. Caves become the primary map, terraforming becomes traversal, karst is the showcase | Contradicts the priority already set: surface first, tunnels secondary |
| **B. Frontier settlement** | Establish a holding, work the region around it, outgrow it, push outward and do it again | Terraforming is site preparation — level a shelf, cut into a hillside, dam a stream, drain a fen. Structural integrity makes *where* you build a real decision | Needs a reason the current holding stops being enough |
| **C. Expedition and extraction** | One permanent safe base near spawn; outward runs with limited carry into lethal ground, and back | Fits the radial gradient most literally | Two bad ones — see below |

**C has a specific problem worth naming.** Permanence works *against* it. Routes you cut stay cut, so
each expedition into a region is easier than the last and the far world gets progressively tamer —
backwards from what the threat gradient is for. It also reduces building to a decorative hub
activity, which wastes the one system with no equivalent in the comparable games.

### Recommendation: B, with A's underground as the reason to go anywhere

Surface primary, as decided. Underground is where the region's value is, so the cave system is the
reason a holding is sited where it is rather than a side activity.

* **Short session** — work the current holding. Build, extend, dig the cave system beneath it,
  process what comes out.
* **Long session** — scout and claim the next one.
* **The tension that makes it a loop** — the next holding is better placed for what you need and
  worse placed for staying alive. Every move outward is that trade.

**The cheapest way to make a holding stop being enough is biome-gated materials**, Valheim-style:
each biome owns something you cannot get elsewhere. It needs no economy simulation and no depletion
model, and it ties the loop directly to the roster in §5 — which means **every biome there now owes a
material**, and that is a new requirement on generation (§8).

### Progression and gating

**[decided]** No Man's Sky-style hazard and tech progression, crossed with Valheim-style biome
materials and combat.

Two axes escalate, and what makes them work rather than collapse into one difficulty number is that
they gate through **different mechanisms**:

| Axis | Source | Pressure | Answered by |
|---|---|---|---|
| Latitude and elevation | Climate (§4) | Cold, heat | **Preparation** — clothing, cooling, shelter, tech |
| Radius from spawn | Threat (§3) | Creature level ceiling | **Capability** — gear and combat skill |

This is what the first draft's grid was reaching for and got wrong. Climate and radius are both
progression axes, but one is a *packing* problem and the other is a *fighting* problem, so a player
can be far ahead on one and behind on the other. That is two progression tracks out of two fields
that already exist.

#### Every gate is a rate, not a wall

**[decided]** No hard boundaries. A hazard drains; it never blocks.

Cold without clothing is health loss per second, not a barrier. A player at full health with food can
cross two minutes of tundra underdressed — they simply cannot live there. That is the whole
risk/reward shape, and it reduces to one implementation rule, worth stating because walls are easy to
build by accident.

Keep both hazard answers available and the gate stays soft from either direction:

* **Materials are what you build with** (Valheim) — the warm coat, the cooled suit. Durable, and
  gated on having already been somewhere.
* **Charges are what you spend** (NMS) — a heat source, a coolant. Consumable, bought with
  preparation rather than with progression.

Having both means *underdressed but well stocked* is a real way to push ahead of the curve, which is
exactly the soft gate players enjoy beating.

#### Underground is the universal refuge — and that is what finally makes digging mandatory

Tunnelling for shade is the strongest idea here, and it generalises much further than heat. Caves sit
near the mean annual temperature of their region: hot climates are cooler underground, cold climates
are warmer, and both are out of the wind. So the rule is not "dig in the desert", it is:

> **Environmental safety increases with depth. Creature danger increases with depth.**

That inversion is the best tension in the document. It gives the second map layer a permanent role in
*every* biome rather than only in the karst, and it is the first mechanic proposed that makes digging
necessary instead of convenient — which is the test at the top of this section, passed.

#### The radial gate needs its own skip, or it is the only hard one

Hazard gates are skippable by taking damage. Creature gates are not: you cannot tank a level ceiling.
Left alone, climate is soft and radius is a wall, which breaks the rule above.

The on-brand answer is **tunnelling under**. Dig past a region instead of fighting through it. It
costs time, it puts you in the worse creature roster, and you surface blind — three real prices — and
it uses the one system no comparable game has. Stealth, night, speed and water crossings are the
conventional alternatives and can coexist with it.

This worry — that tunnelling under a region lets a player skip its content — is largely dissolved by
transport restriction below. Tunnelling helps the *haul*, not the *acquisition*.

#### Water as a soft gate

**[decided]** Large water bodies influence where you can go, and reward perseverance.

This is a real generation requirement rather than a free consequence of the sea-level rule. A random
height field will not reliably put a lake between the player and somewhere worth reaching. Hydrology
has to be *placed* on the coarse tier — lakes, rivers, and land connectivity as a queryable property
— and the far shore has to be worth the crossing. See §8.

### Logistics: what you cannot carry

**[decided]** Materials are gated to biomes, and the gate is enforced by **transport restriction**
rather than by locks. Fast travel exists, is necessary, reaches only places already visited, and
refuses to carry materials above its own tier.

State it as a rule rather than a list:

> **Transport tier N carries materials below tier N.** The newest thing you have mined always has to
> come home on your back.

Valheim enforces this ad hoc — ore and ingots specifically — and the cost is that players memorise an
arbitrary exception. A tier rule is self-documenting, and it has the better progression beat: every
new transport tier is a *retroactive* quality-of-life unlock for everything you have been hauling by
hand until then, while the current frontier material stays physical forever. The logistics problem
never disappears; it just keeps moving outward with you.

#### Why this is the most load-bearing mechanic in the document

Without it the 22.6 km is decoration. Teleport everything and geography stops mattering, hydrology
stops mattering, connectivity stops mattering, and the frontier loop collapses into a menu.
Transport restriction is the thing that converts the map into a surface the game is played on.

It also combines with permanence into the best engine fit proposed anywhere in this document:

> **Roads and tunnels are permanent player infrastructure.**

Cut a switchback up a cliff, bridge a ravine, drive a tunnel through a ridge — `DeltaStore` keeps it
forever, and every subsequent haul over that route is cheaper because of work done once. The first
crossing of a region is brutal; the tenth is a road you built. Minecraft gets a weak version of this.
Valheim cannot do it at all, because its terrain tooling will not cut a pass through a mountain.

This is terraforming as infrastructure, and it passes the test at the top of §2 harder than anything
else here.

#### Tech tiers

**[decided]** Four rungs: craft, power, energy, and the incomprehensible.

**The ladder is spatial, not only temporal.** You do not invent alien tech in your starter base — you
*find* it, further out. That maps the rungs onto the radial gradient §3 already defines: the world
gets stranger outward, so exotic tech is what is lying out there to be scavenged and reverse
engineered. The ladder becomes a consequence of the frontier loop instead of a second progression
running beside it, and it finally gives §1's "temperate but maximally dangerous east and west"
something to hold. Ordering follows from this — the least explicable thing goes furthest out.

**[proposed]** Define each rung by what it changes about the player's relationship with *terrain*,
not by its material list. That is the axis this engine sells, and it keeps each rung mechanically
distinct instead of being the previous one with better stats.

| Tier | Vocabulary | What it changes about terrain | Transport step |
|---|---|---|---|
| **1. Craft** | Wood, bronze, iron, steel | Hand tools. You dig slowly and locally; terrain is an obstacle to work *around* | Your back, and the roads you cut |
| **2. Power** | Combustion, steam, electricity — machines that work while you are elsewhere | Powered excavation at scale; terrain becomes something you *route through* | **Rails and carts, over the routes you already built** |
| **3. Energy** | Lightweight composites, stored energy, field effects | Vertical freedom — gliding, controlled descent, assisted climbing; terrain becomes something you *cross* | Short-range point to point |
| **4. The incomprehensible** | Recovered, never understood | Distance itself becomes negotiable, on conditions you did not set | True fast travel, conditional and costly |

**Tier 2's reward is not better armour — it is that your roads become railways.** That is the payoff
for every hour spent cutting a pass or driving a tunnel, and it makes infrastructure investment
compound instead of plateau. It is also a far sharper identity than "ceramics and plastics", which
are real industrial materials with no romance and a Factorio-sized production chain hiding behind
them. What tier 2 owes mechanically is **power** — things that run while you are not standing there.
Materials should serve that, not be the point.

#### Three suggestions on the ladder, since nothing is off the table

**1. Tiers should compose, not replace.** If tier 3 *consumes* steel and timber rather than obsoleting
them, three problems close at once: early biomes stay economically relevant forever, the starter
region never becomes dead ground, and the fast-travel hub has a permanent reason to exist. It also
means the tier boundaries need not align with danger bands — tier 1 can comfortably span two — which
answers the "one tonal shift per band is too fast" worry without dropping a rung.

**2. Magic as the tier where you stop understanding the technology.** Clarke's law played straight:
tier 3 is alien tech you reverse engineered, tier 4 is alien tech you *cannot*, usable only by rules
discovered empirically rather than derived. Nothing is literally supernatural, so §1's grounded tone
survives — and tier 4 gets a real mechanical identity rather than bigger numbers with particles:
**its items carry conditions and costs instead of stats**. It also needs no fourth art vocabulary,
being tier 3's vocabulary used ritually. Full supernatural is still available if the grounded framing
is not worth the constraint; this is the version that costs less and breaks less.

**3. The ladder inverts the haul, and that needs handling.** Weight falls monotonically across these
rungs — "super lightweight" is tier 3's defining property — while §2 makes hauling the load-bearing
mechanic. Left alone, logistics evaporates exactly when the world is at its biggest.

The fix is to **move the cost from transport to acquisition**: late materials are light but cannot be
stockpiled, because they decay, discharge or destabilise. You can carry plenty; you cannot hoard it.
The haul then survives by changing character rather than by scaling — early it is *heavy and far*,
late it is *light, perishable, and everything out here kills you*.

**One thing to be careful of: flight ends the map.** Tier 3 should buy gliding, controlled descent
and assisted climbing — vertical *mobility*, not free flight. Mobility makes 1,536 m of relief
traversable and makes the reconnaissance channel (§7) cheaper to use. Flight deletes terrain,
logistics, chokepoints and the entire frontier loop in one unlock.

#### How you actually haul: fauna first, machinery later

**[decided]** Production chains are out of scope. Raw logistics is in. The ladder is **tamed fauna
early, simple machinery later** — animals to farm, pull carts and be trained; machines afterwards for
auto-farming and auto-mining.

The scope boundary, stated so it can be enforced rather than re-argued every time something is added:

> **A machine converts terrain or elapsed time into one material. Machines never take another
> machine's output as input.**

That makes a production graph impossible by construction rather than by discipline. An auto-miner, a
pump, a field — each is a leaf. Nothing composes, so nothing quietly becomes a factory game.

**Tamed fauna is the strongest idea in the logistics design**, and it pays for itself several times:

* **It serves both pillars.** Taming means finding the animal (exploration) and surviving it
  (combat). A machine serves neither.
* **It reuses the critical path.** Pack animals are creatures with different AI, so the navigation
  work (§6) that creature danger already forced now serves hauling and taming too. One hard system,
  three uses.
* **It makes the haul interactive**, which is the pass/fail criterion set at the top of §2. Managing
  animals under threat is gameplay; carrying a heavier backpack is not.
* **It gives the creature roster a non-combat role**, which enriches every biome without a new
  system — each roster can now hold something worth taking home alive.

**The elegant consequence: a pack animal has its own hazard tolerance.** A beast tamed in the
pinelands suffers in the south. That makes the climate gate apply to your *logistics* rather than
only to your body, so preparing for a region now means preparing the thing carrying your gear — and a
biome's native fauna becomes the natural answer for hauling inside it. None of this needs new
mechanics. It is the hazard system already decided, pointed at a creature instead of a player.

**[open]** Does taming scale with the tech ladder, or stay a tier-1 answer that machinery supersedes?
Keeping both alive is more interesting — animals go where machines cannot, machines work while you
are elsewhere — but it is two logistics systems to balance forever.

**[open]** The haul is Valheim's most-complained-about feature. It only works if carrying has
gameplay: weight that slows you, ambush en route, and a route that improves because you improved it.
The third is the one this engine can do and Valheim cannot, and it is probably most of the answer.

### Single-player and co-op

**[decided]** Both are supported. Co-op is not bolted on and single-player is not the lesser mode.
Single is slightly harder, and only in combat.

#### Two kinds of scaling, and only one is safe

**Per-capita consumption scales by arithmetic, not by design.** Four players wear four coats and
carry four weapons, so they need four times the pelts and the ore. Nothing has to be built for this
— it falls out of each player being a person.

**Per-item cost scaling is a tax, and should be avoided.** Making a coat cost four times as much
*because* four players are present is the thing players notice and resent: joining a friend makes
your own progress slower. Valheim keeps item costs flat on purpose, and treats "co-op is easier" as
the reward for playing together rather than a bug to patch out.

#### Party size may scale density. It must never scale tier.

This follows directly from §3 and is worth stating as a hard rule. Creature *level* is a property of
position, baked at spawn from the threat field (§6) — it is how the world tells a player where they
are. If a level-7 creature appears near spawn because four players are standing there, the radial
gradient stops meaning anything and the reconnaissance channel (§7) starts lying to everyone.

> **Scale how many. Never scale how dangerous.**

#### The asymmetry nobody mentioned: infrastructure does not scale

Materials scale with party size. Infrastructure does not. A road is a road, a tunnel is a tunnel, a
rail line is built once however many people use it — so co-op takes a *linear* per-capita discount on
exactly the work this design cares most about. That is a bigger difference than combat, and it is
invisible if only materials are considered.

Two honest options:

* **Accept it.** Co-op being easier is normal, and infrastructure is a reasonable place for the
  discount to land. This is Valheim's answer.
* **Let consumption absorb it.** Gear consumption *does* scale, so four players need four times the
  ore and therefore four times the hauling over the road they built once. The road is cheaper per
  head; the traffic over it is not.

The second is better, and it needs nothing new — it is already implied by the two rules above.

#### What co-op demands of the engine

| Demand | Detail |
|---|---|
| **`ChunkStreamer` assumes one focus** | `update(const Vec3d& focus)` and `computeDesired(focus)` are built around a single point. Players kilometres apart need either independent streamers with duplicated residency, or a desired-set that is the *union* over foci with each node taking the finest LOD any focus asks for. The second is cleaner, and far easier now than retrofitted |
| **`DeltaStore` becomes shared state** | Permanence is shared — one player's dig is everyone's road. The edit log has to replicate, and it is the authoritative record of the world |
| **Generator version becomes a compatibility check** | §8 already says to pin it. With two clients it is no longer only a save-file concern: differing versions produce different base density, so identical deltas yield different terrain |
| **Several live areas at once** | §6's simulation bubble becomes several, potentially far apart, each owing creature simulation and machine bookkeeping |

#### Fast travel, which answers itself

"Only to places already visited" raises *visited by whom*. The question disappears if **anchors are
built structures rather than discovered waypoints** — anyone can use a thing that exists in the
world, late joiners are not punished, and it becomes one more way building serves exploration instead
of standing on its own.

### What is still open

| Fork | What it changes |
|---|---|
| Permanent home base, or genuinely nomadic? | Whether every region needs buildable ground, fresh water and shelter, or only some |
| Is there a win condition? | Whether the radial gradient terminates in something, or just runs out |

---

## 3. Three systems, not one grid

**[proposed — this replaces the danger × climate grid from the first draft.]**

The first draft made danger an *axis of biome placement*: a Badlands existed only at 4–7 km south.
That is wrong twice over. It contradicts "biomes can occur anywhere", and it makes biome identity
unlearnable — if each ring outward is a fresh set of names, nothing a player understands at 3 km
transfers to 8 km, and the map becomes a corridor wearing a sandbox costume.

Pull the two apart, and add a third thing to carry what the grid was secretly doing:

| System | What it is | What it controls |
|---|---|---|
| **Biome** | climate + moisture + elevation + relief | terrain character, materials, flora, *which* creatures are native |
| **Threat** | noise-warped radius from spawn | the ceiling of what spawns there and how the roll skews |
| **Rarity** | per-biome placement weight, radius-biased | how strange the world gets as you travel |

Biome answers *where am I*. Threat answers *how much trouble am I in*. Rarity is the thing that stops
the far world from being the near world with bigger numbers.

### The load-bearing rule

**Threat does not touch terrain.** A fen at 2 km and a fen at 9 km are generated by the same code,
with the same materials, the same silhouette, the same cave character. What differs is what lives
there. Three things fall out of that:

1. **Biome knowledge transfers.** Learning to read a fen is worth something for the whole game.
2. **Terrain generation never needs to know the radius**, which means terrain stays a pure function
   of climate and elevation — exactly the terrain-first model that keeps mountain ranges running
   across borders instead of stopping dead at them.
3. **Exploring outward is not the only progression.** Going *down* can be another, because cave
   depth is available as a threat input independent of radius.

### Where "exotic" comes from, if not from terrain

Rarity. Every biome has a placement weight; some of those weights ramp with radius.

* **Common biomes** — flat weight. Forest, fen, downs, taiga, scrub. They occur anywhere the climate
  suits, at any radius, exactly as decided in §1.
* **Rare biomes** — near-zero weight inside some radius, rising steeply outside it. The Hollows,
  the Rime, Cinder flats. Possible at 3 km and almost always absent; dominant past 8 km.
* **Hard exclusion inside the starter radius only** (see §7). This is the one place a hard rule
  earns its keep, because a 1-in-200 roll that ruins the first hour will happen to someone.

So the far world is mostly strange places and the near world is mostly ordinary ones, with no fence
between them. A rare biome reaching inward along one bearing is not a bug — it is the best thing the
map can do, and the reason the radial field must be noise-warped rather than a clean radius.

**[open]** How steep should the rare ramp be? Shallow and the far world is diluted with meadows;
steep and the rare biomes are effectively gated, which is the grid again with extra steps.

---

## 4. How a biome gets chosen

**[proposed]**

Assign the biome from a small field set. Danger is no longer one of the fields.

| Field | Source | Authored or emergent |
|---|---|---|
| Temperature | Latitude gradient + elevation lapse rate + noise jitter | Authored |
| Moisture | Noise + distance to water + rain shadow behind ranges | Mixed |
| Elevation | Terrain tier | Emergent |
| Relief / erosion | Terrain tier | Emergent |
| Rarity weight | Per-biome, some ramped by warped radius | Authored |

Three details that matter more than they look:

1. **Latitude sets the temperature baseline; elevation modifies it.** A lapse rate gives snowy peaks
   in the south for free, so "cold in the north" stays a heuristic rather than a straitjacket.
2. **Biome influence on terrain parameters must be blended over a transition band**, not switched. A
   hard switch on terrain amplitude is a cliff at the border — and once creatures cross borders (§6)
   it is also a wall that breaks pathing.
3. **The threat field must be warped by different noise than the climate fields**, so high threat
   does not correlate with any particular climate or terrain. If it does, players read threat off the
   landscape by proxy, and the rare-biome signal stops carrying information.

---

## 5. Biome roster

**[proposed — names and composition are a starting point, not a decision.]**

Organised by what places the biome, and flagged by rarity. Meadows / Dark Forest from Valheim are in
here as reference points, not as the answer.

| Biome | Climate | Placement | Rarity | What it asks of the engine |
|---|---|---|---|---|
| **Downs** | Temperate, dry | Low elevation, low relief | Common | Buildable ground, shallow caves. The baseline against which everything else is strange |
| **Deepwood** | Temperate, wet | Low–mid elevation | Common | Low light; sightlines as a hazard |
| **Fen** | Temperate, very wet | Low elevation, flat | Common | Unstable footing, sinkholes, water table |
| **Pinelands** | Cold, wet | Mid elevation | Common | Snow accumulation, slope |
| **Scrub** | Hot, dry | Low–mid elevation | Common | Sparse cover, heat |
| **Highland** | Any — elevation overrides latitude | High elevation | Common | Verticality, exposure, alpine caves |
| **Badlands** | Hot, dry | Mid elevation, high relief | Uncommon | Natural arches, slot canyons, overhangs — the clearest proof this is not a heightmap |
| **The Hollows** | Temperate | Mid elevation, carbonate bedrock | Rare | Karst. Sinkholes, dolines, a cave system that *is* the map |
| **The Rime** | Cold | High latitude or high elevation | Rare | Ice sheet over bedrock, crevasses, subglacial rivers |
| **Cinder flats** | Hot | Any elevation, volcanic substrate | Rare | Lava tubes — long, smooth, rarely branching: a different tunnel topology |
| **Ocean / deep sea** | — | Below sea level | Emergent | Not placed; falls out of the height field |

Eleven, of which six are common. That is close to Valheim's eight and a long way below the fifteen
the grid implied.

### The rule that should govern the roster

**A biome earns its place by demanding something from the engine.** This engine's distinguishing
features are overhangs, cave systems, terraforming and structural building. A flat grassland asks
nothing of any of them and could be a heightmap. A karst landscape pocked with collapsed dolines
asks for all four.

**The Hollows should be built first** — not because it is the most dangerous, but because it is the
only entry that stresses tunnels, overhangs and building-on-unstable-ground simultaneously. If the
generator can produce convincing karst, every other biome on this list is a reduction of it.

### What each biome owes the loop

§2 makes every biome responsible for three things. Stating the *requirement* rather than inventing
the fiction, because the fiction is cheap to change and the requirement is not:

| Biome | Hazard | What its material must unlock | Roster character |
|---|---|---|---|
| **Downs** | None | Nothing — this is the baseline | Low tier; teaches the combat verbs |
| **Deepwood** | None; low light | Light, and the first tool step | Ambush at short sightlines |
| **Fen** | Wet, unstable footing | First metal — the step change in tools and weapons | Uses the footing against you |
| **Pinelands** | Mild cold | First cold protection | Pack behaviour |
| **Scrub** | Mild heat | First heat mitigation | Fast, open-ground |
| **Highland** | Cold by elevation; exposure | Structural material for building high and thin | Territorial; attacks from above |
| **Badlands** | Strong heat | Active cooling tech | Vertical ambush |
| **The Hollows** | Darkness below, not above | Whatever makes deep caving survivable | The underground roster proper |
| **The Rime** | Extreme cold | Top-tier cold protection | Apex; low density |
| **Cinder flats** | Extreme heat | Top-tier heat tech | Apex; high density |

**Because gates are rates, this chain bootstraps itself and needs no explicit unlock.** The edge of a
cold biome is survivable for two minutes; that is long enough to take what makes a coat; the coat
buys sustained presence; sustained presence buys the incursion into the next one. Nothing has to be
scripted — the progression is a consequence of the hazard being a drain rather than a door.

**The Hollows earns its keep through depth rather than through a material.** It is where the vertical
axis (§1) is cheapest to reach — the only biome where 500 m down is a short trip rather than an
expedition. That makes it the natural entry point to the depth progression, and it is why a Hollows
near spawn is interesting rather than merely excluded.

**[open]** That leaves the **Downs** as the one entry owing nothing, which under a soft-gate design
means nothing ever requires a player to be there. For the starter region that looks harmless — you
wake up standing in it — but the cost lands on the *return*: in a frontier loop a biome holding
nothing is one you never come back to, so the one region the §7 guarantees spend real effort on is
abandoned by the third hour.

**Proposal: the Downs owes volume rather than tier.** Bulk timber and stone — the thing needed in
quantity forever rather than a tech unlock taken once. Transport restriction makes this stronger than
it first appears: if the safest region is also the bulk supply, it is the natural anchor for a
fast-travel network, and the hub a frontier needs in order to be a frontier rather than a drift.

---

## 6. Creatures: rosters, levels and travel

**[proposed — this is the part that reaches furthest outside the engine as it exists.]**

### The model

Each biome owns a **roster**. Each roster entry carries a minimum threat, a weight curve over
threat, and a level range. One mechanism therefore produces both effects:

* **Composition shifts with threat.** Entries with a high minimum simply are not present near spawn.
  A far fen is not a near fen with the numbers turned up — it has residents the near fen never had.
* **The ceiling rises; the floor does not.** Threat raises the *top* of what can roll and skews the
  distribution upward. A high-threat region can still hand you something trivial. This is what keeps
  a region feeling like a place rather than a difficulty slider, and it is the opposite of flat level
  scaling, where a wolf at nine is a wolf at one with a multiplier.

**Roster entries also carry whether they can be tamed**, because logistics runs on animals before it
runs on machines (§2). That is not a second system — a tamed creature is the same entity with a
different controller — but it does mean every biome's roster should be designed with the question
"what here is worth taking home alive?" rather than only "what here kills you?".

### Travel across borders

A creature is native to a biome only at the moment it spawns. After that it is an entity in the live
simulation and moves by its own logic — it can be lured, it can flee, and it can cross a border.

This is worth more than it looks:

* **Borders become porous**, which is what makes them read as real places rather than invisible
  walls. A pack chasing you out of the pinelands and into the downs is a story; a pack stopping dead
  at a line is a bug the player can see.
* **Luring becomes a player verb.** Safety is never absolute, and the player gets a tool: pull the
  big thing into the fen where the footing ruins it, or away from the thing you actually want.
* **Fleeing is the inverse**, and it gives the world a way to deny a kill.

Two rules this needs, both cheap and both easy to get wrong:

1. **Level is baked at spawn and never recomputed from position.** If a creature's power tracks the
   threat field it is standing in, luring is meaningless and a fight changes mid-swing.
2. **Leaving the live area is a decision, not an accident.** Despawn, freeze in place, or persist —
   each gives a different game, and a lured creature crossing that boundary is the case that exposes
   which one you chose.

**[open]** The obvious exploit: lure high-level creatures inward to farm them near a safe base. Worth
deciding early whether that is a problem or a legitimate high-skill play.

### What does not exist yet

There is no AI, navigation or entity system in the engine. The modules are `animation`, `building`,
`core`, `meshing`, `physics`, `render`, `terraform`, `world`. ozz is wired for skeletal animation and
Jolt for rigid bodies, so creatures have bones and collide — but nothing moves anything with intent,
and there is no navigation representation of the terrain at all.

With danger decided as creatures (§2), this is no longer a future concern — it is the critical path,
and everything in this section is a table rather than content until it is solved.

**Static navmesh baking is not available.** The player can dig the floor out from under a path at 1 m
resolution, and `DeltaStore` makes that edit permanent.

| Approach | Fit | Against |
|---|---|---|
| Grid / sparse-voxel A* straight off the density field | No bake at all; always current | Memory and search cost at range; poor at long surface travel |
| **Per-chunk navmesh, incrementally rebaked on edit** | **Rides a pipeline that already exists** | Rebake latency; stitching across chunk borders, and across LODs |
| Steering with local avoidance, no global path | Cheapest by far | Creatures get stuck in caves — which is where the content is |
| Hierarchical: coarse inter-chunk graph + local steering | Scales to 22 km | Two systems to build and keep agreeing |

**The second option deserves the first look, because the engine already does this shape of work
twice.** A chunk edit invalidates geometry and collision, and `TerrainCollisionBaker` plus the
collision sub-mesh out of `MeshOptimizerStage` already rebuild a simplified representation on exactly
that signal. Navigation is a third consumer of the same event, with the same chunk granularity, the
same invalidation and the same async budget. The long-range half of the problem then wants the
hierarchical graph on top — which the coarse world tier (§8) is already the natural home for.

---

## 7. Guarantees

**[proposed]**

Some properties should hold for every seed, and the cheapest way to get them is to enforce rather
than hope. The coarse world-map tier (see §8) is cheap enough to generate and test before a world is
accepted, so these can be checked by rejection sampling.

* A **starter region** at the centre: buildable flat land, fresh water, shelter, a shallow cave, no
  rare biome inside its radius, and **no climate hazard** — the player begins with no protection of
  either kind, so the first region has to ask for none. Stamped deliberately, not hoped for.
* **No dead starts**: the starter region must connect by land to at least two different climate
  directions, so the first real choice is which way to go, and must not be enclosed by water (§2).
* **Every tech tier is reachable.** Once the ladder in §2 is carried by scavenged set pieces rather
  than crafted from scratch, a seed that fails to place a tier's prefabs is a seed that cannot be
  finished. This is new, and it is the strictest validation requirement in the document — a
  progression gate is only soft if the thing behind it exists.
* **Legibility, which is now only half a problem.** The climate axis announces itself: snow is cold,
  desert is hot, and a player reads both from a ridgeline. The radial axis does not — a fen at 9 km
  looks exactly like a fen at 2 km, by design (§3). See below.

### Legibility: reconnaissance, not signage

**[proposed]** The answer to the radial gradient is **elevation**, not iconography.

Terrain cannot encode radial threat — that is the whole point of §3, and painting warning signs on it
would undo the decoupling. But a player who gets high enough can *observe* it: movement on the plain
below, kills, smoke, how much is moving at dusk. So the channel is reconnaissance, and it has three
properties that make it the right one rather than merely an available one:

* **It is earned, not given.** Climbing a ridge or building a tower costs time and exposure.
* **It stays soft.** You can walk in blind. You simply do not know what you are walking into — which
  is the same risk/reward shape as every other gate here.
* **It activates things that already exist.** 1,536 m of vertical relief stops being scenery and
  becomes an information resource. Building high becomes a real structural-integrity problem rather
  than decoration — which is exactly what the Highland biome's material is for (§5).

It also makes a watchtower the first thing a player builds at a new holding, which is precisely the
rhythm the frontier loop wants.

**[open]** What is actually observable at range? Creature silhouettes are the obvious answer and the
expensive one — they would need entities simulated far outside the live area (§6). The cheap
alternative is proxies that stand in for the simulation: carrion birds, smoke, tracks, lights, calls
at night. Proxies are almost certainly right, and they have to be generated from the same spawn
tables the live simulation would use, or the reconnaissance lies.

**[open]** If a seed fails validation, does the player get told, or is the seed silently advanced to
the next passing one? Silent advancement makes the seed they typed not the seed they got, which is
confusing when they try to share it.

---

## 8. What this demands of the generator

**[proposed]**

**The coarse world-map tier owns biome and threat, at 64 m cells.** That makes the whole world
353 × 353 — trivial to generate eagerly and cache. The cell size is settled by the biome patch
diameter rather than by cost: see §1, *Scale and pacing*. Biome assignment, climate and hazard
fields, the warped threat field, rarity weights, hydrology and set-piece slots all live here.

**Biome and threat must be queryable at runtime, at any position, cheaply.** This is a new
requirement that §6 creates. Spawning is a function of `(biome at point, threat at point)` evaluated
wherever a creature appears — not at chunk generation time, and not from meshed geometry. The coarse
tier has to be a resident, O(1)-lookup structure, not a generation-time scratch buffer. At 353 × 353
that is trivially affordable, which is a point in favour of the finer cell size.

| Consumer | What it needs |
|---|---|
| Height / terrain shaping | Blended terrain parameters per biome — **never threat** |
| Material assignment | Surface and subsurface material palette |
| Tunnel pass | Cave character, density, and whether tunnels exist at all here |
| Set pieces | Which prefabs are legal, and placement constraints (radius-weighted) |
| Spawning | Biome roster + local threat, queried at runtime |
| Resources | A material each biome owns exclusively — the thing that moves the frontier (§2) |
| Hazard | Cold/heat magnitude per cell, driving the preparation gate (§2) |
| Navigation | A coarse inter-chunk graph for long-range pathing (§6) |

**Hydrology must be placed, not emergent.** §2 makes large water bodies a soft gate, and a plain
sea-level cutoff over fractal noise will not reliably put a lake between the player and somewhere
worth reaching. Lakes, rivers and **land connectivity as a queryable property** belong on the coarse
tier — connectivity especially, because "is this reachable on foot, and if not what is the detour"
is a question the starter guarantees, the seed tooling and the navigation graph all need to ask, and
none of them can afford to answer it by meshing.

**Seed tooling constrains what may live where.** Any property a player should be able to search seeds
for has to be computable from the coarse tier alone — "2,100 m peak", "Hollows within 5 km", "river
delta at spawn", "four biomes within 1 km". If judging a seed requires meshing chunks, nobody builds
those tools, including us. The biome/threat split makes this *easier*: a seed is now describable
entirely in coarse-tier terms.

**Terrain needs chokepoints, not just variety.** Transport restriction (§2) only produces interesting
logistics if the land forces choices — a range that is a barrier *with passes*, a river that needs a
ford or a bridge, a ridge worth tunnelling rather than climbing. Uniform fractal relief gives none of
those, because everywhere is equally crossable. Passes, fords and corridors have to be identified on
the coarse tier alongside hydrology, and the terrain tier has to honour them. This is the difference
between a map you route across and a map you merely walk over.

**Rails make gradient a first-class terraforming concern.** Tier 2 (§2) turns player-cut routes into
railways, and a railway cares about something a footpath does not: continuous, bounded slope. That
asks for two things the terraform tools do not have today — editing that works along a *path* with a
target grade rather than a brush at a point, and a query that answers "is this route gradeable, and
where does it fail". It is also the clearest argument yet that terraforming needs higher-level verbs
than add and subtract.

**Powered machines run while nobody is watching.** Tier 2 means things that work while the player is
elsewhere, which puts a second claimant alongside creatures on the question of what happens outside
the live area (§6). Creatures can plausibly despawn; a pump that stops when you look away is a
broken promise. The likely answer is that machines resolve by *bookkeeping* rather than simulation —
elapsed time in, result out — but it has to be decided once for both.

**Auto-mining is the hard case, because it eats terrain.** A miner resolved by bookkeeping still has
to put the hole *somewhere*: terrain is permanent, so "eight hours elapsed, four hundred ore" implies
four hundred voxels gone from specific places. That needs a deterministic excavation pattern the
machine follows, replayable from elapsed time alone and written through `DeltaStore` like any other
edit. Get it wrong and the player returns to a mine whose geometry does not match its output — or
worse, one that regenerates. Auto-farming has the same problem in miniature: a field is terrain
carrying a soil property, so whether flattening a hillside produces farmable ground has to be an
answer rather than an accident.

**Set pieces are now progression content, not flavour.** If the tech ladder is scavenged rather than
crafted (§2), prefabs are the physical form of the tech tree. That raises the authoring effort they
deserve, makes their placement a validation requirement rather than a nicety (§7), and means each one
has to be worth a *single* visit — fast travel to visited places means most regions are truly
explored exactly once.

**Set pieces should be parametric, not baked.** The engine composes SDFs with smooth CSG already, so
a prefab chamber blends into procedural rock without a seam — something a block-stamping engine
cannot do. Parametric recipes (chambers as ellipsoid unions, tunnels as capsule chains) stay
resolution-independent across LODs; baked voxel volumes do not, and should be reserved for genuinely
bespoke one-offs.

**Version the generator now.** `DeltaStore` stores edits relative to regenerated base density, so any
change to generation silently corrupts existing saves. Pin a generator version in the save file
before there is a world worth keeping. We are about to enter a long period of changing the generator
constantly.

With co-op decided (§2) this stops being only a save-file concern. The version becomes a
**compatibility check between clients** — two players on different generator versions regenerate
different base density, so the same replicated delta produces different terrain on each machine, and
the divergence is silent. `DeltaStore` also has to replicate at all, which makes it shared
authoritative state rather than a local log.

---

## 9. Open questions, ranked

1. **What is the navigation representation?** (§6) Now the critical path, not a future concern.
   Danger is creatures, creatures must path, and permanent 1 m edits rule out static navmesh baking.
   Nothing else here is blocked on a problem this size. Start from the per-chunk rebake option,
   because `TerrainCollisionBaker` already proves the pipeline shape.
2. **What is observable at range?** (§7) Reconnaissance is the legibility answer, but creature
   silhouettes at 2 km mean simulating entities far outside the live area. Proxies — birds, smoke,
   tracks, lights — are almost certainly right, and must derive from the same spawn tables the live
   simulation uses or the reconnaissance lies to the player.
2. **Can the haul be a gauntlet rather than a walk?** (§2) Promoted, because the pillars gave it a
   pass/fail criterion instead of a worry. Tamed animals and routes that improve with investment are
   the two answers on the table, and neither has been tested.
3. **What is observable at range?** (§7) Reconnaissance is the legibility answer, but creature
   silhouettes at 2 km mean simulating entities far outside the live area. Proxies — birds, smoke,
   tracks, lights — are almost certainly right, and must derive from the same spawn tables the live
   simulation uses, or the reconnaissance lies to the player.
4. **What happens outside the live area?** (§6, §8) Creatures, pack animals and machines are three
   claimants on one answer that want different things. Auto-mining is the sharp edge: bookkeeping
   still has to put the hole somewhere specific, permanently.
5. **Does combat terraforming have a pit exploit?** (§2) Digging a hole nothing can reach you in is
   the obvious degenerate strategy, and the counters — climbers, diggers, ranged attackers, collapse
   under load — are real work that should be scoped before the verb is promised.
6. **Permanent hub or genuinely nomadic — and what do the Downs owe?** (§2, §5) One question, not
   two. Transport restriction wants a stable anchor for the fast-travel network; a bulk material in
   the safest region supplies both at once. Composing tiers makes the case stronger still.
7. **Do tamed animals stay relevant once machinery arrives?** (§2) Animals go where machines cannot;
   machines work while you are elsewhere. Keeping both alive is the better game and two systems to
   balance forever.
8. **How steep is the rare-biome radius ramp?** (§3) Too shallow and the far world is diluted; too
   steep and it is the danger grid again wearing a probability distribution.
9. **Does `ChunkStreamer` become multi-focus, and when?** (§2) It takes one `focus` today. Co-op
   needs a desired-set that is the union over several, each node at the finest LOD any focus asks
   for. Cheap to do now, awkward once LOD selection has grown features around a single point.
10. **Accept the co-op infrastructure discount, or let consumption absorb it?** (§2) The second
    needs nothing new, which is the argument for it, but neither has been played.
11. **Smaller, but still open:** is there a win condition (§2), is the map edge ocean (§1), and is
    seed rejection visible or silent (§7)?

*Settled so far: the pillars — exploration and combat (§2); single-player and co-op both supported,
with density but never tier scaling by party size (§2); the automation ceiling — machines never feed
machines (§2); four tech tiers and their ordering (§2); transport restriction and the fauna-then-
machinery logistics ladder (§2); coarse tier cell size, 64 m (§1); the tunnelling-bypass worry
(dissolved by transport restriction).*
