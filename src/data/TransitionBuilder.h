#ifndef WORLDWALKER_DATA_TRANSITIONBUILDER_H
#define WORLDWALKER_DATA_TRANSITIONBUILDER_H

#include "build/CollisionLookup.h"
#include "data/Transitions.h"

#include <cstddef>

namespace ww::data
{
    // Per-stage counts produced by finalizeTransitions, for build-log visibility.
    struct TransitionReport
    {
        std::size_t input{};            // raw transitions in
        std::size_t kept{};             // finalized transitions out
        std::size_t droppedDangling{};  // an endpoint had no walkable tile within the snap radius
        std::size_t droppedSelfLoop{};  // origin == dest after snapping (degenerate edge)
        std::size_t droppedDuplicate{}; // collapsed by content-fingerprint dedup
        std::size_t snappedDest{};      // dest moved to a nearby walkable tile
    };

    // Finalize raw transitions: assign tick cost (chain waits + per-kind default),
    // snap each dest to the nearest walkable tile within a small radius, and dedup
    // by full content fingerprint. Local-origin kinds (Transport/FairyRing/chains)
    // interact with an object, so their origin tile is kept as-is (the executor
    // clicks it) but the edge is dropped as dangling when no walkable approach tile
    // lies within the radius, and dropped as a self-loop when its raw origin equals
    // its dest. The collision lookup decides walkability and must cover the same
    // cache the artifact bakes. *outReport (nullable) receives the counts.
    TransitionModel finalizeTransitions(const TransitionModel &raw,
                                        const ww::build::CollisionLookup &collision,
                                        TransitionReport *outReport);
}

#endif  // WORLDWALKER_DATA_TRANSITIONBUILDER_H
