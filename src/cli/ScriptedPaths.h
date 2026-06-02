#ifndef WORLDWALKER_CLI_SCRIPTEDPATHS_H
#define WORLDWALKER_CLI_SCRIPTEDPATHS_H

// Phase 6c entry point: a structured pass/fail report covering four named
// path-correctness categories — long walk, plane-change traversal, teleport-
// seeded leading step, and capability gate — against the supplied artifact.
// Each category discovers its own candidate from the artifact (so the test is
// portable across artifacts cut from different cache snapshots), exercises a
// representative case, and emits a single PASS / FAIL / SKIP line. Returns 0
// when every non-skipped case passed, 1 when any case failed.
int runScriptedPaths(const char *wwaPath);

#endif  // WORLDWALKER_CLI_SCRIPTEDPATHS_H
