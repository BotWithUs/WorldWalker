#include "runtime/PathAssembler.h"

#include "format/Artifact.h"
#include "runtime/TeleportPolicy.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <utility>

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

        // Plane is stored as uint8_t on the wire but flows through the assembler
        // as int32_t. Reject anything outside the baked range so a malformed
        // transition or a logic error upstream never silently truncates into a
        // valid-looking plane byte (cpp-rules: don't paper over invariants with
        // unchecked casts — Step plane must be 0..3 by construction).
        bool isLegalPlane(int32_t plane)
        {
            return plane >= 0 && plane < format::kClipPlanes;
        }

        // Append a Walk step targeting (x, y, plane). Keeps the construction in one
        // place so the pad byte is always zero-initialized; returns false (without
        // mutating the plan) when the plane is out of range.
        bool pushWalk(Plan &plan, int32_t x, int32_t y, int32_t plane)
        {
            if (!isLegalPlane(plane))
            {
                return false;
            }
            plan.steps.push_back({StepKind::Walk, static_cast<uint8_t>(plane), 0u,
                                  x, y, kNoTransitionIndex});
            return true;
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
        if (!isLegalPlane(plane))
        {
            return false;
        }
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
            if (!pushWalk(outPlan, tp.x, tp.y, plane))
            {
                return false;
            }
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
        const int32_t originPlane = static_cast<int32_t>(tx.originPlane);
        const int32_t destPlane   = static_cast<int32_t>(tx.destPlane);
        if (!isLegalPlane(originPlane) || !isLegalPlane(destPlane))
        {
            return false;
        }
        // Walks cannot cross planes (Step.plane invariant). The closing walk
        // toward the interact tile must therefore run on the *transition's*
        // origin plane — which had better match the cursor's plane, because
        // each area is single-plane and the area route landed us in
        // edge.fromArea. A mismatch means either (a) the artifact has an
        // AreaEdge whose origin plane disagrees with its fromArea's plane —
        // a build bug — or (b) the cursor drifted across a plane without a
        // Transition step. Both are bugs in something upstream of us; failing
        // loud beats walking on the wrong plane.
        if (cursorPlane != originPlane)
        {
            return false;
        }
        int32_t interactX = 0;
        int32_t interactY = 0;
        if (!resolveInteractTile(tx.originX, tx.originY, originPlane,
                                 edge.fromArea, interactX, interactY))
        {
            return false;
        }
        if (!appendWalkSegment(cursorX, cursorY, interactX, interactY, originPlane,
                               edge.fromArea, outPlan))
        {
            return false;
        }
        outPlan.steps.push_back({StepKind::Transition,
                                 static_cast<uint8_t>(originPlane), 0u,
                                 interactX, interactY, edge.transitionIndex});
        outPlan.cost += edge.cost;
        cursorX = tx.destX;
        cursorY = tx.destY;
        cursorPlane = destPlane;
        return true;
    }

    bool PathAssembler::assemble(int32_t startX, int32_t startY, int32_t startPlane,
                                 int32_t goalX, int32_t goalY, int32_t goalPlane,
                                 Plan &outPlan)
    {
        return assemble(startX, startY, startPlane, goalX, goalY, goalPlane, nullptr, outPlan);
    }

    // Enumerate every Global-origin transition the borrowed snapshot accepts and
    // whose destination tile lies in a valid baked area, emitting one seed per
    // accepted candidate. Cleared at entry so the caller can always read the
    // resulting span without preconditioning. Caller decides upstream whether
    // the start tile is teleport-allowed; this routine only sees transitions.
    void PathAssembler::buildGlobalTeleportSeeds(const CapabilitySnapshot *capabilities)
    {
        seedScratch.clear();
        const std::span<const format::TransitionRecord> transitions = artifact->transitions();
        const std::span<const format::RequirementRecord> reqs = artifact->requirements();
        for (uint32_t i = 0; i < transitions.size(); ++i)
        {
            const format::TransitionRecord &tx = transitions[i];
            if ((tx.flags & format::kTransitionFlagGlobalOrigin) == 0u)
            {
                continue;
            }
            const uint64_t end = static_cast<uint64_t>(tx.requirementStart) + tx.requirementCount;
            if (end > reqs.size())
            {
                continue;
            }
            if (!meetsRequirements(capabilities,
                                   reqs.subspan(tx.requirementStart, tx.requirementCount)))
            {
                continue;
            }
            const int32_t destArea =
                view->areaAt(tx.destX, tx.destY, static_cast<int32_t>(tx.destPlane));
            if (destArea < 0)
            {
                continue;
            }
            seedScratch.push_back({destArea, tx.cost, i});
        }
    }

    void PathAssembler::emitLeadingTransition(std::span<const format::TransitionRecord> transitions,
                                              int32_t &cursorX, int32_t &cursorY,
                                              int32_t &cursorPlane, Plan &outPlan)
    {
        if (areaPath.leadingTransition < 0
            || static_cast<std::size_t>(areaPath.leadingTransition) >= transitions.size())
        {
            return;
        }
        const format::TransitionRecord &tx =
            transitions[static_cast<std::size_t>(areaPath.leadingTransition)];
        if (!isLegalPlane(cursorPlane) || !isLegalPlane(static_cast<int32_t>(tx.destPlane)))
        {
            return;
        }
        outPlan.steps.push_back({StepKind::Transition,
                                 static_cast<uint8_t>(cursorPlane), 0u,
                                 cursorX, cursorY,
                                 static_cast<uint32_t>(areaPath.leadingTransition)});
        outPlan.cost += tx.cost;
        cursorX = tx.destX;
        cursorY = tx.destY;
        cursorPlane = static_cast<int32_t>(tx.destPlane);
    }

    // Builds the plan into a local scratch and moves it into outPlan only on
    // success. Any failure leaves outPlan empty + cost zero, so a caller that
    // (incorrectly) inspects outPlan.steps.size() instead of the boolean
    // return sees a clean empty plan rather than partial garbage from the
    // hops that succeeded before the failing one.
    bool PathAssembler::assemble(int32_t startX, int32_t startY, int32_t startPlane,
                                 int32_t goalX, int32_t goalY, int32_t goalPlane,
                                 const CapabilitySnapshot *capabilities, Plan &outPlan)
    {
        outPlan.steps.clear();
        outPlan.cost = 0.0f;
        if (!isLegalPlane(startPlane) || !isLegalPlane(goalPlane))
        {
            return false;
        }
        const int32_t startArea = view->areaAt(startX, startY, startPlane);
        const int32_t goalArea = view->areaAt(goalX, goalY, goalPlane);
        if (startArea < 0 || goalArea < 0)
        {
            return false;
        }

        Plan local;
        if (startArea == goalArea)
        {
            if (!appendWalkSegment(startX, startY, goalX, goalY, startPlane, startArea, local))
            {
                return false;
            }
            outPlan = std::move(local);
            return true;
        }
        seedScratch.clear();
        if (isTeleportAllowed(*artifact, startX, startY, startPlane))
        {
            buildGlobalTeleportSeeds(capabilities);
        }
        if (!areaSearch->findPath(startArea, goalArea, capabilities,
                                  std::span<const FrontierSeed>(seedScratch), areaPath))
        {
            return false;
        }
        // Cursor tracks the player's notional tile as the route plays out: walks
        // advance it, transitions snap it to the destination, the closing walk
        // drives it to the goal. A leading global teleport (recorded by
        // AreaSearch in areaPath.leadingTransition) snaps the cursor from start
        // to the teleport's destination before any walking — the player casts
        // in place.
        const std::span<const format::AreaEdgeRecord> edges = artifact->areaEdges();
        const std::span<const format::TransitionRecord> transitions = artifact->transitions();
        int32_t cursorX = startX;
        int32_t cursorY = startY;
        int32_t cursorPlane = startPlane;
        emitLeadingTransition(transitions, cursorX, cursorY, cursorPlane, local);
        for (std::size_t i = 1; i < areaPath.steps.size(); ++i)
        {
            if (!appendTransitionHop(i, edges, transitions, cursorX, cursorY, cursorPlane, local))
            {
                return false;
            }
        }
        if (!appendWalkSegment(cursorX, cursorY, goalX, goalY, cursorPlane, goalArea, local))
        {
            return false;
        }
        outPlan = std::move(local);
        return true;
    }
}
