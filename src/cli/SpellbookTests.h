#ifndef WORLDWALKER_CLI_SPELLBOOKTESTS_H
#define WORLDWALKER_CLI_SPELLBOOKTESTS_H

// `wwcli spellbook [<artifact.wwa>]`: the active-spellbook gate on the spell
// teleports and the Trollheim tablet, against the shipped datasets/ (found
// above the working directory, as `wwcli lodestones` finds it). Returns 0 when
// every check holds, 1 on any failure.
//
// The three books share the ability book's grid component numbers (struct
// param 2975): Ardougne Teleport and the Lunar Fishing Guild Teleport are both
// 1461:330, and Trollheim Teleport, God Wars Dungeon Teleport and Tele-group
// Trollheim Farm are all 1461:450. A row the planner admits on the wrong book
// clicks a different spell, so every book-specific row carries varbit 0
// (the active book: 0 standard, 1 Ancient, 2 Lunar) == its struct's param
// 2871, and an any-book row (2871 = 3) carries none.
//
// Two layers:
//   * dataset:  every spell row's book gate, the Trollheim Farm pair's unlock,
//     and the Trollheim and Trollheim Farm tablet rows (item, landing, chain,
//     the Farm tablet's two unlock gates). Needs no artifact.
//   * planner:  the shipped teleports appended onto a real artifact, then
//     planned for a Lunar account and for one on the standard book:
//     - Yanille -> the Trollheim Farm landing pocket casts Trollheim Farm
//       Teleport on Lunar, never on the standard book;
//     - Yanille -> the dig at 2848,3684 (BWU-JARKM9) casts it too, once the
//       artifact joins the farm landing to the summit (SKIP on an artifact
//       baked before that join);
//     - the same dig breaks a carried Trollheim tablet, on any book;
//     - with neither the runes nor a tablet, neither step is planned;
//     - a carried Trollheim Farm tablet reaches the farm pocket on any book,
//       and is never planned without Lunar Diplomacy;
//     - far from the Fishing Guild (BWU-JCKQ5S), a Lunar account casts the
//       Fishing Guild Teleport, one on the standard book does not.
//     Skipped when no artifact is given.
int runSpellbookTests(const char *artifactPath);

#endif  // WORLDWALKER_CLI_SPELLBOOKTESTS_H
