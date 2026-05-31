#include "runtime/PathAssembler.h"

#include "format/Artifact.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>

namespace ww::runtime
{
    namespace
    {
        // Tunable: maximum tiles per emitted WALK step. The executor re-issues a
        // walkTo at each chunk endpoint, which doubles as the natural cadence for
        // mid-walk stuck / drift checks (open tuning per the implementation plan).
        constexpr std::size_t kWalkChunkTiles = 16;

        // Chebyshev radius searched around a transition's origin tile for the
        // standable interact-from tile. 2 covers every authored transition: the
        // origin itself (typically a one-tile object) plus the surrounding ring.
        constexpr int32_t kInteractSearchRadius = 2;

        constexpr uint32_t kNoTransitionIndex = std::numeric_limits<uint32_t>::max();

        // Append a Walk step targeting (x, y, plane). Keeps the construction in one
        // place so the pad byte is always zero-initialized.
        void pushWalk(Plan &plan, int32_t x, int32_t y, int32_t plane)
        {
            plan.steps.push_back({StepKind::Walk, static_cast<uint8_t>(plane), 0u,
                                  x, y, kNoTransitionIndex});
        }
    }

    PathAssembler::PathAssembler(const format::ArtifactReader &reader, WorldView &view,
                                 AreaSearch &areaSearch, TileSearch &tileSearch)
        : artifact(&reader),
          view(&view),
          areaSearch(&areaSearch),
          tileSearch(&tileSearch)
    {
    }

    // Outward Chebyshev-ring scan: the origin tile (r=0) wins when it is itself
    // standable and in-area; otherwise the first ring-r tile that qualifies does,
    // so the result is always a closest valid neighbor of the object.
    bool PathAssembler::resolveInteractTile(int32_t originX, int32_t originY, int32_t plane,
                                            int32_t area, int32_t &outX, int32_t &outY) const
    {
        for (int32_t r = 0; r <= kInteractSearchRadius; ++r)
        {
            for (int32_t dy = -r; dy <= r; ++dy)
            {
                for (int32_t dx = -r; dx <= r; ++dx)
                {
                    if (std::max(std::abs(dx), std::abs(dy)) != r)
                    {
                        continue;  // inner rings handled in earlier iterations
                    }
                    const int32_t x = originX + dx;
                    const int32_t y = originY + dy;
                    if (view->isStandable(x, y, plane) && view->areaAt(x, y, plane) == area)
                    {
                        outX = x;
                        outY = y;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool PathAssembler::appendWalkSegment(int32_t fromX, int32_t fromY, int32_t toX, int32_t toY,
                                          int32_t plane, int32_t area, Plan &outPlan)
    {
        if (!tileSearch->findPath(fromX, fromY, toX, toY, plane, area, tilePath))
        {
            return false;
        }
        outPlan.cost += tilePath.cost;
        const std::size_t n = tilePath.tiles.size();
        if (n <= 1)
        {
            return true;  // start == end: refined path has one tile, no movement to emit
        }
        // Walk steps advance by at most kWalkChunkTiles tiles per hop; the final
        // hop always lands on the last tile so the segment terminates exactly.
        std::size_t cursor = 0;
        while (cursor < n - 1)
        {
            const std::size_t next = std::min(cursor + kWalkChunkTiles, n - 1);
            const TilePoint &tp = tilePath.tiles[next];
            pushWalk(outPlan, tp.x, tp.y, plane);
            cursor = next;
        }
        return true;
    }

    bool PathAssembler::appendTransitionHop(std::size_t pathIndex,
                                            std::span<const format::AreaEdgeRecord> edges,
                                            std::span<const format::TransitionRecord> transitions,
                                            int32_t &cursorX, int32_t &cursorY, int32_t &cursorPlane,
                                            Plan &outPlan)
    {
        const int32_t edgeIdx = areaPath.steps[pathIndex].viaEdge;
        if (edgeIdx < 0 || static_cast<std::size_t>(edgeIdx) >= edges.size())
        {
            return false;
        }
        const format::AreaEdgeRecord &edge = edges[static_cast<std::size_t>(edgeIdx)];
        if (edge.transitionIndex >= transitions.size())
        {
            return false;
        }
        const format::TransitionRecord &tx = transitions[edge.transitionIndex];
        int32_t interactX = 0;
        int32_t interactY = 0;
        if (!resolveInteractTile(tx.originX, tx.originY, static_cast<int32_t>(tx.originPlane),
                                 edge.fromArea, interactX, interactY))
        {
            return false;
        }
        if (!appendWalkSegment(cursorX, cursorY, interactX, interactY, cursorPlane,
                               edge.fromArea, outPlan))
        {
            return false;
        }
        outPlan.steps.push_back({StepKind::Transition,
                                 static_cast<uint8_t>(tx.originPlane), 0u,
                                 interactX, interactY, edge.transitionIndex});
        outPlan.cost += edge.cost;
        cursorX = tx.destX;
        cursorY = tx.destY;
        cursorPlane = static_cast<int32_t>(tx.destPlane);
        return true;
    }

    bool PathAssembler::assemble(int32_t startX, int32_t startY, int32_t startPlane,
                                 int32_t goalX, int32_t goalY, int32_t goalPlane,
                                 Plan &outPlan)
    {
        return assemble(startX, startY, startPlane, goalX, goalY, goalPlane, nullptr, outPlan);
    }

    bool PathAssembler::assemble(int32_t startX, int32_t startY, int32_t startPlane,
                                 int32_t goalX, int32_t goalY, int32_t goalPlane,
                                 const CapabilitySnapshot *capabilities, Plan &outPlan)
    {
        outPlan.steps.clear();
        outPlan.cost = 0.0f;
        const int32_t startArea = view->areaAt(startX, startY, startPlane);
        const int32_t goalArea = view->areaAt(goalX, goalY, goalPlane);
        if (startArea < 0 || goalArea < 0)
        {
            return false;
        }
        if (startArea == goalArea)
        {
            return appendWalkSegment(startX, startY, goalX, goalY, startPlane, startArea, outPlan);
        }
        if (!areaSearch->findPath(startArea, goalArea, capabilities, areaPath))
        {
            return false;
        }
        // Cursor tracks the player's notional tile as the route plays out: walks
        // advance it, transitions snap it to the destination, the closing walk
        // drives it to the goal.
        const std::span<const format::AreaEdgeRecord> edges = artifact->areaEdges();
        const std::span<const format::TransitionRecord> transitions = artifact->transitions();
        int32_t cursorX = startX;
        int32_t cursorY = startY;
        int32_t cursorPlane = startPlane;
        for (std::size_t i = 1; i < areaPath.steps.size(); ++i)
        {
            if (!appendTransitionHop(i, edges, transitions, cursorX, cursorY, cursorPlane, outPlan))
            {
                return false;
            }
        }
        return appendWalkSegment(cursorX, cursorY, goalX, goalY, cursorPlane, goalArea, outPlan);
    }
}
