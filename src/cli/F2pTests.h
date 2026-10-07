#ifndef WORLDWALKER_CLI_F2PTESTS_H
#define WORLDWALKER_CLI_F2PTESTS_H

// `wwcli f2p [<artifact.wwa> [<dataset_dir>]]` - free-to-play routing
// (WW_RESTRICT_FREE_TO_PLAY).
//
// Without an artifact: the free-to-play land geometry, the members rule, the
// snapshot's handling of the bit, the loader's `"members"` flag and the
// shipped datasets (f2p_zones.json against fixed test tiles, the lodestone
// flags against the cache enums). Self-contained, for CI.
//
// With an artifact that has the F2pZones section: the planner, through the C
// ABI, with the runtime teleports from <dataset_dir> (default: the datasets/
// above the working directory) - charters and the Taverley Dungeon entrance
// refused, members goals refused at plan time, the positive controls arriving
// on free-to-play ground only, and the bit-clear plans byte-identical to a
// plain ww_query_ex. An artifact without the section prints "planner SKIP",
// never PASS, after checking what the bit still does there.
//
// Returns 0 when every check that ran passed.
int runF2pTests(const char *wwaPath, const char *datasetDir);

#endif  // WORLDWALKER_CLI_F2PTESTS_H
