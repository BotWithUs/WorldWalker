# Precomputed baked collision/graph artifact, not runtime decode

A world walker needs global collision for the whole RS3 world (thousands of mapsquares × 4 planes), not just the regions the live client has loaded. We split WorldWalker into two components: an **offline build tool** that decodes the entire cache once into a compact binary artifact, and a **runtime query library** that memory-maps that artifact and answers path queries with near-zero startup cost.

We chose this over lazy per-mapsquare decode (which pays decode cost on cold regions and warms the whole world for global queries anyway) and over full-decode-at-startup (slow startup, high steady memory, no reusable on-disk artifact). The cost: the artifact must be regenerated per cache revision and distributed/cached to disk, and only the build tool depends on NXTCacheLibrary's map decoder — the runtime never touches the cache.
