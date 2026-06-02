#ifndef WORLDWALKER_CLI_DOORPATHS_H
#define WORLDWALKER_CLI_DOORPATHS_H

// `wwcli doors <artifact.wwa>` — exercise pathing across doors/gates.
//
// A "door" here is a local-origin Transport transition that stays on the same
// plane and hops a short distance through a wall (originPlane == destPlane,
// small Chebyshev origin->dest). That isolates doors/gates from the other
// Transport links (ladders/stairs change plane) and from teleports (global
// origin / large hop). For each door that splits two areas — i.e. the wall
// makes the two sides un-walkable to each other — the harness plans a route
// from a standable approach tile on the origin side to the destination tile
// and asserts the plan crosses a door (a same-plane Transport Transition
// step), with a walk-only control confirming the wall is load-bearing.
//
// Returns 0 when every tested door was traversed, 1 on any failure.
int runDoorPaths(const char *wwaPath);

// `wwcli doorprobe <artifact.wwa> <txIndex>` — dump the local collision
// geometry (per-tile area id + standability, plus the origin's wall edges)
// around a transition's origin and destination, for diagnosing why a door
// failed to wire as an area-graph edge.
int runDoorProbe(const char *wwaPath, int txIndex);

#endif  // WORLDWALKER_CLI_DOORPATHS_H
