#ifndef WORLDWALKER_CLI_WALLSHAPETESTS_H
#define WORLDWALKER_CLI_WALLSHAPETESTS_H

// Phase 6b entry point: unit tests over the production shape/rotation -> wall-bit
// table, ww::data::doorEdgeMask (src/data/DoorEdges.h). Returns 0 when all
// invariants hold, 1 on any failure (with the offending case printed).
//
// doorEdgeMask is what the crossing deriver gates door-hop emission on, so a
// single-side regression there mints or suppresses real transitions. This test
// drives that function directly — it does NOT restate the table and assert on
// its own copy, which is what it used to do and which left production on no
// test's path at all.
//
// Three things are checked, in increasing independence from the function:
//   * agreement with the vendored expected values from the prior reference at
//     E:/BotWithUs V2/BotWithUs2/shared/src/collision/collision_map.cpp:91
//     (function getWallFlagsForShape) — the absolute anchor;
//   * the reflection property — reflecting a shape's edges across the tile
//     yields the same shape turned 180 degrees;
//   * the rotation property — rotation N is rotation 0 turned 90 degrees N
//     times, and only the low two bits of the rotation field matter.
//
// The producer-side decoder (NXTCacheLibrary) stamps these flags into the
// artifact at build time; the artifact's empirical agreement with the prior
// nav-stack oracle is validated separately by 6a (`wwcli crosscheck`), and
// chunk rotation inside a dynamic region by `wwcli instance`. This suite covers
// only loc wall SHAPES.
int runWallShapeTests();

#endif  // WORLDWALKER_CLI_WALLSHAPETESTS_H
