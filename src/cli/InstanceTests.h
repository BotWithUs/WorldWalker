#ifndef WORLDWALKER_CLI_INSTANCETESTS_H
#define WORLDWALKER_CLI_INSTANCETESTS_H

// Dynamic-region (instance) unit tests. Returns 0 when every invariant holds,
// 1 on the first failure, with the offending case printed.
//
// These are the layer that must be right before anything is trusted in-game.
// Instance collision is resolved by remapping each tile onto the static chunk it
// was copied from, and the two halves of that remap fail in opposite ways: a
// wrong tile resolve sends the planner somewhere obviously wrong, while a wrong
// wall-bit rotation is invisible everywhere except rotated chunks — and a
// player-owned house, the only instance anyone can reach on demand, copies every
// chunk unrotated. So rotation cannot be validated by walking around in one.
//
// The descriptor decode is a port, not a derivation: the Java host's
// DynamicRegion (heavily tested) and NXTLibrary's probe ResolveDynTile already
// implement the same algorithm against the same spec
// (NXTDebugger/wire/PROTOCOL.md 2.10). Cases here are chosen to pin the parts a
// port gets wrong — the mapsquare-vs-chunk units trap, hole handling, and the
// published-count bound — rather than to re-derive the format.
int runInstanceTests();

#endif  // WORLDWALKER_CLI_INSTANCETESTS_H
