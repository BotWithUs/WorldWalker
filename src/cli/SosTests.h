#ifndef WORLDWALKER_CLI_SOSTESTS_H
#define WORLDWALKER_CLI_SOSTESTS_H

// `wwcli sos <artifact.wwa>` - the Stronghold of Security rows
// (datasets/README.md, "Stronghold of Security"), planned through the C ABI:
//   - Barbarian Village down to the War reward room, then each reward room
//     down to the next (Famine, Pestilence), and Pestilence back out to the
//     surface, every route on plane 0 and through the row it must take;
//   - each start-room portal is planned only with its level's reward varbit
//     at 1, and with it the start room reaches the reward room through it.
// The artifact is only read. Returns 0 when every check passes.
int runSosTests(const char *wwaPath);

#endif  // WORLDWALKER_CLI_SOSTESTS_H
