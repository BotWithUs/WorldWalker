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

// `wwcli txnear <artifact.wwa> <x> <y> <radius>` — diagnostic: print
// standability/area for the tile on every plane, then list every transition
// whose origin or destination falls within `radius` (Chebyshev) of (x,y) on
// any plane. Used to find out why a goal tile has no route — e.g. an upstairs
// position with no derived plane-change transition into it.
int runTxNear(const char *wwaPath, int x, int y, int radius);

// `wwcli areagrid <artifact.wwa> <x> <y> <plane> <radius>` — diagnostic: print
// the area of every tile in the window, one letter per area and '#' for a tile
// in none. Used to place a transition's origin and landing on the right floor.
int runAreaGrid(const char *wwaPath, int x, int y, int plane, int radius);

// `wwcli areastats <artifact.wwa> [<minSpan>]` — audit: per plane, how many
// areas there are and how many span more than minSpan tiles (default 256) on
// either axis, listing each such area above plane 0. No real floor spans that
// far, so a wide upper-plane area is the fill spilling over void.
int runAreaStats(const char *wwaPath, int minSpan);

#endif  // WORLDWALKER_CLI_DOORPATHS_H
