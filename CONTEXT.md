# WorldWalker

The pathfinding context for RuneScape 3 (NXT engine). WorldWalker is the **producer** of walkability: it reads the RS3 cache, builds a collision model of the world, and answers "how do I get from A to B" with a sequence of movement and interaction steps. Consumers (the injected client agent, the Java host) ask it for paths; it owns no game state of its own.

## Language

**Tile**:
The atomic unit of position and movement: a single `(x, y, plane)` cell of the world grid. RS movement is tile-by-tile, 8-directional.
_Avoid_: square (means Mapsquare), cell, coordinate.

**Plane**:
The vertical level of a Tile, `0`–`3`. Ladders, stairs, and trapdoors move between Planes; teleports may too.
_Avoid_: level (overloaded with skill level), floor, height (height is terrain elevation, a different thing), z.

**Mapsquare**:
A `64 × 64`-Tile region of the world, the unit in which map geometry is stored and streamed in the cache. Addressed by its `(squareX, squareY)`.
_Avoid_: region (ambiguous), chunk, zone, scene.

**Loc**:
A scenery object — door, ladder, tree, wall, gate. Two distinct senses that must never be conflated:
- **Loc definition**: the *type* (its `name`, `options`, footprint `sizeX/sizeY`, `solidType`, `interactType`). Comes from NXTCacheLibrary's `loc` config getter. One per object id.
- **Loc instance**: a *placement* of a definition at a specific `(x, y, plane)` with a rotation and shape. Comes from decoding the Mapsquare's landscape data. Many per definition.
_Avoid_: object (overloaded), entity, scenery (use only informally).

**Collision map** (a.k.a. clip flags):
The per-Tile, per-Plane clip word capturing engine-faithful blocking: whole-tile blocked, the four wall-edge blockers (N/E/S/W), and corner/diagonal blockers. Derived from Mapsquare terrain flags plus blocking Loc instances. Movement respects wall edges and forbids corner-cutting. The core input to pathfinding.
_Avoid_: clipping (verb), blockmap, navmesh (a navmesh is a different representation we are not committed to).

**Chunk**:
An `8 × 8`-Tile block — the unit a Dynamic region is assembled from, and the only sense in which this context uses the word. Note the Mapsquare entry above tells you to avoid "chunk" *as a synonym for Mapsquare*; a Chunk is an eighth of a Mapsquare on each axis, not another name for one.

**Dynamic region** (a.k.a. instance):
A scene the client builds at runtime by copying Chunks out of the static map into a scratch area of the world, optionally rotated: a player-owned house, a Dungeoneering floor, a boss or minigame instance. The one place this context says "region" rather than Mapsquare — it is the wire's term, published by the agent, and renaming it here would break the shared vocabulary for a purely local preference.
_Avoid_: instanced map, procedural region (nothing is procedurally generated — every Tile is copied from somewhere).

**Descriptor grid**:
The plane-major table the client publishes for a Dynamic region: one packed descriptor per (plane, Chunk) naming the **Source chunk** it was copied from and its rotation, or a **Hole**. The UNITS TRAP lives here — the grid's origin is in Mapsquares while its dimensions are in Chunks.
_Avoid_: chunk table, layout, mapping.

**Source chunk / Source tile**:
The static-map Chunk a Dynamic region's Chunk was copied from, and the corresponding Tile within it. Collision inside a Dynamic region is the Source tile's baked clip word with its directional bits rotated. Coordinates crossing WorldWalker's API are always instance coordinates — Source tiles exist only inside the lookup.
_Avoid_: original tile, template tile, real tile.

**Hole**:
A cell of the Descriptor grid with no Source chunk. Reads as fully blocked: a Hole in a Dynamic region is genuinely solid, not merely unknown.
_Avoid_: gap, empty chunk, unmapped (unmapped means "outside any baked Mapsquare", a different condition with the same clip answer).

## Pathfinding

**Tick**:
The RS3 game's fixed time quantum (0.6 s) and the cost unit the search minimizes. Walking covers ~2 Tiles/Tick when running (the assumed default); each Transition has a fixed Tick cost. Run-energy is not modeled in the cost graph.
_Avoid_: turn, step (a Step is a path element), frame, ms.

**Cluster**:
A fixed rectangular partition of one Plane's tile grid (HPA*). The unit over which intra-cluster portal-to-portal distances are precomputed.
_Avoid_: block, sector, tile-group.

**Portal**:
A border crossing between two adjacent Clusters, and a node in the abstract graph. Walking the grid is refined within Clusters; the abstract search hops Portal to Portal.
_Avoid_: entrance (the HPA* paper's word — we say Portal), gateway, node.

**Transition**:
A non-walking move between two Tiles that are not grid-adjacent — ladder, stair, door/gate, teleport, lodestone, agility shortcut. Modeled as an edge in the abstract graph carrying an action to perform, a destination Tile, and requirement predicates. Distinct from ordinary tile-to-tile walking. Two classes by origin:
- **Point-to-point Transition**: a specific origin Tile → specific destination Tile (stairs, ladders, fairy rings, dungeon entrances, boats), even when the destination is in a distant Mapsquare. A baked directed edge in the abstract graph.
- **Global teleport**: usable from (almost) any teleport-allowed Tile → a fixed destination (lodestones, home teleport, `global` spell teleports). Never baked as per-cluster edges (that would explode the graph); instead injected at query time by seeding the search frontier with the enabled destinations.
_Avoid_: link, jump, warp, shortcut (a shortcut is one *kind* of Transition).

**Teleport-allowed**:
A property of a Tile/area: whether a Global teleport may be initiated from it. False in the deep Wilderness (above the cutoff) and designated no-teleport areas. Gates query-time frontier seeding.
_Avoid_: teleblock (a specific PvP mechanic), no-port.

**Transition kind**:
The category of a Transition (fairy ring, ladder, stair, door, agility shortcut, boat, lodestone, spell teleport, item teleport, ...). Drives per-kind enable/disable toggles in the Capability snapshot and the Transition's cost, complementing the structured Requirement predicate.
_Avoid_: type (overloaded with config types), group, category.

**Requirement predicate**:
A baked, structured condition gating a Transition, evaluated at query time against the Capability snapshot. Real predicates exist where the data has them — `skill{id, level}`, `items[{id, count}]`, `varbit{id, value}`, membership — chiefly on spell/item/lodestone teleports. Scenery and fairy-ring Transitions usually carry none and are unconditional (subject only to a per-kind toggle).
_Avoid_: requirement (bare), condition, gate, constraint.

**Capability snapshot**:
The bundle of player state the search evaluates Requirement predicates against — skill levels, item/rune counts, varbit values, membership, lodestone unlocks, and the set of enabled Transition kinds. The Planner receives it as a query argument; the Executor obtains a fresh one through a Primitive at each re-plan, so a re-plan reflects state that changed mid-walk (runes just banked, a level gained, crossing into no-teleport ground). The Planner reads no game state itself — it only ever sees a snapshot.
_Avoid_: player state, context, profile, game state.

**Execution chain**:
The ordered interface/click/wait actions needed to carry out a Transition (e.g. open a teleport interface, click a component, wait N ticks). Sourced from the dataset and attached to a Transition; run by the Executor via Primitive callbacks.
_Avoid_: macro, script, sequence, recipe.

**Planner**:
The pure side of WorldWalker: a function of (artifact + start + Goal + Capability snapshot) that returns a Path. Reads no game state and performs no actions.
_Avoid_: solver, engine, router.

**Executor**:
The orchestrating side of WorldWalker: it drives a Path to completion within a single consumer invocation that runs until the Goal is reached, the walk fails, or the caller cancels — sequencing Steps, running Execution chains, timing waits, detecting arrival, and re-invoking the Planner itself when it drifts, gets stuck, or reaches a teleport-allowed Tile. Written once in C; it reads live player state, performs actions, checks for cancellation, and reports progress only through Primitive callbacks the consumer supplies, holding no game state of its own.
_Avoid_: runner, driver, walker (the whole library is the walker), interpreter.

**Primitive (callback)**:
A thin consumer-supplied function the Executor calls to reach the game — reads (player position, the live Capability snapshot, a varbit, whether an interface is open), actions (walk to a Tile, interact with a loc, wait N Ticks), a cancellation check, and a progress/event report. The only game-facing surface; a new consumer language implements just these, never the Executor logic.
_Avoid_: hook, action, command, op.

**Path**:
The result of a query: an ordered list of Steps from a start Tile to a goal Tile.
_Avoid_: route (use informally only), trail, plan.

**Step**:
One element of a Path. Either a WALK step (a target Tile chunked to stay within the client's local walk-to range) or a Transition step (a loc/spell/item to interact with plus its destination Tile). The consumer executes Steps in order.
_Avoid_: waypoint, node, move, leg.

**Goal**:
The target of a query: an acceptance set of one or more Tiles. The search returns the cheapest Path reaching any of them. A single Tile, "nearest bank/lodestone" (the set of their Tiles), and "within interaction range" (the ring of Tiles around an object) are all expressed this way.
_Avoid_: destination (a Transition has a destination Tile — different), target, endpoint.

**Transition dataset**:
The externally-curated data supplying Transition destinations, costs, Requirement predicates, and Execution chains that the cache cannot yield — scenery transition links, fairy-ring / npc / object teleport chains, and spell/item/lodestone teleports. It is authoritative over cache-derived Transitions on conflict. Cache derivation only auto-pairs unambiguous vertical ladders/stairs to catch geometry added in a game update before the dataset covers it.
_Avoid_: transports file, teleport table.
