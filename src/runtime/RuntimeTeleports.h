#ifndef WORLDWALKER_RUNTIME_RUNTIMETELEPORTS_H
#define WORLDWALKER_RUNTIME_RUNTIMETELEPORTS_H

#include "format/ArtifactReader.h"

#include <string>

namespace ww::runtime
{
    // Parse the scripter-editable global-teleport datasets (spell_teleports.json
    // + item_teleports.json) from `directory` and append them onto an already
    // open ArtifactReader. Replaces any previously appended set (truncate +
    // re-append), so this doubles as a hot reload.
    //
    // Global teleports never participate in the baked area graph; the planner's
    // frontier seeding and the executor both index the full transitions() span,
    // so appended records are picked up with no further wiring. Destination
    // validity is checked per-query at seed time (areaAt >= 0), so no collision
    // / WorldView access is needed here — a teleport whose dest is off-area is
    // simply never seeded.
    //
    // NOT thread-safe against concurrent ww_query / ww_executor_run: it mutates
    // the reader's transition pools. The caller serialises (the Java host holds
    // its lifecycle write-lock while calling).
    //
    // Returns the number of teleport transitions appended. Throws
    // std::exception (nlohmann json) on malformed JSON in a present file;
    // missing files are skipped (zero appended).
    std::size_t loadGlobalTeleportsInto(format::ArtifactReader &reader,
                                         const std::string &directory);
}

#endif  // WORLDWALKER_RUNTIME_RUNTIMETELEPORTS_H
