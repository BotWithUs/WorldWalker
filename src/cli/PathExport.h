#ifndef WORLDWALKER_CLI_PATHEXPORT_H
#define WORLDWALKER_CLI_PATHEXPORT_H

// `wwcli path` entry point: plan a route through the runtime's PathAssembler
// and emit it as JSON for consumption by the standalone Leaflet viewer in
// tools/viewer/. Capabilities default to fully permissive (every requirement-
// gated transition admitted); a future flag can load a snapshot from JSON.
//
// `args` is the argv slice immediately after the subcommand token, i.e.
// argv + 2 for the canonical invocation
//   wwcli path <artifact.wwa> <fromX> <fromY> <fromPlane>
//              <toX> <toY> <toPlane> [--out path.json]
//
// Returns 0 on a planned path, non-zero on parse failure / load failure /
// no-route-found / write failure — scriptable.
int runPathExport(int argc, char **args);

#endif  // WORLDWALKER_CLI_PATHEXPORT_H
