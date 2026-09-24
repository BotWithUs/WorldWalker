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
        //
        // A start the walker has no map for (an instance whose goal lies outside
        // it, or a tile in no baked square) is planned by assembleTeleportOut:
        // a global teleport first, then the normal route from its landing.
        //
        // When the borrowed WorldView has a dynamic region installed, everything
        // above is bypassed for assembleInstanceRoute: the baked area graph does
        // not describe an instance's terrain, so the plan is pure tile-level
        // walking within the descriptor grid and the capability snapshot is
        // unused (there are no transitions to gate).
        bool assemble(int32_t startX, int32_t startY, int32_t startPlane,
                      int32_t goalX, int32_t goalY, int32_t goalPlane,
                      Plan &outPlan);
        bool assemble(int32_t startX, int32_t startY, int32_t startPlane,
                      int32_t goalX, int32_t goalY, int32_t goalPlane,
                      const CapabilitySnapshot *capabilities, Plan &outPlan);

    private:
        // Plan from a start the walker has no map for. The only way on is a
        // global teleport, so the plan is one: every teleport the capability
        // snapshot admits is seeded from the start, and the cheapest landing
        // that routes to the goal wins. The start counts as teleport-allowed,
        // since no wilderness box or no-teleport zone is known there; in combat
        // nothing can be cast, so the plan fails. Any installed instance is set
        // aside meanwhile: everything after the teleport is static world.
        bool assembleTeleportOut(int32_t startX, int32_t startY, int32_t startPlane,
                                 int32_t goalX, int32_t goalY, int32_t goalPlane,
                                 const CapabilitySnapshot *capabilities, Plan &outPlan);

        // Find the closest standable tile to (originX, originY) that belongs to
        // `area`. The transition's object tile is permitted to be blocked, so the
        // player walks to an adjacent walkable tile and interacts from there. The
        // origin tile itself wins when it qualifies; otherwise the innermost ring
        // with a candidate wins, and within it the tile nearest (nearX, nearY),
        // the player's cursor, so the approach walk is as short as it can be.
        bool resolveInteractTile(int32_t originX, int32_t originY, int32_t plane,
                                 int32_t area, int32_t nearX, int32_t nearY,
                                 int32_t &outX, int32_t &outY) const;

        // When the requested goal tile is blocked (off-area), find the nearest
        // standable tile within kGoalSnapRadius and report it (and its area) as
        // the effective goal. Returns false when the goal is buried too deep in
        // blocked terrain for any walkable stand-in to be found.
        //
        // `requireArea` demands the stand-in also belong to a baked area, and
        // reports it through outArea — the static-world case, where a tile with
        // no area is unreachable by definition. Inside a dynamic region nothing
        // has a baked area, so the instance path passes false and outArea is
        // left untouched.
        bool resolveGoalTile(int32_t goalX, int32_t goalY, int32_t plane, bool requireArea,
                             int32_t &outX, int32_t &outY, int32_t &outArea) const;

        // Plan a route wholly inside a dynamic region (instance), appending Walk
        // steps to outPlan. Returns false when either endpoint is outside the
        // descriptor grid, the planes differ, or no walkable route exists.
        bool assembleInstanceRoute(int32_t startX, int32_t startY, int32_t startPlane,
                                   int32_t goalX, int32_t goalY, int32_t goalPlane,
                                   Plan &outPlan);

        // Refine (fromX, fromY) -> (toX, toY) inside `area` and append chunked
        // WALK steps to outPlan. Each step's target advances at most kWalkChunkTiles
        // along the refined path; the final step always lands on the end tile.
        // A zero-distance refinement appends nothing.
        bool appendWalkSegment(int32_t fromX, int32_t fromY, int32_t toX, int32_t toY,
                               int32_t plane, int32_t area, Plan &outPlan);

        // Take one baked area edge from the cursor: walk inside edge.fromArea to
        // the underlying transition's interact tile, emit the Transition step,
        // and snap the cursor to the transition's destination tile. The one
        // place a crossing is materialised — the area-route hop and the
        // near-goal-exit chain both come through here. Returns false on a bad
        // transition index, an illegal plane, a cursor that is not on the
        // transition's origin plane, no interact tile, or an unreachable walk.
        bool takeEdge(const format::AreaEdgeRecord &edge,
                      int32_t &cursorX, int32_t &cursorY, int32_t &cursorPlane,
                      Plan &outPlan);

        // One hop of the area route at areaPath.steps[pathIndex]: validate the
        // recorded edge index and take that edge. Returns false on any sub-step
        // failure (bad index, no interact-tile, unreachable).
        bool appendTransitionHop(std::size_t pathIndex,
                                 int32_t &cursorX, int32_t &cursorY, int32_t &cursorPlane,
                                 Plan &outPlan);

        // Run the seeded area-graph search startArea -> goalArea and stitch the
        // resulting area route into Walk/Transition steps, appending to outPlan
        // from (startX, startY, startPlane). `seeds` are the global-teleport
        // frontier seeds offered to the area search (empty = baked crossings
        // only). Returns false when no area route exists or a segment is
        // unreachable.
        bool assembleAreaRoute(int32_t startX, int32_t startY, int32_t startPlane,
                               int32_t startArea, int32_t goalX, int32_t goalY,
                               int32_t goalArea, const CapabilitySnapshot *capabilities,
                               std::span<const FrontierSeed> seeds, Plan &outPlan);

        // The baseline plan before any teleport-landing improvement: a pure
        // tile walk for a same-area query, otherwise the seeded area route.
        // Writes into outPlan (cleared first) and returns true on success.
        bool assembleBaseline(int32_t startX, int32_t startY, int32_t startPlane,
                              int32_t startArea, int32_t goalX, int32_t goalY,
                              int32_t goalArea, const CapabilitySnapshot *capabilities,
                              Plan &outPlan);

        // Emit a global-teleport Transition step at the cursor (cast in place)
        // and snap the cursor to the transition's destination. No-op when the
        // index is out of range or a plane is illegal.
        void emitGlobalTeleport(uint32_t transitionIndex,
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
        void emitLeadingTransition(int32_t &cursorX, int32_t &cursorY, int32_t &cursorPlane,
                                   Plan &outPlan);

        // A global teleport whose dest lands in the goal's area, paired with an
        // admissible lower-bound cost estimate (teleport cost + octile to goal)
        // used to order and prune candidates. seedIndex indexes seedScratch.
        struct TeleCandidate
        {
            float    estimate;
            uint32_t seedIndex;
        };

        // Cached metadata for one baked area edge whose underlying
        // transition's destination tile is within kNearGoalRadius of the
        // query's goal tile on the goal plane (an "exit near the goal").
        // Built once per assemble by scanNearGoalExits; the inner
        // teleport-landing loop matches by fromArea and prunes by
        // closingBound + seed.cost without re-fetching the underlying
        // TransitionRecord per pair.
        struct NearGoalEdge
        {
            int32_t edgeIdx;        // index into artifact->areaEdges()
            int32_t fromArea;       // hoisted from the AreaEdgeRecord
            // E.cost + octile(T.destX-goalX, T.destY-goalY). Adding
            // seed.cost yields an admissible lower bound on the realised
            // (teleport -> walk to edge interact tile -> edge -> closing
            // walk) plan for this pair; skipping when the bound already
            // beats the running best avoids the full assembleAreaRoute
            // call hidden inside tryTeleportNearGoalExit.
            float   closingBound;
        };

        // Fill nearGoalEdgeScratch with every baked, non-global area edge whose
        // transition lands within kNearGoalRadius of the goal on the goal
        // plane, via the reader's per-square edge buckets (3x3 squares around
        // the goal). Cleared first; the radius is goal-dependent.
        void scanNearGoalExits(int32_t goalX, int32_t goalY, int32_t goalPlane);

        // Order seedScratch's teleports by an admissible lower bound on the
        // plan they can produce (see improveWithTeleports), cheapest first,
        // into teleCandidateScratch.
        void rankTeleportCandidates(int32_t goalX, int32_t goalY, int32_t goalArea);

        // Teleport landing optimisation: realise, at tile level, every seedable
        // teleport as teleport -> baked area route -> closing walk (plus the
        // near-goal-exit chains), keeping the cheapest that beats ioBest.
        // Candidates are visited cheapest-bound first so the loop stops as soon
        // as no remaining one can win.
        void improveWithTeleports(int32_t startX, int32_t startY, int32_t startPlane,
                                  int32_t goalX, int32_t goalY, int32_t goalArea,
                                  const CapabilitySnapshot *capabilities,
                                  Plan &ioBest, bool &ioHaveBest);

        // Cast `seed` in place, then route from its destination to the goal
        // over baked crossings only. Writes into outAlt (cleared first).
        bool tryTeleportLanding(int32_t startX, int32_t startY, int32_t startPlane,
                                const FrontierSeed &seed,
                                int32_t goalX, int32_t goalY, int32_t goalArea,
                                const CapabilitySnapshot *capabilities, Plan &outAlt);

        // For every near-goal exit leaving `seed`'s landing area, price the
        // chained route (teleport -> walk -> exit -> closing route) and keep
        // it in ioBest when it wins. Pairs whose lower bound cannot beat
        // ioBest are skipped before any search runs.
        void tryNearGoalExits(int32_t startX, int32_t startY, int32_t startPlane,
                              const FrontierSeed &seed,
                              int32_t goalX, int32_t goalY, int32_t goalArea,
                              const CapabilitySnapshot *capabilities,
                              Plan &ioBest, bool &ioHaveBest);

        // Try a chained route — emit the global teleport in `seed`, walk inside
        // its destination area to `edge`'s origin, take `edge`, then run the
        // closing area route from `edge.toArea` to (goalX, goalY, goalArea).
        // Writes the resulting Plan into outAlt (cleared first) and returns
        // true on success. Failure leaves outAlt in an indeterminate state —
        // callers must treat outAlt as junk when the return is false. Used by
        // the teleport-landing optimisation to consider exits that AreaSearch
        // would not pick on area-cost alone but that drop the player next door
        // to the goal (the "dungeon-cape resource-dungeon" pattern called out
        // in improveWithTeleports).
        bool tryTeleportNearGoalExit(int32_t startX, int32_t startY, int32_t startPlane,
                                     const FrontierSeed &seed,
                                     const format::AreaEdgeRecord &edge,
                                     int32_t goalX, int32_t goalY, int32_t goalArea,
                                     const CapabilitySnapshot *capabilities,
                                     Plan &outAlt);

        const format::ArtifactReader *artifact;
        WorldView *view;
        AreaSearch *areaSearch;
        TileSearch *tileSearch;
        AreaPath areaPath;                       // reusable scratch for the area-level route
        TilePath tilePath;                       // reusable scratch for each refined segment
        std::vector<FrontierSeed> seedScratch;   // reusable scratch for global-teleport frontier seeds
        std::vector<TeleCandidate> teleCandidateScratch;  // reusable scratch for goal-area teleport ranking
        std::vector<NearGoalEdge> nearGoalEdgeScratch;   // reusable scratch for near-goal baked edges (idx + fromArea + closingBound)
    };
}

#endif  // WORLDWALKER_RUNTIME_PATHASSEMBLER_H
