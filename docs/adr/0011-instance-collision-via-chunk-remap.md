# Instance collision by chunk remap onto the baked artifact

Supersedes the instance half of [ADR 0006](0006-scope-overworld-and-static-dungeons.md). That ADR said extending to instances "would require blending the static artifact with live consumer-supplied layout overlays, which we are intentionally not doing in v1." This is that blend, and the reason it is now cheap is that the overlay became available: the agent publishes the client's instance chunk-descriptor grid on the wire at protocol v19, so the consumer can hand us the table that maps each instance tile back to the static tile it was copied from.

## What changed

A dynamic region is not procedurally invented terrain. The client assembles it by copying 8x8 chunks out of the *static* map into a scratch area of the world, optionally rotated 90/180/270 degrees. Every tile in a player-owned house or a Dungeoneering floor therefore has a source tile whose collision `wwbuild` already baked — the collision builder walks every map archive in the cache with no whitelist, so template regions are in the artifact whether or not anyone asked for them. Nothing needs re-baking and the `.wwa` format is unchanged.

The remap lives at exactly one place: `WorldView::clipAt` resolves the tile through `InstanceMap`, reads the *source* tile's baked clip word, and rotates its directional wall bits by the chunk rotation. Resolution happens *before* the map-square lookup, which leaves the clip cache and its sticky slot keyed in source space — so entering or leaving an instance needs no cache invalidation.

## What we deliberately did not do

**We do not plan in source space.** Translating the player's tile to its source and running the normal planner is the obvious shortcut and it is wrong: an instance is stitched from *scattered* source chunks, and two chunks that are neighbours in the instance can be map-squares apart in the real world. A source-space route would send the avatar across the overworld between them. Planning stays in instance coordinates; only the per-tile collision *read* is redirected.

**We do not remap `areaAt`.** The baked area graph is a connectivity model of the static world. The source chunks belong to unrelated areas, so remapping would stitch a graph whose edges do not exist. Inside an instance `areaAt` answers `-1` and `PathAssembler` takes a dedicated branch: tile-level A* with the area constraint off (`TileSearch::kAnyArea`), chunked into Walk steps by the same `appendWalkSegment` the static path uses. The area search, the ALT heuristic, the near-goal edge index and global-teleport seeding are all skipped rather than fed coordinates they cannot describe.

**We did not take the live-collision route.** The client keeps a real per-mapsquare collision grid that is authoritative for everything, including locs the server spawns at runtime. Publishing it would need a protocol v20 across every wire consumer, and it only covers the loaded scene. Remapping is a v19-only change that needs no new wire surface. The cost is recorded below.

## Consequences

- Instance pathing is walking only. Doors, ladders and stairs *inside* the footprint are baked as area edges, and there are no areas here, so they are not used. A house does not need them; a Dungeoneering floor will, and that is the next phase.
- No plane changes inside an instance, for the same reason — a plane change is a transition.
- Routing across the instance boundary fails with `WW_ERR_NOT_FOUND`. Exits are content-specific transitions nothing bakes yet. ADR 0006 still governs the outside-in direction: route to the entrance and stop.
- **Locs the server spawns at runtime are invisible.** House furniture and Dungeoneering doors are not in the source chunk's baked collision, so the planner will happily route through them. This is the known cost of choosing remap over live collision, and it is the trigger to revisit.
- **Spurious walls at every chunk seam.** Wall edges are baked onto the owning tile *and reflected onto the neighbour*. A source tile at chunk-local `x == 7` therefore carries reflections of walls owned by its real east neighbour — a tile that was not copied into this instance. The planner sees a wall the assembled scene does not have, at every 8-tile boundary. The error is strictly one-sided: a wall genuinely between two instance tiles is owned by one of them, both chunks are copied, and `TileSearch::tryStep` checks both endpoints, so **nothing is ever under-blocked**. The failure mode is a detour or a spurious "no route", never a route through a real wall. Unlike the runtime-spawned locs above this fires on every instance, not just furnished ones, so it is the more likely thing to be seen first.
- Rotation is unit-tested but **not field-proven**. It has never been observed non-zero in any live scene anyone has inspected, and a player-owned house — the only instance reachable on demand — is a flat 1:1 copy with rotation 0 everywhere. `wwcli instance` cross-validates `rotateClipWord` against `InstanceMap`'s own tile rotation so the two cannot silently disagree, but only a Dungeoneering floor can confirm the wire ever carries a non-zero rotation.

## Reopen triggers

1. Scripts need to path around runtime-spawned scenery inside an instance (house furniture, Dungeoneering doors) — that is the live-collision overlay, and it needs the v20 wire change.
2. A script needs to walk *out* of an instance under its own steam rather than being handed to content-specific code — that needs instance exits ingested as transitions.
3. A live scene is observed with a non-zero chunk rotation and the remap disagrees with what the player can walk — the rotation direction is derived in `rotateClipWord`'s comment and pinned by `wwcli instance`; start there.
