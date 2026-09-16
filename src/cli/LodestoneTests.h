#ifndef WORLDWALKER_CLI_LODESTONETESTS_H
#define WORLDWALKER_CLI_LODESTONETESTS_H

// `wwcli lodestones [<artifact.wwa>]`: the two lodestone routes. Returns 0 when
// every check holds, 1 on any failure (with the offending case printed).
//
// item_teleports.json can describe a lodestone twice: cast from the Magic
// ability book, or picked on the lodestone map. The loader splits the two on
// the book's lodestone filter varbit so a real player is only ever offered one
// of them: the spell while the book lists lodestone spells, the map once they
// are filtered out, which is V1 nav's InteractStep LODESTONE rule.
//
// Two layers, in increasing reach:
//   * loader:  synthetic item_teleports.json fixtures through the production
//     loadGlobalTeleports: which transitions, gates and chains each shape of
//     config yields, and that a half-configured spell route fails the load.
//     Needs nothing.
//   * planner: a fixture appended onto a real artifact through
//     loadGlobalTeleportsInto, then one query planned with the filter varbit at
//     0 and at 1: the plan must lead with the spell and with the map
//     respectively. Skipped when no artifact is given.
//
// Expected chains and gates are written out as literals. Nothing here rebuilds
// them from a config the way the loader does.
int runLodestoneTests(const char *artifactPath);

#endif  // WORLDWALKER_CLI_LODESTONETESTS_H
