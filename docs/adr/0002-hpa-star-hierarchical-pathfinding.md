# Hierarchical pathfinding — connected-component area graph (HPA*-cluster deferred)

Flat tile-grid A* across the whole RS3 world is too slow for long routes. The planner needs an abstraction that A* can search cheaply, with tile-level refinement *inside* each abstract node. Two designs were on the table when planning began (see `lovely-forging-honey.md` — "Abstraction layer decided empirically"):

- **HPA*-cluster** — partition each plane's tile grid into fixed-size clusters, expose border entrances ("portals") with precomputed intra-cluster portal-to-portal distances, and search the portal graph.
- **Connected-component area graph** — flood-fill each plane into walkable connected components ("areas"); each area is one abstract node; transitions are edges between areas (including cross-plane and cross-mapsquare edges). This is the structure the prior `BotWithUs2` nav stack used (see `E:/BotWithUs V2/BotWithUs2/area_gen/`).

Special transitions — teleports, ladders, stairs, doors — are modeled the same way under both designs: as additional edges in the abstract layer, rather than a separate routing tier. We preferred that generic structure to a bespoke anchor-graph or a single flat grid + special-edge search.

## Decision

**Adopt the connected-component area graph. Defer HPA*-cluster as Phase 7+ work, to be revisited only if the perf budget tightens.**

The Phase 6d benchmark (`wwcli bench`) measured the area-graph planner end-to-end on the development artifact (46,589 areas, 9,657 transitions, 16 ALT landmarks). Median latency per `PathAssembler::assemble` call:

| Category | n | succeeded | median | mean | p99 |
|---|---|---|---|---|---|
| same_area refinement | 50 | 50 | 7.9 µs | 42.9 µs | 304 µs |
| short cross-area hop | 50 | 45 | 371 µs | 5.1 ms | 36.7 ms |
| long random pair | 50 | 0 | 138 µs | 148 µs | 266 µs |

The long-category zero-success rate is a property of the artifact (the world has many small disconnected components — overworld, dungeons, instances), not a planner bug; unreachable probes terminate in sub-300 µs. The executor polls the planner once per game tick (~600 ms); typical-case 8–400 µs and p99 < 40 ms leave several orders of magnitude of headroom. There is no perf signal that justifies the cost of building a second abstraction.

## What we lose by deferring HPA*

- **Tighter intra-cluster locality.** A fixed-cluster HPA* would partition long areas (e.g. the whole Wilderness floor) into smaller refinement zones, reducing tile-level A* search size on within-area routes. The area graph relies on per-area centroids + the tile A* finishing the last segment — fine at our area sizes; if a future map update produces giant flood-fill components, we revisit.
- **Cluster-portal distance precomputation.** HPA* bakes portal-to-portal cost matrices that act like a denser ALT table; we approximate with 16 ALT landmarks (Phase 2 `AltLandmarks`).

## Trigger conditions to revisit

Reopen this ADR if any of the following hold:

1. p99 of `wwcli bench short_hop` exceeds 250 ms on a target machine (game-tick budget begins to matter).
2. A future cache snapshot produces a single area whose tile span is so large that intra-area tile A* dominates query latency.
3. A new consumer needs sub-100 µs *cross-world* planning (e.g. real-time UI hover paths). Today's consumers are the bot executor (one call per tick) — they don't need it.

## References

- Bench source: `E:/BotWithUsv2.5/WorldWalker/src/cli/Bench.cpp`
- Area graph build: `E:/BotWithUsv2.5/WorldWalker/src/build/AreaGraph.cpp`
- ALT bake: `E:/BotWithUsv2.5/WorldWalker/src/build/AltLandmarks.cpp`
- Original "prototype both" decision: `lovely-forging-honey.md` — "Abstraction layer decided empirically"
