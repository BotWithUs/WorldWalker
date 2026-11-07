#include "runtime/TileSearch.h"

#include "format/ClipFlags.h"
#include "runtime/TeleportPolicy.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace ww::runtime
{
    namespace
    {
        constexpr float kDiagCost = 1.41421356f;
        // Expansion budget. PathAssembler runs whole same-plane baseline walks
        // through here (the overworld is a handful of mega-areas), not just
        // within-area refinement — a detour-heavy several-hundred-tile route
        // can close a six-figure expansion count before reaching the goal, and
        // 1 << 16 falsely reported such routes unreachable. Termination still
        // holds; only pathological searches pay the full budget.
        constexpr int kMaxExpansions = 1 << 18;

        // A walk fenced out of the Wilderness keeps this many tiles
        // (Chebyshev) off a box tile it could otherwise walk onto, where a
        // detour costs less than kFenceMarginCost a tile. The plan is clicked
        // a step at a time, and the game walks each click its own way: at
        // Fort Forinthry's south-west corner the first box's own corner
        // (3279..3281, 3521..3523) is open ground in the mainland's area, so
        // a step planned to hug it is a click whose game walk can cut across
        // it, and a player led along it stands where every click on towards
        // the fort can. The cost is a premium, not a wall: a goal against
        // the corner is still reached. It only adds to a step's cost, so the
        // octile heuristic stays admissible and consistent. Margin 1 also
        // passes the leg-corner test, with twice the hops started from a box
        // tile and about twice the crawl clicks near the fence.
        constexpr int32_t kFenceMargin = 2;
        constexpr float kFenceMarginCost = 3.0f;

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
        // A free-to-play walk that starts on its land can never step off it,
        // so a goal off it is refused here rather than after the whole
        // expansion budget is spent proving it.
        if (isOnFreeToPlayLand(startX, startY, plane) && !isOnFreeToPlayLand(goalX, goalY, plane))
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

    bool TileSearch::isOnFreeToPlayLand(int32_t x, int32_t y, int32_t plane) const
    {
        return currentFence.freeToPlay == nullptr || currentFence.freeToPlay->isFreeToPlay(x, y, plane);
    }

    float TileSearch::fenceMarginCost(int32_t x, int32_t y, int32_t plane) const
    {
        int32_t area = -1;
        for (const format::WildernessRegion &w : currentFence.wilderness)
        {
            const int32_t loX = std::max(x - kFenceMargin, w.minX);
            const int32_t hiX = std::min(x + kFenceMargin, w.maxX);
            const int32_t loY = std::max(y - kFenceMargin, w.minY);
            const int32_t hiY = std::min(y + kFenceMargin, w.maxY);
            const bool isOnPlane =
                plane >= static_cast<int32_t>(w.planeMin) && plane <= static_cast<int32_t>(w.planeMax);
            if (!isOnPlane || loX > hiX || loY > hiY)
            {
                continue;
            }
            area = area < 0 ? view->areaAt(x, y, plane) : area;
            for (int32_t by = loY; by <= hiY; ++by)
            {
                for (int32_t bx = loX; bx <= hiX; ++bx)
                {
                    if (area >= 0 && view->areaAt(bx, by, plane) == area)
                    {
                        return kFenceMarginCost;
                    }
                }
            }
        }
        return 0.0f;
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
        if (!currentFence.wilderness.empty()
            && isInWilderness(currentFence.wilderness, outNx, outNy, plane))
        {
            return false;
        }
        return !isExpandingFromFreeToPlay || isOnFreeToPlayLand(outNx, outNy, plane);
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
        const float ng = curG + stepCost + fenceMarginCost(nx, ny, plane);
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
        isExpandingFromFreeToPlay =
            currentFence.freeToPlay != nullptr && isOnFreeToPlayLand(fx, fy, plane);

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

    namespace
    {
        // Index of (x, y) in the window stepCounts fills, or -1 outside it.
        std::ptrdiff_t windowCell(int32_t x, int32_t y, int32_t centreX, int32_t centreY,
                                  int32_t reach)
        {
            const int32_t col = x - centreX + reach;
            const int32_t row = y - centreY + reach;
            const int32_t side = 2 * reach + 1;
            if (col < 0 || row < 0 || col >= side || row >= side)
            {
                return -1;
            }
            return static_cast<std::ptrdiff_t>(row) * side + col;
        }
    }

    bool TileSearch::canStep(int32_t fx, int32_t fy, int dir, int32_t plane, int32_t &outNx,
                             int32_t &outNy)
    {
        const uint32_t fromFlags = view->clipAt(fx, fy, plane);
        if ((dir & 1) != 0)
        {
            int32_t flankX = 0;
            int32_t flankY = 0;
            if (!tryStep(fx, fy, fromFlags, dir - 1, plane, kAnyArea, flankX, flankY)
                || !tryStep(fx, fy, fromFlags, (dir + 1) & 7, plane, kAnyArea, flankX, flankY))
            {
                return false;
            }
        }
        return tryStep(fx, fy, fromFlags, dir, plane, kAnyArea, outNx, outNy);
    }

    void TileSearch::stepCounts(int32_t ox, int32_t oy, int32_t centreX, int32_t centreY,
                                int32_t plane, int32_t reach, int32_t limit, bool isInbound,
                                std::vector<int32_t> &outCounts)
    {
        const std::size_t side = static_cast<std::size_t>(2 * reach + 1);
        outCounts.assign(side * side, -1);
        std::vector<TilePoint> queue;
        const std::ptrdiff_t origin = windowCell(ox, oy, centreX, centreY, reach);
        if (origin < 0)
        {
            return;
        }
        outCounts[static_cast<std::size_t>(origin)] = 0;
        queue.push_back({ox, oy});
        for (std::size_t head = 0; head < queue.size(); ++head)
        {
            const TilePoint at = queue[head];
            const int32_t count =
                outCounts[static_cast<std::size_t>(windowCell(at.x, at.y, centreX, centreY, reach))];
            if (count >= limit)
            {
                continue;
            }
            for (int dir = 0; dir < 8; ++dir)
            {
                // Inbound, the tile one step back along `dir` is a neighbour
                // when a step `dir` from it lands here.
                const TilePoint next = isInbound ? TilePoint{at.x - kDx[dir], at.y - kDy[dir]}
                                                 : TilePoint{at.x + kDx[dir], at.y + kDy[dir]};
                const std::ptrdiff_t cell = windowCell(next.x, next.y, centreX, centreY, reach);
                if (cell < 0 || outCounts[static_cast<std::size_t>(cell)] >= 0)
                {
                    continue;
                }
                int32_t nx = 0;
                int32_t ny = 0;
                const bool isLegal = isInbound ? canStep(next.x, next.y, dir, plane, nx, ny)
                                               : canStep(at.x, at.y, dir, plane, nx, ny);
                if (isLegal)
                {
                    outCounts[static_cast<std::size_t>(cell)] = count + 1;
                    queue.push_back(next);
                }
            }
        }
    }

    bool TileSearch::canShortestWalkEnter(int32_t startX, int32_t startY, TilePoint goal,
                                          int32_t plane,
                                          std::span<const format::WildernessRegion> boxes,
                                          int32_t reach)
    {
        const std::ptrdiff_t goalCell = windowCell(goal.x, goal.y, startX, startY, reach);
        if (goalCell < 0 || !view->isStandable(startX, startY, plane)
            || !view->isStandable(goal.x, goal.y, plane))
        {
            return true;
        }
        currentFence = WalkFence{};
        isExpandingFromFreeToPlay = false;
        std::vector<int32_t> fromStart;
        stepCounts(startX, startY, startX, startY, plane, reach, reach, false, fromStart);
        const int32_t length = fromStart[static_cast<std::size_t>(goalCell)];
        if (length < 0)
        {
            return true;
        }
        std::vector<int32_t> intoGoal;
        stepCounts(goal.x, goal.y, startX, startY, plane, reach, length, true, intoGoal);
        for (const format::WildernessRegion &w : boxes)
        {
            if (plane < w.planeMin || plane > w.planeMax)
            {
                continue;
            }
            const int32_t yEnd = std::min(w.maxY, startY + reach);
            const int32_t xEnd = std::min(w.maxX, startX + reach);
            for (int32_t y = std::max(w.minY, startY - reach); y <= yEnd; ++y)
            {
                for (int32_t x = std::max(w.minX, startX - reach); x <= xEnd; ++x)
                {
                    // A tile lies on some least-step walk exactly when its
                    // steps from the start and its steps to the goal add up
                    // to the walk's length.
                    const std::size_t cell =
                        static_cast<std::size_t>(windowCell(x, y, startX, startY, reach));
                    if (fromStart[cell] >= 0 && intoGoal[cell] >= 0
                        && fromStart[cell] + intoGoal[cell] == length)
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    std::vector<uint8_t> TileSearch::boxReachedCells(int32_t centreX, int32_t centreY, int32_t plane,
                                                     std::span<const format::WildernessRegion> boxes,
                                                     int32_t reach, const std::vector<int32_t> &fromStart)
    {
        const int32_t side = 2 * reach + 1;
        std::vector<std::size_t> order;
        for (std::size_t c = 0; c < fromStart.size(); ++c)
        {
            if (fromStart[c] >= 0)
            {
                order.push_back(c);
            }
        }
        std::sort(order.begin(), order.end(),
                  [&](std::size_t a, std::size_t b) { return fromStart[a] < fromStart[b]; });
        std::vector<uint8_t> isReached(fromStart.size(), 0);
        for (const std::size_t c : order)
        {
            const int32_t x = centreX - reach + static_cast<int32_t>(c) % side;
            const int32_t y = centreY - reach + static_cast<int32_t>(c) / side;
            bool isBoxed = isInWilderness(boxes, x, y, plane);
            for (int dir = 0; dir < 8 && !isBoxed; ++dir)
            {
                const std::ptrdiff_t p = windowCell(x - kDx[dir], y - kDy[dir], centreX, centreY, reach);
                int32_t nx = 0;
                int32_t ny = 0;
                isBoxed = p >= 0 && isReached[static_cast<std::size_t>(p)] != 0
                    && fromStart[static_cast<std::size_t>(p)] == fromStart[c] - 1
                    && canStep(x - kDx[dir], y - kDy[dir], dir, plane, nx, ny);
            }
            isReached[c] = isBoxed ? 1 : 0;
        }
        return isReached;
    }

    std::optional<TilePoint> TileSearch::clearestStepTowards(int32_t startX, int32_t startY, TilePoint goal,
                                                             int32_t plane,
                                                             std::span<const format::WildernessRegion> boxes,
                                                             int32_t reach, int32_t minTiles, int32_t maxTiles)
    {
        const std::ptrdiff_t goalCell = windowCell(goal.x, goal.y, startX, startY, reach);
        if (goalCell < 0 || !view->isStandable(startX, startY, plane)
            || !view->isStandable(goal.x, goal.y, plane))
        {
            return std::nullopt;
        }
        currentFence = WalkFence{};
        isExpandingFromFreeToPlay = false;
        std::vector<int32_t> fromStart;
        stepCounts(startX, startY, startX, startY, plane, reach, reach, false, fromStart);
        const int32_t length = fromStart[static_cast<std::size_t>(goalCell)];
        if (length < 0)
        {
            return std::nullopt;
        }
        std::vector<int32_t> intoGoal;
        stepCounts(goal.x, goal.y, startX, startY, plane, reach, length, true, intoGoal);
        const std::vector<uint8_t> isReached = boxReachedCells(startX, startY, plane, boxes, reach, fromStart);
        std::optional<TilePoint> best;
        int32_t bestLeft = length;
        const int32_t side = 2 * reach + 1;
        for (std::size_t c = 0; c < fromStart.size(); ++c)
        {
            const TilePoint at{ startX - reach + static_cast<int32_t>(c) % side,
                                startY - reach + static_cast<int32_t>(c) / side };
            const int64_t dx = at.x - startX;
            const int64_t dy = at.y - startY;
            const bool isInRange = std::max(std::abs(dx), std::abs(dy)) >= minTiles
                && dx * dx + dy * dy <= static_cast<int64_t>(maxTiles) * maxTiles;
            const bool isOnWalk = fromStart[c] >= 0 && intoGoal[c] >= 0 && fromStart[c] + intoGoal[c] == length;
            if (isOnWalk && isInRange && isReached[c] == 0 && intoGoal[c] > 0 && intoGoal[c] < bestLeft)
            {
                best = at;
                bestLeft = intoGoal[c];
            }
        }
        return best;
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
        return findPath(startX, startY, goalX, goalY, plane, areaConstraint, WalkFence{}, outPath);
    }

    bool TileSearch::findPath(int32_t startX, int32_t startY, int32_t goalX, int32_t goalY,
                              int32_t plane, int32_t areaConstraint, const WalkFence &fence,
                              TilePath &outPath)
    {
        currentFence = fence;
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
