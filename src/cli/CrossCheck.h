#ifndef WORLDWALKER_CLI_CROSSCHECK_H
#define WORLDWALKER_CLI_CROSSCHECK_H

// Phase 6a entry point: diff the clip words baked into a WorldWalker artifact
// against the prior nav-stack's collision_map.bin (COLL/v8) for every map
// square the oracle has data for. Returns 0 on success (regardless of how many
// mismatches were found — count is summarized to stdout), 1 on I/O or format
// failure.
int runCrossCheck(const char *wwaPath, const char *oraclePath);

#endif  // WORLDWALKER_CLI_CROSSCHECK_H
