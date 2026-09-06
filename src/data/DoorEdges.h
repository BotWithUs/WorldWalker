#ifndef WORLDWALKER_DATA_DOOREDGES_H
#define WORLDWALKER_DATA_DOOREDGES_H

#include "format/ClipFlags.h"

#include <cstdint>

namespace ww::data
{
    // The wall-edge bits a loc of this (shape, rotation) stamps onto its OWN
    // tile. Shape 0 is a single straight wall along one side; shapes 1 and 3 are
    // the thin and thick forms of a diagonal across one corner (identical for
    // collision); shape 2 is an L-bend covering two adjacent sides. Every other
    // shape is scenery or floor decoration and contributes no wall edge.
    //
    // Rotation turns the pattern 90 degrees clockwise per step, so rotation 0's
    // WEST edge becomes NORTH at rotation 1 — the order the tables below encode.
    //
    // The reflection onto the neighbour across each edge is NOT part of this
    // function: it is the collision builder's job, and `wwcli walltest` asserts
    // the reflection and rotation invariants against this table rather than
    // against a copy of it.
    inline uint32_t doorEdgeMask(uint8_t shape, uint8_t rotation)
    {
        static constexpr uint32_t kStraight[4] = {
            format::CLIP_WALL_W, format::CLIP_WALL_N,
            format::CLIP_WALL_E, format::CLIP_WALL_S,
        };
        static constexpr uint32_t kDiagonal[4] = {
            format::CLIP_WALL_NW, format::CLIP_WALL_NE,
            format::CLIP_WALL_SE, format::CLIP_WALL_SW,
        };
        static constexpr uint32_t kCorner[4] = {
            format::CLIP_WALL_W | format::CLIP_WALL_N,
            format::CLIP_WALL_N | format::CLIP_WALL_E,
            format::CLIP_WALL_E | format::CLIP_WALL_S,
            format::CLIP_WALL_S | format::CLIP_WALL_W,
        };
        const uint32_t r = rotation & 3u;
        switch (shape)
        {
            case 0:
                return kStraight[r];
            case 1:
            case 3:
                return kDiagonal[r];
            case 2:
                return kCorner[r];
            default:
                return 0u;
        }
    }
}

#endif  // WORLDWALKER_DATA_DOOREDGES_H
