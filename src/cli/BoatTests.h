#ifndef WORLDWALKER_CLI_BOATTESTS_H
#define WORLDWALKER_CLI_BOATTESTS_H

// `wwcli boat <artifact.wwa>`: plans rowed routes over Lake Mnemosyne
// (runtime/BoatWater) through the C ABI and checks every step lands on lake
// water, that a start ashore by Ed's shed boards with one step onto the water,
// and that a walk elsewhere is planned as before. Returns 1 when any case
// fails.
int runBoatTests(const char *wwaPath);

#endif  // WORLDWALKER_CLI_BOATTESTS_H
