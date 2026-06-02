#ifndef WORLDWALKER_RUNTIME_PATHASSEMBLER_H
#define WORLDWALKER_RUNTIME_PATHASSEMBLER_H

#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "runtime/AreaSearch.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/TileSearch.h"
#include "runtime/WorldView.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ww::runtime
{
    // What the consumer should do at a Step's target.
    //   Walk       — move toward (targetX, targetY, plane); arriving there ends the step.
    //   Transition — at (targetX, targetY, plane) — a standable tile adjacent to the
    //                transition's object — interact with the artifact's TransitionRecord
    //                indexed by transitionIndex and run its embedded chain to arrive at
    //                the transition's destination tile.
    enum class StepKind : uint8_t
    {
        Walk       = 0,
        Transition = 1,
    };

    // One emitted action of an assembled Plan. Fixed-size POD so a later C ABI step
    // can mirror it byte-for-byte. transitionIndex is the artifact-relative index
    // into ArtifactReader::transitions() for Transition steps; UINT32_MAX for Walk.
    struct Step
    {
        StepKind kind;
        uint8_t  plane;
        uint16_t pad;             // zero-filled
        int32_t  targetX;
        int32_t  targetY;
        uint32_t transitionIndex;
    };

    static_assert(sizeof(Step) == 16, "Step must be 16 bytes (POD wire-shape)");

    // An assembled Plan from the query's start tile to its goal tile. cost is the
    // sum of walked tile cost (cardinal 1.0, diagonal sqrt(2)) and transition tick
    // cost; the two units are comparable for ordering but not yet rescaled to pure
    // ticks (open tuning per the implementation plan).
    struct Plan
    {
        std::vector<Step> steps;
        float cost{};
    };

    // Orchestrates AreaSearch + TileSearch into a Step list. The area-graph A*
    // produces the route; tile-level A* refines each area-segment to legal moves;
    // this class stitches them into chunked WALK steps and Transition steps.
    //
    // One instance per search context (ADR 0007). Borrows the artifact + searches
    // (which themselves borrow the same artifact / view), holds reusable scratch
    // for the area route and the tile path. Not thread-safe; everything borrowed
    // must outlive the assembler.
    //
    // Phase 3d-2 scope: contiguous area route refined to tiles, WALK steps chunked
    // to a local walk range, Transition steps emitted at each area-edge. Deferred
    // to follow-on Phase 3d sub-steps: capability-predicate edge filtering (3d-3),
    // global-teleport frontier seeding (3d-4), the search-context pool.
    class PathAssembler
    {
    public:
        PathAssembler(const format::ArtifactReader &reader, WorldView &view,
                      AreaSearch &areaSearch, TileSearch &tileSearch);

        PathAssembler(const PathAssembler &) = delete;
        PathAssembler &operator=(const PathAssembler &) = delete;
        PathAssembler(PathAssembler &&) = default;
        PathAssembler &operator=(PathAssembler &&) = default;

        // Assemble a Plan from (startX, startY, startPlane) to (goalX, goalY,
        // goalPlane). Returns false (outPlan.steps empty) when either endpoint is
        // unstandable / off-area, the area route cannot be found, no standable
        // interact-tile sits within range of any transition origin on the route,
        // or any refined segment is unreachable. A same-area query skips the
        // area-graph search entirely; a start == goal query yields an empty step
        // list and cost 0.
        //
        // The capability-aware overload forwards a CapabilitySnapshot to
        // AreaSearch so transitions whose Requirements are not satisfied are
        // excluded from the area route; nullptr is equivalent to the no-snapshot
        // overload. The snapshot is consulted only for inter-area transition
        // gating — same-area routing (pure walking) is unfiltered by design.
        bool assemble(int32_t startX, int32_t startY, int32_t startPlane,
                      int32_t goalX, int32_t goalY, int32_t goalPlane,
                      Plan &outPlan);
        bool assemble(int32_t startX, int32_t startY, int32_t startPlane,
                      int32_t goalX, int32_t goalY, int32_t goalPlane,
                      const CapabilitySnapshot *capabilities, Plan &outPlan);

    private:
        // Find the closest standable tile to (originX, originY) that belongs to
        // `area`. The transition's object tile is permitted to be blocked, so the
        // player walks to an adjacent walkable tile and interacts from there. The
        // origin tile itself wins when it qualifies.
        bool resolveInteractTile(int32_t originX, int32_t originY, int32_t plane,
                                 int32_t area, int32_t &outX, int32_t &outY) const;

        // When the requested goal tile is blocked (off-area), find the nearest
        // standable tile within kGoalSnapRadius and report it (and its area) as
        // the effective goal. Returns false when the goal is buried too deep in
        // blocked terrain for any walkable stand-in to be found.
        bool resolveGoalTile(int32_t goalX, int32_t goalY, int32_t plane,
                             int32_t &outX, int32_t &outY, int32_t &outArea) const;

        // Refine (fromX, fromY) -> (toX, toY) inside `area` and append chunked
        // WALK steps to outPlan. Each step's target advances at most kWalkChunkTiles
        // along the refined path; the final step always lands on the end tile.
        // A zero-distance refinement appends nothing.
        bool appendWalkSegment(int32_t fromX, int32_t fromY, int32_t toX, int32_t toY,
                               int32_t plane, int32_t area, Plan &outPlan);

        // One hop of the area route at areaPath.steps[pathIndex]: walk from the
        // cursor to the transition's interact-tile in the prior area, emit the
        // Transition step, advance the cursor to the destination tile. Returns
        // false on any sub-step failure (bad index, no interact-tile, unreachable).
        bool appendTransitionHop(std::size_t pathIndex,
                                 std::span<const format::AreaEdgeRecord> edges,
                                 std::span<const format::TransitionRecord> transitions,
                                 int32_t &cursorX, int32_t &cursorY, int32_t &cursorPlane,
                                 Plan &outPlan);

        // Populate seedScratch with one FrontierSeed per global-origin transition
        // whose Requirements the borrowed snapshot satisfies and whose dest tile
        // lands in a valid area. Cleared first; left empty when the caller has
        // already determined the start tile is not teleport-allowed.
        void buildGlobalTeleportSeeds(const CapabilitySnapshot *capabilities);

        // When AreaSearch recorded a frontier-seeded entry in areaPath, emit a
        // Transition step at the cursor (the start tile — global teleports cast
        // in place) and snap the cursor to the transition's destination tile.
        // No-op when the area route walked out of startArea normally or when the
        // recorded transition index is out of range.
        void emitLeadingTransition(std::span<const format::TransitionRecord> transitions,
                                   int32_t &cursorX, int32_t &cursorY, int32_t &cursorPlane,
                                   Plan &outPlan);

        const format::ArtifactReader *artifact;
        WorldView *view;
        AreaSearch *areaSearch;
        TileSearch *tileSearch;
        AreaPath areaPath;                       // reusable scratch for the area-level route
        TilePath tilePath;                       // reusable scratch for each refined segment
        std::vector<FrontierSeed> seedScratch;   // reusable scratch for global-teleport frontier seeds
    };
}

#endif  // WORLDWALKER_RUNTIME_PATHASSEMBLER_H
