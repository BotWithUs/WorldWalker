#ifndef WORLDWALKER_CLI_MOVETESTS_H
#define WORLDWALKER_CLI_MOVETESTS_H

// `wwcli categories <artifact.wwa> [<teleport_dir>]` - how many transitions
// fall in each movement category (format::MoveCategory, the WW_MOVE_* bits),
// with a few examples of each, for a human to sanity-check the classifier.
// With a teleport dir the runtime-loaded global teleports are counted too.
// Always returns 0.
int runMoveCategories(const char *wwaPath, const char *teleportDir);

// `wwcli moves <artifact.wwa> [<teleport_dir>]` - the disabledMoves mask
// through the C ABI. For each category the artifact has, a query whose route
// uses one of its transitions: planned with the mask off it uses the category,
// with the category's bit set it does not (it detours or finds no route). And
// for every such query, ww_query_ex and ww_query_moves(..., 0) return the
// same path byte for byte. Returns 0 when every check passes.
int runMoveTests(const char *wwaPath, const char *teleportDir);

#endif  // WORLDWALKER_CLI_MOVETESTS_H
