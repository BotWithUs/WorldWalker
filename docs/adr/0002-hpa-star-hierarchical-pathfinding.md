# HPA* hierarchical pathfinding over a clustered tile grid

Flat tile-grid A* across the whole RS3 world is too slow for long routes. We use HPA* (Hierarchical Pathfinding A*): the per-plane tile grid is partitioned into clusters; each cluster exposes border entrances ("portals") with precomputed intra-cluster portal-to-portal distances; an abstract graph of portals is searched first, then refined to tiles within each cluster.

Special transitions — teleports, ladders, stairs, doors — are modeled as additional edges in the abstract layer (including cross-plane and cross-mapsquare edges), rather than as a separate routing tier. We preferred this generic, well-understood structure over a bespoke anchor-graph or a single flat grid + special-edge search.
