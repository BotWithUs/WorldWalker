#ifndef WORLDWALKER_RUNTIME_TILESEARCH_H
#define WORLDWALKER_RUNTIME_TILESEARCH_H

#include "runtime/WorldView.h"

#include <cstdint>
#include <unordered_set>
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
        bool isStepOpen(int32_t fromX, int32_t fromY, int dir, int32_t plane, int32_t areaConstraint);
        bool canMove(int32_t fromX, int32_t fromY, int dir, int32_t plane, int32_t areaConstraint);
        void expand(int32_t curIndex, int32_t goalX, int32_t goalY, int32_t plane, int32_t areaConstraint);
        void reconstruct(int32_t endIndex, TilePath &outPath) const;

        WorldView *view;
        std::vector<Node> nodes;                 // node arena (scratch)
        std::vector<OpenTile> openHeap;          // binary min-heap of open node indices (scratch)
        std::unordered_set<uint64_t> visited;    // settled tiles, packed (x,y) (closed set, scratch)
    };
}

#endif  // WORLDWALKER_RUNTIME_TILESEARCH_H
