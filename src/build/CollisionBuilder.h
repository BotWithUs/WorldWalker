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

    // Enumerate index-5 map archives and decode each into the model. *outSkipped
    // (nullable) receives the count of archives that enumerated but failed to
    // decode (a square vanishing between enumeration and read).
    CollisionModel buildCollisionModel(const CacheClient &cache, int *outSkipped);
}

#endif  // WORLDWALKER_BUILD_COLLISIONBUILDER_H
