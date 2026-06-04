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

        // Chebyshev radius searched around a blocked goal tile for the nearest
        // standable stand-in. A flag dropped on a wall, a closed door, or the
        // footprint of an object (e.g. a ladder tile) is not itself standable;
        // rather than fail the whole query, snap the goal to the closest walkable
        // tile so the route lands the player as near the requested spot as the
        // collision allows. 3 reaches across a 2-wide object plus its wall ring.
        constexpr int32_t kGoalSnapRadius = 3;

        // Chebyshev radius from a baked transition's destination tile to the
        // goal tile (same plane only) below which the transition counts as a
        // "near-goal exit" — an alternative way to leave a teleport's landing
        // area that drops the player close to the goal. AreaSearch picks edges
        // by area-graph cost alone, so a single-edge exit that lands far from
        // the goal can beat a two-edge chain whose second edge drops you next
        // door. The teleport-landing loop tries each candidate in this radius
        // as an alternative second hop after the teleport, comparing realised
        // costs. ~1.5 screens — wide enough to catch the dungeon-cape resource
        // dungeon exits called out below, tight enough that only a handful of
        // edges qualify per query.
        constexpr int32_t kNearGoalRadius = 24;

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

        // Octile distance in the planner's cost units (cardinal 1.0, diagonal
        // sqrt(2)). An admissible lower bound on the tile-walk cost between two
        // same-plane tiles — obstacles only make the real path longer — so it is
        // safe to use to prune teleport candidates that cannot beat a known plan.
        float octileDistance(int32_t dx, int32_t dy)
        {
            dx = dx < 0 ? -dx : dx;
            dy = dy < 0 ? -dy : dy;
            const int32_t lo = dx < dy ? dx : dy;
            const int32_t hi = dx < dy ? dy : dx;
            return static_cast<float>(hi - lo) + static_cast<float>(lo) * 1.41421356f;
        }

        // True when a baked transition's destination is within kNearGoalRadius
        // (Chebyshev) of the goal tile on the same plane. Cross-plane edges
        // never qualify — octile distance across a plane band is meaningless,
        // and a transition whose destination plane disagrees with the goal
        // plane needs at least one more plane-changing hop the scan would not
        // see.
        bool isNearGoalExit(const format::TransitionRecord &t,
                            int32_t goalX, int32_t goalY, int32_t goalPlane)
        {
            if (static_cast<int32_t>(t.destPlane) != goalPlane)
            {
                return false;
            }
            const int32_t dx = std::abs(t.destX - goalX);
            const int32_t dy = std::abs(t.destY - goalY);
            return std::max(dx, dy) <= kNearGoalRadius;
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

    // Outward Chebyshev-ring scan for the nearest standable, in-area tile to a
    // blocked goal. Unlike resolveInteractTile this is not pinned to a known
    // area — a blocked tile has no area of its own — so it accepts the first
    // standable neighbour that belongs to any area, which is by construction the
    // room the wall/object sits against. Returns false when nothing standable
    // lies within kGoalSnapRadius (goal is deep in blocked terrain).
    bool PathAssembler::resolveGoalTile(int32_t goalX, int32_t goalY, int32_t plane,
                                        int32_t &outX, int32_t &outY, int32_t &outArea) const
    {
        for (int32_t r = 1; r <= kGoalSnapRadius; ++r)
        {
            for (int32_t dy = -r; dy <= r; ++dy)
            {
                for (int32_t dx = -r; dx <= r; ++dx)
                {
                    if (std::max(std::abs(dx), std::abs(dy)) != r)
                    {
                        continue;  // inner rings handled in earlier iterations
                    }
                    const int32_t x = goalX + dx;
                    const int32_t y = goalY + dy;
                    const int32_t area = view->areaAt(x, y, plane);
                    if (area >= 0 && view->isStandable(x, y, plane))
                    {
                        outX = x;
                        outY = y;
                        outArea = area;
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
                                   reqs.subspan(tx.requirementStart, tx.requirementCount),
                                   static_cast<data::TransitionKind>(tx.kind)))
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

    void PathAssembler::emitGlobalTeleport(uint32_t transitionIndex,
                                           int32_t &cursorX, int32_t &cursorY,
                                           int32_t &cursorPlane, Plan &outPlan)
    {
        const std::span<const format::TransitionRecord> transitions = artifact->transitions();
        if (transitionIndex >= transitions.size())
        {
            return;
        }
        const format::TransitionRecord &tx = transitions[transitionIndex];
        if (!isLegalPlane(cursorPlane) || !isLegalPlane(static_cast<int32_t>(tx.destPlane)))
        {
            return;
        }
        // A global teleport is cast in place: the Transition step sits at the
        // current cursor (start tile), then the cursor snaps to the dest.
        outPlan.steps.push_back({StepKind::Transition,
                                 static_cast<uint8_t>(cursorPlane), 0u,
                                 cursorX, cursorY, transitionIndex});
        outPlan.cost += tx.cost;
        cursorX = tx.destX;
        cursorY = tx.destY;
        cursorPlane = static_cast<int32_t>(tx.destPlane);
    }

    void PathAssembler::emitLeadingTransition(int32_t &cursorX, int32_t &cursorY,
                                              int32_t &cursorPlane, Plan &outPlan)
    {
        if (areaPath.leadingTransition < 0)
        {
            return;
        }
        emitGlobalTeleport(static_cast<uint32_t>(areaPath.leadingTransition),
                           cursorX, cursorY, cursorPlane, outPlan);
    }

    // The chained-route alternative the near-goal exit scan builds. Mirrors
    // emitGlobalTeleport + the prefix of assembleAreaRoute that would walk to
    // the first transition's interact tile and emit it, then defers the rest
    // of the route to assembleAreaRoute from the edge's other side. Skipping
    // ahead to a near-goal edge (instead of letting AreaSearch pick the
    // area-cheapest first hop) is the whole point: a single-edge exit at the
    // far end of a huge landing area can win on area cost yet lose by a
    // factor of ten once the intra-area walk is materialised.
    bool PathAssembler::tryTeleportNearGoalExit(int32_t startX, int32_t startY, int32_t startPlane,
                                                 const FrontierSeed &seed,
                                                 const format::AreaEdgeRecord &edge,
                                                 int32_t goalX, int32_t goalY, int32_t goalArea,
                                                 const CapabilitySnapshot *capabilities,
                                                 Plan &outAlt)
    {
        const std::span<const format::TransitionRecord> txs = artifact->transitions();
        if (edge.transitionIndex >= txs.size())
        {
            return false;
        }
        const format::TransitionRecord &T = txs[edge.transitionIndex];

        outAlt.steps.clear();
        outAlt.cost = 0.0f;

        // Cast the teleport in place: cursor snaps from the player's tile to
        // the seed's destination tile, on the seed's destination plane.
        int32_t cx = startX;
        int32_t cy = startY;
        int32_t cp = startPlane;
        emitGlobalTeleport(seed.transitionIndex, cx, cy, cp, outAlt);

        // The walk inside seed.destArea must run on the edge's origin plane —
        // appendWalkSegment is single-plane. A teleport landing on plane P
        // followed by an edge whose origin is plane Q means there is no walk
        // between them (a baked Transition would have to bridge the planes,
        // and the area-edge bookkeeping never produces one); just bail.
        const int32_t originPlane = static_cast<int32_t>(T.originPlane);
        if (cp != originPlane)
        {
            return false;
        }

        // Same Chebyshev-ring scan appendTransitionHop uses for the regular
        // area-route path: the origin tile is usually blocked (the loc itself),
        // so the player walks to an in-area standable neighbour and interacts
        // from there. fromArea must match seed.destArea here by construction —
        // the caller filters edges so — but resolveInteractTile re-checks it.
        int32_t interactX = 0;
        int32_t interactY = 0;
        if (!resolveInteractTile(T.originX, T.originY, originPlane,
                                  edge.fromArea, interactX, interactY))
        {
            return false;
        }

        if (!appendWalkSegment(cx, cy, interactX, interactY, cp,
                                edge.fromArea, outAlt))
        {
            return false;
        }

        const int32_t destPlane = static_cast<int32_t>(T.destPlane);
        if (!isLegalPlane(destPlane))
        {
            return false;
        }
        outAlt.steps.push_back({StepKind::Transition,
                                 static_cast<uint8_t>(originPlane), 0u,
                                 interactX, interactY, edge.transitionIndex});
        outAlt.cost += edge.cost;
        cx = T.destX;
        cy = T.destY;
        cp = destPlane;

        // Closing route runs over baked crossings only — the one global
        // teleport was already emitted, so the sub-route must not teleport
        // again (mirrors the noSeeds reasoning in the main teleport loop).
        const std::span<const FrontierSeed> noSeeds{};
        return assembleAreaRoute(cx, cy, cp, edge.toArea, goalX, goalY, goalArea,
                                  capabilities, noSeeds, outAlt);
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
        if (startArea < 0)
        {
            return false;
        }
        int32_t goalArea = view->areaAt(goalX, goalY, goalPlane);
        if (goalArea < 0)
        {
            // The requested goal tile is blocked (wall / closed door / object
            // footprint). Snap to the nearest standable tile so the route still
            // lands the player against the intended spot instead of failing.
            if (!resolveGoalTile(goalX, goalY, goalPlane, goalX, goalY, goalArea))
            {
                return false;
            }
        }

        // Teleport seeds feed both the inter-area backbone search and the
        // goal-area landing optimisation below, so build them once up front.
        seedScratch.clear();
        const bool teleportAllowed = isTeleportAllowed(*artifact, startX, startY, startPlane);
        if (teleportAllowed)
        {
            buildGlobalTeleportSeeds(capabilities);
        }

        // Pre-scan baked area edges for "near-goal exits" — transitions whose
        // destination tile lands within kNearGoalRadius of the goal on the goal
        // plane. The teleport-landing loop tries each as an alternative second
        // hop, sidestepping AreaSearch's area-cost-only ordering when a longer
        // chain has a much shorter intra-area walk. Cleared on every assemble
        // since the radius is goal-dependent. Cheap: one pass over the edge
        // span, a few comparisons per edge.
        nearGoalEdgeScratch.clear();
        {
            const std::span<const format::TransitionRecord> txsScan = artifact->transitions();
            const std::span<const format::AreaEdgeRecord> edgesScan = artifact->areaEdges();
            for (std::size_t i = 0; i < edgesScan.size(); ++i)
            {
                const format::AreaEdgeRecord &E = edgesScan[i];
                if (E.transitionIndex >= txsScan.size())
                {
                    continue;
                }
                const format::TransitionRecord &T = txsScan[E.transitionIndex];
                // Global-origin transitions are seeded separately and produce
                // no fromArea-keyed AreaEdge in practice; skip defensively so a
                // stray flagged record cannot be replayed here as a baked hop.
                if ((T.flags & format::kTransitionFlagGlobalOrigin) != 0u)
                {
                    continue;
                }
                if (isNearGoalExit(T, goalX, goalY, goalPlane))
                {
                    nearGoalEdgeScratch.push_back(static_cast<int32_t>(i));
                }
            }
        }

        // Baseline route: a same-area query is a pure tile-level walk; otherwise
        // the seeded area-graph search (which already teleports across area
        // boundaries when that is cheaper).
        Plan best;
        bool haveBest = false;
        if (startArea == goalArea)
        {
            Plan walk;
            if (appendWalkSegment(startX, startY, goalX, goalY, startPlane, startArea, walk))
            {
                best = std::move(walk);
                haveBest = true;
            }
        }
        else
        {
            Plan route;
            if (assembleAreaRoute(startX, startY, startPlane, startArea,
                                  goalX, goalY, goalArea, capabilities,
                                  std::span<const FrontierSeed>(seedScratch), route))
            {
                best = std::move(route);
                haveBest = true;
            }
        }

        // Teleport landing optimisation. The area graph only knows that a
        // teleport "reaches some area", not which one lands CLOSEST to the goal
        // in tile terms — and it never seeds for a same-area query at all. Both
        // matter when the goal sits deep inside a huge area (the overworld
        // landmass is only a handful of areas): the nearest lodestone can save a
        // several-hundred-tile walk, and a teleport that lands in a *different*
        // area one stairs/ladder away (e.g. a dungeon-cape resource-dungeon
        // landing that climbs straight out next to the goal) can beat both the
        // walk and any lodestone. So consider, at tile level, every seedable
        // teleport, realising teleport -> baked area route -> closing walk and
        // keeping the cheapest that beats the baseline.
        //
        // Ordering uses an admissible lower-bound estimate so the scan stops as
        // soon as no remaining candidate can beat the best realised plan:
        //   - lands in the goal area: cost + octile(dest, goal). octile is a
        //     valid tile lower bound *within one area* (a single coordinate
        //     band), so this is tight.
        //   - lands elsewhere: cost alone. A stairs/ladder warps between
        //     coordinate bands, so octile(dest, goal) across it is meaningless
        //     (it would wildly over-estimate and prune real routes); the
        //     teleport cost is the only band-safe lower bound. This stays loose
        //     but correct — and because the closing area route adds real cost,
        //     such candidates are only realised when the baseline already costs
        //     more than the bare teleport (i.e. exactly when a teleport can win).
        if (teleportAllowed)
        {
            const std::span<const format::TransitionRecord> txs = artifact->transitions();
            teleCandidateScratch.clear();
            for (std::size_t s = 0; s < seedScratch.size(); ++s)
            {
                const FrontierSeed &seed = seedScratch[s];
                const format::TransitionRecord &tx = txs[seed.transitionIndex];
                const float estimate = seed.destArea == goalArea
                    ? seed.cost + octileDistance(tx.destX - goalX, tx.destY - goalY)
                    : seed.cost;
                teleCandidateScratch.push_back({estimate, static_cast<uint32_t>(s)});
            }
            std::sort(teleCandidateScratch.begin(), teleCandidateScratch.end(),
                      [](const TeleCandidate &a, const TeleCandidate &b)
                      { return a.estimate < b.estimate; });
            // The closing area route uses baked crossings only — the one global
            // teleport is already emitted, so the sub-route must not teleport
            // again (which would double-count and tangle the cost accounting).
            const std::span<const FrontierSeed> noSeeds{};
            const std::span<const format::AreaEdgeRecord> areaEdgesAll = artifact->areaEdges();
            for (const TeleCandidate &cand : teleCandidateScratch)
            {
                if (haveBest && cand.estimate >= best.cost)
                {
                    break;  // estimate is a lower bound; nothing cheaper remains
                }
                const FrontierSeed &seed = seedScratch[cand.seedIndex];
                Plan tele;
                int32_t cx = startX;
                int32_t cy = startY;
                int32_t cp = startPlane;
                emitGlobalTeleport(seed.transitionIndex, cx, cy, cp, tele);
                // From the teleport's dest, route to the goal: a closing walk
                // when it landed in the goal area, otherwise teleport-dest area
                // -> goal area over baked crossings plus the closing walk. Both
                // are exactly what assembleAreaRoute produces (a same-area area
                // search yields a single trivial step and just the closing walk).
                if (assembleAreaRoute(cx, cy, cp, seed.destArea, goalX, goalY, goalArea,
                                      capabilities, noSeeds, tele))
                {
                    if (!haveBest || tele.cost < best.cost)
                    {
                        best = std::move(tele);
                        haveBest = true;
                    }
                }

                // Near-goal-exit alternatives. AreaSearch picks the first hop
                // out of seed.destArea on area-cost alone, so a single direct
                // edge to goalArea wins over a two-edge chain even when the
                // chain's intra-area walk is an order of magnitude shorter —
                // the dungeon-cape resource-dungeon landing the comment above
                // calls out is exactly that pattern. For each baked transition
                // whose dest tile is near the goal and whose fromArea is the
                // seed's landing area, build and price the chained route and
                // keep it if it beats `best`. nearGoalEdgeScratch was filtered
                // against goal plane + global flag at scan time, so the inner
                // body is just an area-match filter and the realised-cost
                // build.
                for (int32_t edgeIdx : nearGoalEdgeScratch)
                {
                    const format::AreaEdgeRecord &E = areaEdgesAll[edgeIdx];
                    if (E.fromArea != seed.destArea)
                    {
                        continue;
                    }
                    Plan alt;
                    if (!tryTeleportNearGoalExit(startX, startY, startPlane, seed, E,
                                                  goalX, goalY, goalArea, capabilities, alt))
                    {
                        continue;
                    }
                    if (!haveBest || alt.cost < best.cost)
                    {
                        best = std::move(alt);
                        haveBest = true;
                    }
                }
            }
        }

        if (!haveBest)
        {
            return false;
        }
        outPlan = std::move(best);
        return true;
    }

    // Cursor tracks the player's notional tile as the route plays out: walks
    // advance it, transitions snap it to the destination, the closing walk drives
    // it to the goal. A leading global teleport (recorded by AreaSearch in
    // areaPath.leadingTransition) snaps the cursor from start to the teleport's
    // destination before any walking — the player casts in place.
    bool PathAssembler::assembleAreaRoute(int32_t startX, int32_t startY, int32_t startPlane,
                                          int32_t startArea, int32_t goalX, int32_t goalY,
                                          int32_t goalArea,
                                          const CapabilitySnapshot *capabilities,
                                          std::span<const FrontierSeed> seeds, Plan &outPlan)
    {
        if (!areaSearch->findPath(startArea, goalArea, capabilities, seeds, areaPath))
        {
            return false;
        }
        const std::span<const format::AreaEdgeRecord> edges = artifact->areaEdges();
        const std::span<const format::TransitionRecord> transitions = artifact->transitions();
        int32_t cursorX = startX;
        int32_t cursorY = startY;
        int32_t cursorPlane = startPlane;
        emitLeadingTransition(cursorX, cursorY, cursorPlane, outPlan);
        for (std::size_t i = 1; i < areaPath.steps.size(); ++i)
        {
            if (!appendTransitionHop(i, edges, transitions, cursorX, cursorY, cursorPlane, outPlan))
            {
                return false;
            }
        }
        if (!appendWalkSegment(cursorX, cursorY, goalX, goalY, cursorPlane, goalArea, outPlan))
        {
            return false;
        }
        return true;
    }
}
