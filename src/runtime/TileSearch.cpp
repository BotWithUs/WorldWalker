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

        uint64_t tileKey(int32_t x, int32_t y)
        {
            return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32)
                 | static_cast<uint64_t>(static_cast<uint32_t>(y));
        }

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

    // True when a single step in direction `dir` off (fromX,fromY) lands on a
    // standable, in-constraint tile with no wall blocking that edge. This is the
    // whole check for a cardinal step and one edge of a diagonal's corner test.
    bool TileSearch::isStepOpen(int32_t fromX, int32_t fromY, int dir, int32_t plane,
                                int32_t areaConstraint)
    {
        const int32_t toX = fromX + kDx[dir];
        const int32_t toY = fromY + kDy[dir];
        if (!view->isStandable(toX, toY, plane))
        {
            return false;
        }
        if (areaConstraint >= 0 && view->areaAt(toX, toY, plane) != areaConstraint)
        {
            return false;
        }
        const uint32_t fromFlags = view->clipAt(fromX, fromY, plane);
        const uint32_t toFlags = view->clipAt(toX, toY, plane);
        return (fromFlags & kWallCheck[dir][0]) == 0u && (toFlags & kWallCheck[dir][1]) == 0u;
    }

    // A diagonal step is legal only when its own edge is open AND both flanking
    // cardinal steps are open — you cannot squeeze past a blocked corner.
    bool TileSearch::canMove(int32_t fromX, int32_t fromY, int dir, int32_t plane,
                             int32_t areaConstraint)
    {
        if (!isStepOpen(fromX, fromY, dir, plane, areaConstraint))
        {
            return false;
        }
        if ((dir & 1) == 0)
        {
            return true;
        }
        const int flankA = dir - 1;
        const int flankB = (dir + 1) & 7;
        return isStepOpen(fromX, fromY, flankA, plane, areaConstraint)
            && isStepOpen(fromX, fromY, flankB, plane, areaConstraint);
    }

    void TileSearch::expand(int32_t curIndex, int32_t goalX, int32_t goalY, int32_t plane,
                            int32_t areaConstraint)
    {
        const Node cur = nodes[static_cast<std::size_t>(curIndex)];  // by value: push_back may realloc
        for (int dir = 0; dir < 8; ++dir)
        {
            if (!canMove(cur.x, cur.y, dir, plane, areaConstraint))
            {
                continue;
            }
            const int32_t nx = cur.x + kDx[dir];
            const int32_t ny = cur.y + kDy[dir];
            if (visited.count(tileKey(nx, ny)) != 0)
            {
                continue;
            }
            const float ng = cur.g + kStepCost[dir];
            const int32_t newIndex = static_cast<int32_t>(nodes.size());
            nodes.push_back({nx, ny, ng, curIndex});
            openHeap.push_back({ng + heuristic(nx, ny, goalX, goalY), newIndex});
            std::push_heap(openHeap.begin(), openHeap.end(), ByPriority{});
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
        visited.clear();
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
            if (!visited.insert(tileKey(cur.x, cur.y)).second)
            {
                continue;
            }
            --budget;
            expand(curIndex, goalX, goalY, plane, areaConstraint);
        }
        return false;
    }
}
