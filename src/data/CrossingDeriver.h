#ifndef WORLDWALKER_DATA_CROSSINGDERIVER_H
#define WORLDWALKER_DATA_CROSSINGDERIVER_H

#include "build/CacheClient.h"
#include "build/CollisionLookup.h"
#include "data/Transitions.h"

#include <cstddef>
#include <vector>

namespace ww::data
{
    // Counts from deriveDoorTransitions, for build-log visibility.
    struct CrossingReport
    {
        std::size_t doorCrossings{};   // CrossingKind::Door records seen
        std::size_t blockedOrigin{};   // skipped: the door's loc tile is not standable
        std::size_t noEdge{};          // skipped: no blocked wall edge to a standable neighbour
        std::size_t emitted{};         // directed Transport transitions returned (2 per crossable edge)
    };

    // Derive same-plane door Transitions from the cache crossings (ADR 0003/0009).
    // A door is a wall loc on a standable tile L whose blocked wall edges separate
    // L from its neighbours; for each blocked edge to a standable neighbour N this
    // emits TWO directed Transport transitions, L->N and N->L, both interacting
    // with the door's loc id (the executor resolves the loc within a tile of the
    // origin). The two sides already land in different areas (the wall blocks the
    // flood fill), so these edges are what lets the planner cross a closed door.
    //
    // Reads wall bits + standability from `lookup` (the same collision the area
    // graph is built over). Object-footprint doors (a blocked loc tile, e.g. some
    // shape-9 doors) carry no wall bits here and are skipped — climb-overs and such
    // come from the curated transport-links dataset instead. The returned
    // transitions are raw (uncosted, unsnapped); feed them through
    // finalizeTransitions. *outReport (nullable) receives the counts.
    TransitionModel deriveDoorTransitions(const std::vector<ww::build::Crossing> &crossings,
                                          const ww::build::CollisionLookup &lookup,
                                          CrossingReport *outReport);
}

#endif  // WORLDWALKER_DATA_CROSSINGDERIVER_H
