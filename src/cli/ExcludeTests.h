#ifndef WORLDWALKER_CLI_EXCLUDETESTS_H
#define WORLDWALKER_CLI_EXCLUDETESTS_H

// `wwcli exclude <artifact.wwa> [<teleport_dir>]` - WwPlanOptions through the
// C ABI (ww_query_opts). Against the artifact's own transitions:
//   - a route that uses transition T, re-planned with T excluded, never
//     contains T: it goes round when another way exists, and is NOT_FOUND
//     when T is the only edge into the goal's area;
//   - with WW_PLAN_EXCLUDE_LOC_SIBLINGS no row of T's loc appears; without
//     it a sibling row may still be used (shown when the artifact has a case);
//   - with a teleport dir, an excluded global teleport is never seeded;
//   - NULL options, an empty struct, ww_query_ex and ww_query_moves(0) give
//     byte-identical paths, and ww_query_moves(m) matches disabledMoves = m;
//   - malformed options (size, flags, pad, NULL list) and an out-of-range index
//     are refused with WW_ERR_INVALID and a zeroed path.
// The artifact is only read. Returns 0 when every check passes.
int runExcludeTests(const char *wwaPath, const char *teleportDir);

#endif  // WORLDWALKER_CLI_EXCLUDETESTS_H
