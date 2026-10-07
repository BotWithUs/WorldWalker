#ifndef WORLDWALKER_CLI_F2PPLANNERTESTS_H
#define WORLDWALKER_CLI_F2PPLANNERTESTS_H

#include <filesystem>

// The planner layer of `wwcli f2p` (cli/F2pTests.h): the artifact at
// `wwaPath`, with the runtime teleports from `datasetDir` appended, planned
// through the C ABI with and without WW_RESTRICT_FREE_TO_PLAY. Prints
// "planner SKIP" (and checks only what the bit does without land) when the
// artifact has no F2pZones section. Returns the number of failed checks.
int runF2pPlannerTests(const char *wwaPath, const std::filesystem::path &datasetDir);

#endif  // WORLDWALKER_CLI_F2PPLANNERTESTS_H
