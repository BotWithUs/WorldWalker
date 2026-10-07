#ifndef WORLDWALKER_RUNTIME_RUNTIMETELEPORTS_H
#define WORLDWALKER_RUNTIME_RUNTIMETELEPORTS_H

#include "data/DatasetLoader.h"
#include "format/ArtifactReader.h"

#include <cstddef>
#include <string>
#include <vector>

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
    // missing files are skipped (zero appended). A row whose requirements do
    // not parse is left out and named on stderr instead of failing the load
    // (data::RowFaultPolicy::ExcludeRow); when `outExcluded` is non-null it
    // receives those rows (replaced, not appended to), empty when none were.
    std::size_t loadGlobalTeleportsInto(format::ArtifactReader &reader,
                                         const std::string &directory,
                                         std::vector<data::ExcludedRow> *outExcluded = nullptr);

    // At most this many excluded rows are spelled out by describeExcludedRows;
    // the rest are counted. Keeps the ww_last_error text bounded however badly
    // a scripter's file is broken.
    inline constexpr std::size_t kMaxDescribedExcludedRows = 16;

    // One line for a host log naming every excluded row (up to
    // kMaxDescribedExcludedRows, then "... and N more"), e.g.
    //   "2 teleport row(s) excluded: item_teleports.json
    //    lodestones.destinations[1]: requirements: unknown key 'quest'; ..."
    // Empty when `rows` is empty. ww_artifact_load_teleports publishes it
    // through ww_last_error on a WW_OK result.
    std::string describeExcludedRows(const std::vector<data::ExcludedRow> &rows);
}

#endif  // WORLDWALKER_RUNTIME_RUNTIMETELEPORTS_H
