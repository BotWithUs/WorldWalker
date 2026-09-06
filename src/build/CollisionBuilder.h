#ifndef WORLDWALKER_BUILD_COLLISIONBUILDER_H
#define WORLDWALKER_BUILD_COLLISIONBUILDER_H

#include "build/CacheClient.h"

#include <vector>

namespace ww::build
{
    // The decoded directional clip for every populated map square, sorted by
    // (squareY, squareX) so the serialized square table is deterministic and
    // range-friendly. Present-but-empty squares are kept: the runtime treats an
    // absent square as unknown/blocked, so an open square must still be stored.
    struct CollisionModel
    {
        std::vector<SquareClip> squares;
    };

    // Everything one pass over the map index yields. `crossings` holds the
    // interactable crossings (doors / climb-overs / ladders-stairs / agility)
    // of every decoded square in archive-enumeration order — the clip grid
    // alone cannot name the loc a transition interacts with, and the producer
    // builds both from a single landscape decode, so they are collected
    // together rather than by a second sweep over the same archives.
    struct CollisionBuildResult
    {
        CollisionModel model;
        std::vector<Crossing> crossings;
        // Archives that enumerated but did not decode into a square: a junk
        // archive id outside the addressable grid, or a square vanishing
        // between enumeration and read.
        int skippedArchives{};
    };

    // Enumerate index-5 map archives and decode each square's clip and
    // crossings in one pass.
    CollisionBuildResult buildCollisionModel(const CacheClient &cache);
}

#endif  // WORLDWALKER_BUILD_COLLISIONBUILDER_H
