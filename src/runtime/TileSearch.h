#ifndef WORLDWALKER_RUNTIME_TILESEARCH_H
#define WORLDWALKER_RUNTIME_TILESEARCH_H

#include "runtime/WorldView.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ww::runtime
{
    // One walkable tile on a refined path. Plane is fixed for a whole TilePath
    // (a tile-level walk never changes plane — plane changes are transitions), so
    // it is not repeated per tile.
    struct TilePoint
    {
        int32_t x;
        int32_t y;
    };

    // A refined tile-level path on a single plane: standable tiles from start
    // (front) to goal (back), each adjacent pair a single legal 8-direction step,
    // plus the total movement cost (cardinal 1.0, diagonal ~1.414). tiles is empty
    // when no path was found.
    struct TilePath
    {
        std::vector<TilePoint> tiles;
        float cost{};
    };

    // A priority-queue entry: an open node index keyed by its A* f-score.
    // Namespace-scope so the .cpp's heap comparator can name it.
    struct OpenTile
    {
        float priority;
        int32_t node;
    };

    // A* over the directional collision grid — the tile-level refinement that
    // walks within one area of the abstract route (Phase 3 plan). Movement is the
    // eight compass steps; each is gated by the directional wall edges on both the
    // source and target tile and, for diagonals, an anti-corner-cut rule (a
    // diagonal is legal only when both flanking cardinal steps are independently
    // legal), faithfully matching the prior nav stack's canMove. The cost metric
    // and octile heuristic are consistent, so A* returns a least-cost path.
    //
    // One instance per search context (ADR 0007): it borrows a WorldView (whose
    // clip/area cache it drives) and reuses its node/heap/closed-set scratch
    // across queries. Not thread-safe; the borrowed WorldView must outlive it.
    class TileSearch
    {
    public:
        explicit TileSearch(WorldView &view);

        TileSearch(const TileSearch &) = delete;
        TileSearch &operator=(const TileSearch &) = delete;
        TileSearch(TileSearch &&) = default;
        TileSearch &operator=(TileSearch &&) = default;

        // Least-cost walkable path from (startX,startY) to (goalX,goalY) on one
        // plane. When areaConstraint >= 0 the search stays within that area id (the
        // within-area refinement); pass -1 to walk over any standable tile.
        // Returns false (outPath.tiles empty) when an endpoint is unstandable or
        // violates the area constraint, or the goal is unreachable within the
        // expansion budget; a start == goal query yields a single-tile path.
        bool findPath(int32_t startX, int32_t startY, int32_t goalX, int32_t goalY,
                      int32_t plane, int32_t areaConstraint, TilePath &outPath);

    private:
        struct Node
        {
            int32_t x;
            int32_t y;
            float g;
            int32_t parent;
        };


        bool acceptsEndpoints(int32_t startX, int32_t startY, int32_t goalX, int32_t goalY,
                              int32_t plane, int32_t areaConstraint);

        // Legality + neighbor coordinates + neighbor clip word for direction
        // `dir` off (fx, fy) given the already-fetched fromFlags. Reads the
        // destination clip word exactly once (the source's wall bit is
        // tested off the hoisted fromFlags, and the standable + opposite-
        // side wall bit + stand-blocked all come from the single fetched
        // toFlags). On success writes outNx/outNy; returns false on any
        // blocker. The area-constraint check is the only remaining view
        // call beyond clipAt.
        bool tryStep(int32_t fx, int32_t fy, uint32_t fromFlags, int dir,
                     int32_t plane, int32_t areaConstraint,
                     int32_t &outNx, int32_t &outNy);

        // Try to enqueue (nx, ny) as a new open node parented at curIndex
        // with g-cost curG + stepCost. No-op when the tile is already
        // settled. Used by expand() to share the heap-push body between
        // cardinals and diagonals.
        void enqueueNeighbor(int32_t curIndex, float curG, int32_t nx, int32_t ny,
                             float stepCost, int32_t goalX, int32_t goalY, int32_t plane);

        void expand(int32_t curIndex, int32_t goalX, int32_t goalY, int32_t plane, int32_t areaConstraint);
        void reconstruct(int32_t endIndex, TilePath &outPath) const;

        WorldView *view;
        std::vector<Node> nodes;                 // node arena (scratch)
        std::vector<OpenTile> openHeap;          // binary min-heap of open node indices (scratch)
        // Phase 2: the closed set is now an epoch-stamped grid backed by
        // WorldView (per-(square, plane) uint32 stamps). The epoch is bumped
        // at the top of each findPath via view.beginTileSearch() so prior
        // marks read as stale without wiping anything. visitedEpoch holds
        // the current value for this findPath; isTileClosed / markTileClosed
        // on WorldView use it as the comparison key.
        uint32_t visitedEpoch{0};
    };
}

#endif  // WORLDWALKER_RUNTIME_TILESEARCH_H
