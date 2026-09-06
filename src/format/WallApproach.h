#ifndef WORLDWALKER_FORMAT_WALLAPPROACH_H
#define WORLDWALKER_FORMAT_WALLAPPROACH_H

#include "format/ClipFlags.h"

#include <cstdint>

// "Can a unit walk from here to there without crossing a wall?" — the wall-edge
// rules the baker and the runtime must answer identically.
//
// The clip word contract (see ClipFlags.h) puts a wall edge on the tile it
// belongs to AND reflects it onto the neighbour, except that reflections
// falling outside a mapsquare's 64x64 grid are dropped at bake time. A one-sided
// check therefore misses every wall whose owning tile sits across a mapsquare
// seam, so every edge here is checked on BOTH endpoints.
//
// Every entry point is a template over a clip accessor: any callable
//   uint32_t clipAt(int x, int y, int plane)
// that answers with the tile's clip word and with CLIP_BLOCKED for a tile it
// cannot address (absent square, out-of-range plane). That is what
// build::CollisionLookup::clipAt and runtime::WorldView::clipAt both do, so the
// bake and the runtime can share one implementation rather than two that drift.
namespace ww::format
{
    namespace detail
    {
        constexpr int stepToward(int from, int to)
        {
            return (to > from) - (to < from);
        }
    }

    // True when a wall edge seals the cardinal crossing from (fromX, fromY)
    // toward (dx, dy). Exactly one of dx / dy must be non-zero.
    template <typename ClipAt>
    bool wallBlocksCardinal(ClipAt &&clipAt, int fromX, int fromY, int plane, int dx, int dy)
    {
        uint32_t fromMask = 0;
        uint32_t toMask = 0;
        if (dy == 1)
        {
            fromMask = CLIP_WALL_N;
            toMask = CLIP_WALL_S;
        }
        else if (dy == -1)
        {
            fromMask = CLIP_WALL_S;
            toMask = CLIP_WALL_N;
        }
        else if (dx == 1)
        {
            fromMask = CLIP_WALL_E;
            toMask = CLIP_WALL_W;
        }
        else
        {
            fromMask = CLIP_WALL_W;
            toMask = CLIP_WALL_E;
        }
        return (clipAt(fromX, fromY, plane) & fromMask) != 0u
            || (clipAt(fromX + dx, fromY + dy, plane) & toMask) != 0u;
    }

    // Corner-blocker bit for a diagonal crossing, checked on both endpoints
    // like the cardinal edges. Both dx and dy must be non-zero.
    template <typename ClipAt>
    bool cornerBlocksDiagonal(ClipAt &&clipAt, int fromX, int fromY, int plane, int dx, int dy)
    {
        uint32_t fromMask = 0;
        uint32_t toMask = 0;
        if (dx == 1 && dy == 1)
        {
            fromMask = CLIP_WALL_NE;
            toMask = CLIP_WALL_SW;
        }
        else if (dx == 1 && dy == -1)
        {
            fromMask = CLIP_WALL_SE;
            toMask = CLIP_WALL_NW;
        }
        else if (dx == -1 && dy == -1)
        {
            fromMask = CLIP_WALL_SW;
            toMask = CLIP_WALL_NE;
        }
        else
        {
            fromMask = CLIP_WALL_NW;
            toMask = CLIP_WALL_SE;
        }
        return (clipAt(fromX, fromY, plane) & fromMask) != 0u
            || (clipAt(fromX + dx, fromY + dy, plane) & toMask) != 0u;
    }

    // True when walls seal the single step from (fromX, fromY) in direction
    // (dx, dy), each in {-1, 0, 1} and not both zero.
    //
    // A cardinal step is one two-sided edge check. A diagonal step is blocked
    // by the corner bit on either endpoint, and ALSO when both flanking
    // cardinal L-paths cross a wall — the no-corner-cutting rule. Without that
    // second test a tile diagonally across a wall corner slips through on the
    // single diagonal bit, which is how the sealed side of a door used to be
    // admitted as an approach.
    template <typename ClipAt>
    bool wallBlocksApproach(ClipAt &&clipAt, int fromX, int fromY, int plane, int dx, int dy)
    {
        if (dx == 0 || dy == 0)
        {
            return wallBlocksCardinal(clipAt, fromX, fromY, plane, dx, dy);
        }
        if (cornerBlocksDiagonal(clipAt, fromX, fromY, plane, dx, dy))
        {
            return true;
        }
        const bool viaXClear =
            !wallBlocksCardinal(clipAt, fromX, fromY, plane, dx, 0)
            && !wallBlocksCardinal(clipAt, fromX + dx, fromY, plane, 0, dy);
        const bool viaYClear =
            !wallBlocksCardinal(clipAt, fromX, fromY, plane, 0, dy)
            && !wallBlocksCardinal(clipAt, fromX, fromY + dy, plane, dx, 0);
        return !viaXClear && !viaYClear;
    }

    // True when the straight-line walk from (fromX, fromY) to (originX, originY)
    // is sealed: some step along it crosses a wall, or some tile it passes
    // through is not standable.
    //
    // This is wallBlocksApproach generalised past a single step, for callers
    // that look for an interact-from tile further than one tile from the object.
    // The wall check is applied per step, not once over the whole displacement,
    // so a two-tile approach cannot hop a wall that sits mid-way.
    //
    // The two endpoints are treated asymmetrically, and deliberately:
    //   - (fromX, fromY) is not checked for standability. It is the candidate
    //     the caller is testing and the caller has already qualified it.
    //   - (originX, originY) is not checked either. It is normally the loc's own
    //     blocked footprint, which is the entire reason a caller is hunting for
    //     a tile to stand on beside it.
    // Every tile strictly between them must be standable.
    //
    // Returns false when from == origin (nothing to seal).
    template <typename ClipAt>
    bool approachSealed(ClipAt &&clipAt, int fromX, int fromY, int originX, int originY, int plane)
    {
        int curX = fromX;
        int curY = fromY;
        while (curX != originX || curY != originY)
        {
            const int sx = detail::stepToward(curX, originX);
            const int sy = detail::stepToward(curY, originY);
            if (wallBlocksApproach(clipAt, curX, curY, plane, sx, sy))
            {
                return true;
            }
            curX += sx;
            curY += sy;
            if (curX == originX && curY == originY)
            {
                return false;
            }
            if ((clipAt(curX, curY, plane) & kClipStandBlockedMask) != 0u)
            {
                return true;
            }
        }
        return false;
    }
}

#endif  // WORLDWALKER_FORMAT_WALLAPPROACH_H
