#ifndef WORLDWALKER_CLI_BENCH_H
#define WORLDWALKER_CLI_BENCH_H

// Phase 6d entry point: query-latency benchmark over an existing artifact.
// Samples are drawn from the artifact's own area graph so the bench is
// portable across artifacts. Three categories — same-area refinement,
// short-hop cross-area, long-distance cross-area — each run as 50 (start,
// goal) pairs. Reports median and p99 latency per category, plus a final
// throughput line. Returns 0 on success regardless of measured numbers
// (the bench is informational; ADR 0002 records the empirical conclusion).
int runBench(const char *wwaPath);

#endif  // WORLDWALKER_CLI_BENCH_H
