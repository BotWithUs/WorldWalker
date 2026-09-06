#ifndef WORLDWALKER_BUILD_COLLISIONLOOKUP_H
#define WORLDWALKER_BUILD_COLLISIONLOOKUP_H

#include "build/CollisionBuilder.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

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

        // Raw directional clip word at a tile. Absent square, out-of-range plane,
        // or out-of-range index all return CLIP_BLOCKED, so a step into them
        // always fails the wall/stand checks the caller performs.
        uint32_t clipAt(int worldX, int worldY, int plane) const;

    private:
        // The clip words of one square, or an empty vector when the square is
        // absent. Every hot caller (the area-graph flood fill, the wall and
        // approach checks, endpoint snapping) sweeps a neighbourhood, so
        // consecutive lookups almost always land in the same square: a
        // one-slot sticky cache turns the hash lookup from once per tile read
        // into once per square. The runtime's WorldView::bakedClipAt does the
        // same thing for the same reason.
        const std::vector<uint32_t> &squareWords(int squareX, int squareY) const;

        const CollisionModel &model;
        std::unordered_map<uint32_t, std::size_t> squareIndex;
        // What an absent square reads as. Held as a member so squareWords can
        // return a reference for the miss case too, and so the sticky slot can
        // memoize a miss rather than re-hashing it on every tile of the gap.
        const std::vector<uint32_t> absentWords;
        // Sticky slot. Mutable: it caches a read, it does not change what any
        // read answers.
        mutable int lastSquareX{0};
        mutable int lastSquareY{0};
        mutable const std::vector<uint32_t> *lastWords{nullptr};
    };
}

#endif  // WORLDWALKER_BUILD_COLLISIONLOOKUP_H
