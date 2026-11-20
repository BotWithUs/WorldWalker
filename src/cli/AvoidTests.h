#ifndef WORLDWALKER_CLI_AVOIDTESTS_H
#define WORLDWALKER_CLI_AVOIDTESTS_H

// `wwcli avoid <artifact.wwa> [<teleport_dir>]` - avoid areas (WwPlanOptions
// V2) and route links (ww_path_links) through the C ABI. Against the artifact:
//   - a walk whose route crosses a box detours round it: no step target, no
//     line between consecutive targets and no transition end inside it;
//   - a box over a crossing the route takes keeps the route off that crossing
//     (it goes another way, or there is no route);
//   - a goal ringed by boxes, reachable only across them, has no route
//     (WW_ERR_NOT_FOUND, zeroed path), though it has one without them;
//   - a box holding the start or the goal is lifted: the path is the one
//     planned with no boxes;
//   - V1 options (structSize 32) never read the avoid fields, and malformed
//     areas or sizes are refused with WW_ERR_INVALID;
//   - ww_path_links lists every Transition step, in order, with its landing,
//     and names a runtime-loaded lodestone ("Burthorpe") but no baked loc.
// The teleport cases (a lodestone in the route, a box over its landing) need
// <teleport_dir>; without it they print SKIP. The artifact is only read.
// Returns 0 when every check that ran passes.
int runAvoidTests(const char *wwaPath, const char *teleportDir);

#endif  // WORLDWALKER_CLI_AVOIDTESTS_H
