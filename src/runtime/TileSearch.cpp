#include "runtime/TileSearch.h"

#include "format/ClipFlags.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ww::runtime
{
    namespace
    {
        constexpr float kDiagCost = 1.41421356f;
        constexpr int kMaxExpansions = 1 << 16;  // ample for refinement within one area

        // Compass step deltas, indexed N, NE, E, SE, S, SW, W, NW. Diagonals are
        // the odd indices. (North is +Y, East is +X.)
        constexpr int32_t kDx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
        constexpr int32_t kDy[8] = {1, 1, 0, -1, -1, -1, 0, 1};
        constexpr float kStepCost[8] = {1.0f, kDiagCost, 1.0f, kDiagCost,
                                         1.0f, kDiagCost, 1.0f, kDiagCost};

        // Per-direction wall bits: [dir][0] is the edge bit checked on the source
        // tile, [dir][1] the opposite bit on the target tile. A step is walled off
        // when either endpoint carries its bit (see ClipFlags.h on reflection).
        constexpr uint32_t kWallCheck[8][2] = {
            {format::CLIP_WALL_N,  format::CLIP_WALL_S },
            {format::CLIP_WALL_NE, format::CLIP_WALL_SW},
            {format::CLIP_WALL_E,  format::CLIP_WALL_W },
            {format::CLIP_WALL_SE, format::CLIP_WALL_NW},
            {format::CLIP_WALL_S,  format::CLIP_WALL_N },
            {format::CLIP_WALL_SW, format::CLIP_WALL_NE},
            {format::CLIP_WALL_W,  format::CLIP_WALL_E },
            {format::CLIP_WALL_NW, format::CLIP_WALL_SE},
        };

        // Orders the open vector as a binary min-heap on f-score: the std heap
        // surfaces the *greatest* element under the comparator, so "greater
        // priority" means "worse", keeping the cheapest f-score on top.
        struct ByPriority
        {
            bool operator()(const OpenTile &a, const OpenTile &b) const
            {
                return a.priority > b.priority;
            }
        };

        // Octile distance: admissible and consistent for the cardinal-1 /
        // diagonal-sqrt(2) cost metric the search uses.
        float heuristic(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
        {
            const int32_t dx = x1 < x2 ? x2 - x1 : x1 - x2;
            const int32_t dy = y1 < y2 ? y2 - y1 : y1 - y2;
            const int32_t hi = std::max(dx, dy);
            const int32_t lo = std::min(dx, dy);
            return static_cast<float>(hi) + (kDiagCost - 1.0f) * static_cast<float>(lo);
        }
    }

    // ---- TileSearch ---------------------------------------------------------

    TileSearch::TileSearch(WorldView &view)
        : view(&view)
    {
    }

    bool TileSearch::acceptsEndpoints(int32_t startX, int32_t startY, int32_t goalX, int32_t goalY,
                                      int32_t plane, int32_t areaConstraint)
    {
        if (!view->isStandable(startX, startY, plane) || !view->isStandable(goalX, goalY, plane))
        {
            return false;
        }
        if (areaConstraint < 0)
        {
            return true;
        }
        return view->areaAt(startX, startY, plane) == areaConstraint
            && view->areaAt(goalX, goalY, plane) == areaConstraint;
    }

    bool TileSearch::tryStep(int32_t fx, int32_t fy, uint32_t fromFlags, int dir,
                             int32_t plane, int32_t areaConstraint,
                             int32_t &outNx, int32_t &outNy)
    {
        outNx = fx + kDx[dir];
        outNy = fy + kDy[dir];
        // Source wall bit is the cheapest check (no view call); short-circuit
        // first so a walled-off direction skips the destination clip fetch.
        if ((fromFlags & kWallCheck[dir][0]) != 0u)
        {
            return false;
        }
        const uint32_t toFlags = view->clipAt(outNx, outNy, plane);
        if ((toFlags & format::kClipStandBlockedMask) != 0u)
        {
            return false;
        }
        if ((toFlags & kWallCheck[dir][1]) != 0u)
        {
            return false;
        }
        if (areaConstraint >= 0 && view->areaAt(outNx, outNy, plane) != areaConstraint)
        {
            return false;
        }
        return true;
    }

    void TileSearch::enqueueNeighbor(int32_t curIndex, float curG, int32_t nx, int32_t ny,
                                     float stepCost, int32_t goalX, int32_t goalY, int32_t plane)
    {
        // Expand-time visited check prevents redundant heap entries for tiles
        // that are already settled. Lazy-pop handles in-flight duplicates.
        // Phase 2: epoch-stamped grid on WorldView replaces the std::unordered_set
        // — same two-check structure (expand-time skip + pop-time skip) so heap
        // push order and A* tie-breaking are unchanged.
        if (view->isTileClosed(nx, ny, plane, visitedEpoch))
        {
            return;
        }
        const float ng = curG + stepCost;
        const int32_t newIndex = static_cast<int32_t>(nodes.size());
        nodes.push_back({nx, ny, ng, curIndex});
        openHeap.push_back({ng + heuristic(nx, ny, goalX, goalY), newIndex});
        std::push_heap(openHeap.begin(), openHeap.end(), ByPriority{});
    }

    void TileSearch::expand(int32_t curIndex, int32_t goalX, int32_t goalY, int32_t plane,
                            int32_t areaConstraint)
    {
        const Node cur = nodes[static_cast<std::size_t>(curIndex)];  // by value: push_back may realloc
        const int32_t fx = cur.x;
        const int32_t fy = cur.y;
        // Source clip word is the same for every direction off this tile —
        // hoist it once instead of having each tryStep call refetch.
        const uint32_t fromFlags = view->clipAt(fx, fy, plane);

        // Pre-compute the four cardinal verdicts up front WITHOUT emitting
        // them yet. A diagonal step is legal only when both flanking
        // cardinals are independently legal; caching here means each
        // cardinal pays its tryStep cost once instead of being re-resolved
        // (twice) as a flank check during diagonal handling.
        bool cardOpen[4] = {false, false, false, false};
        int32_t cardNx[4] = {0, 0, 0, 0};
        int32_t cardNy[4] = {0, 0, 0, 0};
        for (int ci = 0; ci < 4; ++ci)
        {
            const int dir = ci * 2;
            cardOpen[ci] = tryStep(fx, fy, fromFlags, dir, plane, areaConstraint,
                                   cardNx[ci], cardNy[ci]);
        }

        // Walk dirs 0..7 in the original order so the open-heap push
        // sequence (and hence A* tie-breaking) matches the prior nav stack
        // byte-for-byte. Cardinals consume the cached verdict; diagonals
        // gate on the two flanking cardinals before running their own
        // tryStep. Diagonal `2di + 1` flanks are card[di] and card[(di+1)&3].
        for (int dir = 0; dir < 8; ++dir)
        {
            if ((dir & 1) == 0)
            {
                const int ci = dir >> 1;
                if (!cardOpen[ci])
                {
                    continue;
                }
                enqueueNeighbor(curIndex, cur.g, cardNx[ci], cardNy[ci],
                                kStepCost[dir], goalX, goalY, plane);
            }
            else
            {
                const int di = dir >> 1;  // 0,1,2,3 for dirs 1,3,5,7
                if (!cardOpen[di] || !cardOpen[(di + 1) & 3])
                {
                    continue;  // corner-cut would clip a wall
                }
                int32_t nx = 0;
                int32_t ny = 0;
                if (!tryStep(fx, fy, fromFlags, dir, plane, areaConstraint, nx, ny))
                {
                    continue;
                }
                enqueueNeighbor(curIndex, cur.g, nx, ny, kStepCost[dir], goalX, goalY, plane);
            }
        }
    }

    void TileSearch::reconstruct(int32_t endIndex, TilePath &outPath) const
    {
        outPath.cost = nodes[static_cast<std::size_t>(endIndex)].g;
        int32_t idx = endIndex;
        while (idx >= 0)
        {
            const Node &n = nodes[static_cast<std::size_t>(idx)];
            outPath.tiles.push_back({n.x, n.y});
            idx = n.parent;
        }
        std::reverse(outPath.tiles.begin(), outPath.tiles.end());
    }

    bool TileSearch::findPath(int32_t startX, int32_t startY, int32_t goalX, int32_t goalY,
                              int32_t plane, int32_t areaConstraint, TilePath &outPath)
    {
        outPath.tiles.clear();
        outPath.cost = 0.0f;
        if (!acceptsEndpoints(startX, startY, goalX, goalY, plane, areaConstraint))
        {
            return false;
        }
        if (startX == goalX && startY == goalY)
        {
            outPath.tiles.push_back({startX, startY});
            return true;
        }

        nodes.clear();
        openHeap.clear();
        // Phase 2: bump the WorldView's tile-search epoch instead of wiping a
        // hash set. The new epoch is what isTileClosed / markTileClosed test
        // against; the actual stamp memory survives across findPath calls.
        visitedEpoch = view->beginTileSearch();
        nodes.push_back({startX, startY, 0.0f, -1});
        openHeap.push_back({heuristic(startX, startY, goalX, goalY), 0});

        int budget = kMaxExpansions;
        while (!openHeap.empty() && budget > 0)
        {
            std::pop_heap(openHeap.begin(), openHeap.end(), ByPriority{});
            const int32_t curIndex = openHeap.back().node;
            openHeap.pop_back();
            const Node cur = nodes[static_cast<std::size_t>(curIndex)];
            if (cur.x == goalX && cur.y == goalY)
            {
                reconstruct(curIndex, outPath);
                return true;
            }
            // Pop-time closed check: catches in-flight heap duplicates that
            // were enqueued before the tile was settled. Mirrors the prior
            // visited.insert(...).second pattern with an epoch-stamp grid.
            if (view->isTileClosed(cur.x, cur.y, plane, visitedEpoch))
            {
                continue;
            }
            view->markTileClosed(cur.x, cur.y, plane, visitedEpoch);
            --budget;
            expand(curIndex, goalX, goalY, plane, areaConstraint);
        }
        return false;
    }
}
