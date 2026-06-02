#ifndef WORLDWALKER_CLI_WALLSHAPETESTS_H
#define WORLDWALKER_CLI_WALLSHAPETESTS_H

// Phase 6b entry point: vendored wall-shape reference table + property-tests
// that any correct directional-clip pipeline must satisfy. Returns 0 when all
// invariants hold, 1 on any failure (with the offending case printed).
//
// The producer-side decoder (NXTCacheLibrary) is what stamps these flags into
// the artifact at build time; the artifact's empirical agreement with the
// prior nav-stack oracle is validated separately by 6a (`wwcli crosscheck`).
// This test is the unit-level guard: it pins the canonical (shape, rotation)
// → expected-flags table — vendored from the prior reference at
// E:/BotWithUs V2/BotWithUs2/shared/src/collision/collision_map.cpp:91 — and
// asserts the geometric invariants (paired reflection, rotational symmetry)
// that catch a single-side table regression long before 6a's bulk diff would.
int runWallShapeTests();

#endif  // WORLDWALKER_CLI_WALLSHAPETESTS_H
