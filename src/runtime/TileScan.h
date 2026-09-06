#ifndef WORLDWALKER_RUNTIME_TILESCAN_H
#define WORLDWALKER_RUNTIME_TILESCAN_H

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace ww::runtime
{
    // The single answer to "which tile near here is the one to stand on": the
    // interact tile beside a transition's loc, the stand-in for a blocked goal,
    // the snapped destination of a teleport — at bake time and at runtime.
    //
    // This used to be written out five times with two different tie-break
    // policies (first hit in scan order vs. closest), so a lodestone loaded at
    // runtime could snap to a different tile than the same lodestone baked.
    // One copy, one policy: rings are searched outward, and inside a ring the
    // candidate with the smallest squared Euclidean distance wins, so a
    // cardinal neighbour beats a corner. Ties break on (dy, dx) so the answer
    // is deterministic and independent of loop order.

    // Accepted tile on the Chebyshev ring of radius `r` around (x, y) that is
    // closest to the ranking point (refX, refY). Writes the winning offset and
    // returns true; false when the ring has none.
    template <typename AcceptFn>
    bool nearestOnRing(int32_t x, int32_t y, int32_t r, AcceptFn &accept,
                       int32_t refX, int32_t refY, int32_t &outDx, int32_t &outDy)
    {
        int64_t bestSq = std::numeric_limits<int64_t>::max();
        bool found = false;
        for (int32_t dy = -r; dy <= r; ++dy)
        {
            for (int32_t dx = -r; dx <= r; ++dx)
            {
                if (std::max(std::abs(dx), std::abs(dy)) != r || !accept(x + dx, y + dy))
                {
                    continue;
                }
                const int64_t rx = x + dx - refX;
                const int64_t ry = y + dy - refY;
                const int64_t sq = rx * rx + ry * ry;
                const bool closer = sq < bestSq
                    || (sq == bestSq && (dy < outDy || (dy == outDy && dx < outDx)));
                if (!found || closer)
                {
                    bestSq = sq;
                    outDx = dx;
                    outDy = dy;
                    found = true;
                }
            }
        }
        return found;
    }

    // Nearest tile to (x, y) within `radius` (Chebyshev) that `accept(x, y)`
    // approves, ranking ties within a ring by distance to (refX, refY). With
    // includeCentre the centre tile wins outright when accepted; without it,
    // the scan starts on ring 1 (a goal known to be blocked). Returns false,
    // leaving the outputs untouched, when nothing in range is accepted.
    //
    // The ranking point is what makes this serve both questions: "nearest
    // standable tile to this spot" ranks from the centre; "which tile beside
    // this object should the player stand on" ranks from the player, so the
    // innermost ring still wins but the walk to reach it is the shortest.
    template <typename AcceptFn>
    bool findNearestTile(int32_t x, int32_t y, int32_t radius, bool includeCentre,
                         AcceptFn accept, int32_t refX, int32_t refY,
                         int32_t &outX, int32_t &outY)
    {
        if (includeCentre && accept(x, y))
        {
            outX = x;
            outY = y;
            return true;
        }
        for (int32_t r = 1; r <= radius; ++r)
        {
            int32_t dx = 0;
            int32_t dy = 0;
            if (nearestOnRing(x, y, r, accept, refX, refY, dx, dy))
            {
                outX = x + dx;
                outY = y + dy;
                return true;
            }
        }
        return false;
    }

    // findNearestTile ranked from the centre itself: the plain "closest
    // standable tile to (x, y)" question.
    template <typename AcceptFn>
    bool findNearestTile(int32_t x, int32_t y, int32_t radius, bool includeCentre,
                         AcceptFn accept, int32_t &outX, int32_t &outY)
    {
        return findNearestTile(x, y, radius, includeCentre, accept, x, y, outX, outY);
    }
}

#endif  // WORLDWALKER_RUNTIME_TILESCAN_H
