#ifndef WORLDWALKER_BUILD_COLLISIONLOOKUP_H
#define WORLDWALKER_BUILD_COLLISIONLOOKUP_H

#include "build/CollisionBuilder.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace ww::build
{
    // O(1) world-tile walkability over a built CollisionModel. Indexes the model's
    // squares by (squareX, squareY) once at construction; a tile in an absent
    // square is treated as blocked. Holds a reference to the model — keep the model
    // alive for the lookup's lifetime.
    class CollisionLookup
    {
    public:
        explicit CollisionLookup(const CollisionModel &model);

        CollisionLookup(const CollisionLookup &) = delete;
        CollisionLookup &operator=(const CollisionLookup &) = delete;

        // True when (worldX, worldY, plane) is a standable tile: its square is
        // present and the tile carries no whole-tile blocker.
        bool isWalkable(int worldX, int worldY, int plane) const;

    private:
        const CollisionModel &model;
        std::unordered_map<uint32_t, std::size_t> squareIndex;
    };
}

#endif  // WORLDWALKER_BUILD_COLLISIONLOOKUP_H
