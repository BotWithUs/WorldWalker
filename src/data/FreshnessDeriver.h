#ifndef WORLDWALKER_DATA_FRESHNESSDERIVER_H
#define WORLDWALKER_DATA_FRESHNESSDERIVER_H

#include "build/CacheClient.h"
#include "build/CollisionBuilder.h"
#include "data/Transitions.h"

#include <cstddef>
#include <vector>

namespace ww::data
{
    // Counts from deriveVerticalTransitions, for build-log visibility.
    struct FreshnessReport
    {
        std::size_t pairsFound{};             // columns flagged as plane-change on both p and p+1
        std::size_t emitted{};                // candidate transitions before conflict filtering (2 per pair)
        std::size_t droppedDatasetConflict{}; // candidates suppressed because a dataset transition shares the origin
        std::size_t droppedClimbMismatch{};   // candidates whose loc climbs the other way (e.g. up-only on a down edge)
        std::size_t droppedNoOption{};        // candidates whose loc has no clickable option
        std::size_t kept{};                   // transitions returned
    };

    // Freshness fallback (ADR 0003/0009): derive vertical ladder/stair transitions
    // straight from the cache-decoded clip grid, so geometry added before the
    // datasets catch up is still traversable. A column (x, y) is paired only when
    // BOTH (x, y, p) and (x, y, p + 1) carry CLIP_PLANE_CHANGE — the unambiguous
    // same-tile / adjacent-plane case; cache derivation never invents a far-region
    // link. Each pair yields two Transport transitions (up and down).
    //
    // `datasets` is authoritative: a candidate whose origin tile + plane matches any
    // non-global dataset transition is dropped. The returned transitions are raw
    // (uncosted, unsnapped) — feed them through finalizeTransitions alongside the
    // dataset transitions. *outReport (nullable) receives the counts.
    // `crossings` supplies the loc id + click option for each plane-change tile
    // (a PlaneChange crossing at the origin tile/plane): the up/down transitions
    // are stamped with that objectId + optionIndex so the executor can climb them.
    // A pair with no matching crossing falls back to objectId == -1.
    TransitionModel deriveVerticalTransitions(const ww::build::CollisionModel &collision,
                                              const TransitionModel &datasets,
                                              const std::vector<ww::build::Crossing> &crossings,
                                              FreshnessReport *outReport);
}

#endif  // WORLDWALKER_DATA_FRESHNESSDERIVER_H
