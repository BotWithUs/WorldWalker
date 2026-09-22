#ifndef WORLDWALKER_CLI_LODESTONETESTS_H
#define WORLDWALKER_CLI_LODESTONETESTS_H

// `wwcli lodestones [<artifact.wwa>]`: the lodestone map route and the extra
// `routes` a destination can carry. Returns 0 when every check holds, 1 on any
// failure (with the offending case printed).
//
// A lodestone destination in item_teleports.json always yields the map chain
// built from `lodestones.config`, plus one transition per entry in its
// optional `routes` array — each an explicit requirements+chain pair from the
// dataset, gated on the destination's own unlock *and* its own gates. The
// shipped dataset uses one route per lodestone: the cast from the Magic
// ability book, gated on the book's lodestone filter varbit being 0.
//
// The map is deliberately left ungated, so it stays reachable for a player no
// route describes. That is the property the planner layer pins here with an
// unexpected filter value.
//
// Three layers, in increasing reach:
//   * loader:   synthetic item_teleports.json fixtures through the production
//     loadGlobalTeleports — which transitions, gates and chains each shape
//     yields, that a route's gates are ANDed onto the destination's, that the
//     map keeps no route gate, and that a malformed route fails the load.
//     Needs nothing.
//   * schema:   `requirements.varbit` in both its spellings — one object and
//     an array — and that a scalar there throws rather than silently leaving
//     the transition ungated.
//   * planner:  a fixture appended onto a real artifact through
//     loadGlobalTeleportsInto, then one query planned at three filter values:
//     0 must lead with the cast, 1 must lead with the map, and an
//     unexpected 7 must still lead with the map rather than leaving the
//     destination unreachable. Skipped when no artifact is given.
//
// Expected chains and gates are written out as literals. Nothing here rebuilds
// them from a config the way the loader does.
int runLodestoneTests(const char *artifactPath);

#endif  // WORLDWALKER_CLI_LODESTONETESTS_H
