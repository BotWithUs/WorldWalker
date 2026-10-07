#ifndef WORLDWALKER_CLI_DESCRIBETESTS_H
#define WORLDWALKER_CLI_DESCRIBETESTS_H

// `wwcli describe <artifact.wwa> <dataset_dir>` - ww_transition_describe
// against the datasets the artifact was baked from. Every enabled
// transport_links row and every spell / item / lodestone teleport is matched
// to its artifact record, and each field the description carries is checked
// against the JSON row it came from, read by an oracle written apart from the
// loader that baked it. A handful of named rows (a gate, an NPC charter, a
// skill-and-varbit gated chain, a teleport of each kind) must be found and
// pass. Refuses to compare, and returns 2, when the artifact's provenance
// says it was baked from different dataset bytes. Returns 0 when every check
// passes, 1 otherwise.
int runDescribeTests(const char *wwaPath, const char *datasetDir);

#endif  // WORLDWALKER_CLI_DESCRIBETESTS_H
