#include "runtime/PathAssembler.h"

#include "format/Artifact.h"
#include "runtime/InstanceMap.h"
#include "runtime/TeleportPolicy.h"
#include "runtime/TileScan.h"

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

        // Square half-width covering kNearGoalRadius: with a 64-tile mapsquare
        // the dest can sit at most one square away (worst case when the goal
        // hugs its own square's edge), so the exit scan visits a 3x3 grid.
        constexpr int kNearGoalSquareRadius = 1;

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

        // Adopt `candidate` as the best plan so far when it is the first one or
        // strictly cheaper than the incumbent. Moves out of candidate on adopt.
        void keepIfCheaper(Plan &candidate, Plan &ioBest, bool &ioHaveBest)
        {
            if (!ioHaveBest || candidate.cost < ioBest.cost)
            {
                ioBest = std::move(candidate);
                ioHaveBest = true;
            }
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

    // The origin tile (r=0) wins when it is itself standable and in-area;
    // otherwise the innermost ring with a qualifying tile does, ranked by
    // distance to the cursor, so the result is always a closest valid
    // neighbour of the object that costs the least walking to reach.
    bool PathAssembler::resolveInteractTile(int32_t originX, int32_t originY, int32_t plane,
                                            int32_t area, int32_t nearX, int32_t nearY,
                                            int32_t &outX, int32_t &outY) const
    {
        const auto standableInArea = [&](int32_t x, int32_t y)
        {
            return view->isStandable(x, y, plane) && view->areaAt(x, y, plane) == area;
        };
        return findNearestTile(originX, originY, kInteractSearchRadius, true,
                               standableInArea, nearX, nearY, outX, outY);
    }

    // Unlike resolveInteractTile this is not pinned to a known area — a blocked
    // tile has no area of its own — so it accepts the closest standable
    // neighbour that belongs to any area, which is by construction the room the
    // wall/object sits against. The centre is skipped: the caller already knows
    // the goal tile itself is blocked. Returns false when nothing standable lies
    // within kGoalSnapRadius (goal is deep in blocked terrain).
    bool PathAssembler::resolveGoalTile(int32_t goalX, int32_t goalY, int32_t plane,
                                        bool requireArea,
                                        int32_t &outX, int32_t &outY, int32_t &outArea) const
    {
        const auto standIn = [&](int32_t x, int32_t y)
        {
            return view->isStandable(x, y, plane)
                && (!requireArea || view->areaAt(x, y, plane) >= 0);
        };
        if (!findNearestTile(goalX, goalY, kGoalSnapRadius, false, standIn, outX, outY))
        {
            return false;
        }
        if (requireArea)
        {
            outArea = view->areaAt(outX, outY, plane);
        }
        return true;
    }

    // Plan a route wholly inside a dynamic region (instance).
    //
    // None of the baked graph applies here. An instance is stitched from 8x8
    // chunks copied out of scattered source regions whose areas are unrelated to
    // each other, and the footprint the instance occupies is not baked at all —
    // so areaAt answers -1 for every tile in it and the area-level backbone has
    // nothing to route over. What survives is per-tile collision, which
    // WorldView resolves through the chunk descriptors. This is therefore a plain
    // tile-level A* with the area constraint off, chunked into Walk steps by the
    // same appendWalkSegment the static path uses.
    //
    // Three limits, deliberate rather than accidental:
    //   * No transitions. Doors, ladders and stairs inside the instance are baked
    //     as area edges, and there are no areas here, so they are not used. A
    //     player-owned house does not need them; a Dungeoneering floor will.
    //   * No plane changes, for the same reason — a plane change IS a transition.
    //   * Both endpoints must lie inside the descriptor grid. Routing between an
    //     instance and the overworld needs an exit transition nothing bakes yet.
    //     Failing here is the honest answer: the static tiles that happen to share
    //     the instance's coordinates describe unrelated terrain, so planning
    //     through them would walk the avatar into scenery.
    bool PathAssembler::assembleInstanceRoute(int32_t startX, int32_t startY, int32_t startPlane,
                                              int32_t goalX, int32_t goalY, int32_t goalPlane,
                                              Plan &outPlan)
    {
        const InstanceMap *map = view->instanceMap();
        if (map == nullptr
            || !map->coversTile(startX, startY, startPlane)
            || !map->coversTile(goalX, goalY, goalPlane)
            || startPlane != goalPlane)
        {
            return false;
        }
        if (!view->isStandable(startX, startY, startPlane))
        {
            return false;
        }
        int32_t targetX = goalX;
        int32_t targetY = goalY;
        if (!view->isStandable(goalX, goalY, goalPlane))
        {
            // Same courtesy the static path extends: a goal on a wall or an
            // object footprint snaps to the nearest standable neighbour so the
            // route still lands the player against the intended spot.
            int32_t unusedArea = -1;
            if (!resolveGoalTile(goalX, goalY, goalPlane, false, targetX, targetY, unusedArea))
            {
                return false;
            }
        }
        return appendWalkSegment(startX, startY, targetX, targetY, startPlane,
                                 TileSearch::kAnyArea, outPlan);
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
        // Reserve up front so a long segment (kMaxExpansions tiles in the worst
        // case) does not trigger geometric grows on outPlan.steps mid-emit.
        const std::size_t chunks = (n - 1u + kWalkChunkTiles - 1u) / kWalkChunkTiles;
        outPlan.steps.reserve(outPlan.steps.size() + chunks);
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

    bool PathAssembler::takeEdge(const format::AreaEdgeRecord &edge,
                                 int32_t &cursorX, int32_t &cursorY, int32_t &cursorPlane,
                                 Plan &outPlan)
    {
        const std::span<const format::TransitionRecord> transitions = artifact->transitions();
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
        // Walks cannot cross planes (Step.plane invariant). The walk toward the
        // interact tile must therefore run on the transition's origin plane —
        // which had better match the cursor's plane, because each area is
        // single-plane and the cursor sits in edge.fromArea. A mismatch means
        // either (a) the artifact has an AreaEdge whose origin plane disagrees
        // with its fromArea's plane — a build bug — or (b) the cursor drifted
        // across a plane without a Transition step. Both are bugs in something
        // upstream of us; failing loud beats walking on the wrong plane.
        if (cursorPlane != originPlane)
        {
            return false;
        }
        int32_t interactX = 0;
        int32_t interactY = 0;
        if (!resolveInteractTile(tx.originX, tx.originY, originPlane, edge.fromArea,
                                 cursorX, cursorY, interactX, interactY))
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

    bool PathAssembler::appendTransitionHop(std::size_t pathIndex,
                                            int32_t &cursorX, int32_t &cursorY, int32_t &cursorPlane,
                                            Plan &outPlan)
    {
        const std::span<const format::AreaEdgeRecord> edges = artifact->areaEdges();
        const int32_t edgeIdx = areaPath.steps[pathIndex].viaEdge;
        if (edgeIdx < 0 || static_cast<std::size_t>(edgeIdx) >= edges.size())
        {
            return false;
        }
        return takeEdge(edges[static_cast<std::size_t>(edgeIdx)],
                        cursorX, cursorY, cursorPlane, outPlan);
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
        // Walk the pre-indexed global-origin list (built once at artifact
        // load, kept in sync by append/truncate) instead of the full
        // transition span. Cuts a per-query O(transitions) scan — typically
        // 15k+ entries — to O(globals), usually a few dozen.
        const std::span<const uint32_t> globals = artifact->globalOriginTransitions();
        for (uint32_t i : globals)
        {
            const format::TransitionRecord &tx = transitions[i];
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

    // The chained-route alternative the near-goal exit scan builds. Casts the
    // teleport, takes the near-goal edge from its landing area, then defers the
    // rest of the route to assembleAreaRoute from the edge's other side.
    // Skipping ahead to a near-goal edge (instead of letting AreaSearch pick the
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

        // The seed's transition was requirement-gated upstream, but this exit
        // edge's was not: the near-goal scan filters on geometry only. Gate it
        // here like every other crossing — a skill-gated shortcut the snapshot
        // rejects must not ride into the plan via the chained alternative (the
        // executor would fail it, and every re-plan would rebuild it).
        const std::span<const format::RequirementRecord> reqs = artifact->requirements();
        const uint64_t reqEnd = static_cast<uint64_t>(T.requirementStart) + T.requirementCount;
        if (reqEnd > reqs.size()
            || !meetsRequirements(capabilities,
                                  reqs.subspan(T.requirementStart, T.requirementCount),
                                  static_cast<data::TransitionKind>(T.kind)))
        {
            return false;
        }

        outAlt.steps.clear();
        outAlt.cost = 0.0f;

        // Cast the teleport in place: cursor snaps from the player's tile to
        // the seed's destination tile, on the seed's destination plane. takeEdge
        // then insists the landing plane is the edge's origin plane — a teleport
        // landing on plane P followed by an edge whose origin is plane Q has no
        // walk between them, and the area-edge bookkeeping never bridges that.
        int32_t cx = startX;
        int32_t cy = startY;
        int32_t cp = startPlane;
        emitGlobalTeleport(seed.transitionIndex, cx, cy, cp, outAlt);
        if (!takeEdge(edge, cx, cy, cp, outAlt))
        {
            return false;
        }

        // Closing route runs over baked crossings only — the one global
        // teleport was already emitted, so the sub-route must not teleport
        // again (mirrors the noSeeds reasoning in improveWithTeleports).
        const std::span<const FrontierSeed> noSeeds{};
        return assembleAreaRoute(cx, cy, cp, edge.toArea, goalX, goalY, goalArea,
                                  capabilities, noSeeds, outAlt);
    }

    // Bucketed lookup: ArtifactReader::nearGoalEdgeBucket pre-groups baked
    // edges by (destPlane, destSquareX, destSquareY), so the scan visits only
    // the 3x3 squares within kNearGoalRadius (24 tiles, less than one square
    // width) of the goal instead of every baked edge. Each surviving entry
    // caches fromArea (skips the inner loop's edge re-deref) and a closing
    // lower bound (E.cost + octile(T.dest, goal)) used to prune the per-(seed,
    // edge) pair before any A* work.
    void PathAssembler::scanNearGoalExits(int32_t goalX, int32_t goalY, int32_t goalPlane)
    {
        nearGoalEdgeScratch.clear();
        const std::span<const format::TransitionRecord> txs = artifact->transitions();
        const std::span<const format::AreaEdgeRecord> edges = artifact->areaEdges();
        const int goalSquareX = goalX >> 6;
        const int goalSquareY = goalY >> 6;
        for (int dsy = -kNearGoalSquareRadius; dsy <= kNearGoalSquareRadius; ++dsy)
        {
            for (int dsx = -kNearGoalSquareRadius; dsx <= kNearGoalSquareRadius; ++dsx)
            {
                const std::span<const uint32_t> bucket =
                    artifact->nearGoalEdgeBucket(goalPlane, goalSquareX + dsx, goalSquareY + dsy);
                for (uint32_t edgeIdx : bucket)
                {
                    const format::AreaEdgeRecord &E = edges[edgeIdx];
                    const format::TransitionRecord &T = txs[E.transitionIndex];
                    if (!isNearGoalExit(T, goalX, goalY, goalPlane))
                    {
                        continue;
                    }
                    const float closingBound =
                        E.cost + octileDistance(T.destX - goalX, T.destY - goalY);
                    nearGoalEdgeScratch.push_back({static_cast<int32_t>(edgeIdx),
                                                   E.fromArea, closingBound});
                }
            }
        }
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
        // Inside a dynamic region the baked area graph describes none of the
        // terrain under the player, so the whole area-level machinery below —
        // areaAt endpoints, teleport seeding, the near-goal edge scan — is
        // skipped rather than fed coordinates it cannot describe.
        if (view->isInstanced())
        {
            return assembleInstanceRoute(startX, startY, startPlane,
                                         goalX, goalY, goalPlane, outPlan);
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
            if (!resolveGoalTile(goalX, goalY, goalPlane, true, goalX, goalY, goalArea))
            {
                return false;
            }
        }

        // Teleport seeds feed both the inter-area backbone search and the
        // goal-area landing optimisation, so build them once up front.
        seedScratch.clear();
        const bool teleportAllowed = isTeleportAllowed(*artifact, startX, startY, startPlane);
        if (teleportAllowed)
        {
            buildGlobalTeleportSeeds(capabilities);
        }
        scanNearGoalExits(goalX, goalY, goalPlane);

        Plan best;
        bool haveBest = assembleBaseline(startX, startY, startPlane, startArea,
                                         goalX, goalY, goalArea, capabilities, best);
        if (teleportAllowed)
        {
            improveWithTeleports(startX, startY, startPlane, goalX, goalY, goalArea,
                                 capabilities, best, haveBest);
        }
        if (!haveBest)
        {
            return false;
        }
        outPlan = std::move(best);
        return true;
    }

    // Baseline route: a same-area query is a pure tile-level walk; otherwise
    // the seeded area-graph search (which already teleports across area
    // boundaries when that is cheaper).
    bool PathAssembler::assembleBaseline(int32_t startX, int32_t startY, int32_t startPlane,
                                         int32_t startArea, int32_t goalX, int32_t goalY,
                                         int32_t goalArea, const CapabilitySnapshot *capabilities,
                                         Plan &outPlan)
    {
        outPlan.steps.clear();
        outPlan.cost = 0.0f;
        bool ok = false;
        if (startArea == goalArea)
        {
            ok = appendWalkSegment(startX, startY, goalX, goalY, startPlane, startArea, outPlan);
        }
        else
        {
            ok = assembleAreaRoute(startX, startY, startPlane, startArea,
                                   goalX, goalY, goalArea, capabilities,
                                   std::span<const FrontierSeed>(seedScratch), outPlan);
        }
        if (!ok)
        {
            outPlan.steps.clear();
            outPlan.cost = 0.0f;
        }
        return ok;
    }

    // Ordering uses an admissible lower-bound estimate so the landing scan
    // stops as soon as no remaining candidate can beat the best realised plan:
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
    void PathAssembler::rankTeleportCandidates(int32_t goalX, int32_t goalY, int32_t goalArea)
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
    }

    // The area graph only knows that a teleport "reaches some area", not which
    // one lands CLOSEST to the goal in tile terms — and it never seeds for a
    // same-area query at all. Both matter when the goal sits deep inside a
    // huge area (the overworld landmass is only a handful of areas): the
    // nearest lodestone can save a several-hundred-tile walk, and a teleport
    // that lands in a *different* area one stairs/ladder away (e.g. a
    // dungeon-cape resource-dungeon landing that climbs straight out next to
    // the goal) can beat both the walk and any lodestone. So consider, at tile
    // level, every seedable teleport, realising teleport -> baked area route ->
    // closing walk and keeping the cheapest that beats the baseline.
    void PathAssembler::improveWithTeleports(int32_t startX, int32_t startY, int32_t startPlane,
                                             int32_t goalX, int32_t goalY, int32_t goalArea,
                                             const CapabilitySnapshot *capabilities,
                                             Plan &ioBest, bool &ioHaveBest)
    {
        rankTeleportCandidates(goalX, goalY, goalArea);
        for (const TeleCandidate &cand : teleCandidateScratch)
        {
            if (ioHaveBest && cand.estimate >= ioBest.cost)
            {
                break;  // estimate is a lower bound; nothing cheaper remains
            }
            const FrontierSeed &seed = seedScratch[cand.seedIndex];
            Plan tele;
            if (tryTeleportLanding(startX, startY, startPlane, seed,
                                   goalX, goalY, goalArea, capabilities, tele))
            {
                keepIfCheaper(tele, ioBest, ioHaveBest);
            }
            tryNearGoalExits(startX, startY, startPlane, seed,
                             goalX, goalY, goalArea, capabilities, ioBest, ioHaveBest);
        }
    }

    // From the teleport's dest, route to the goal: a closing walk when it
    // landed in the goal area, otherwise teleport-dest area -> goal area over
    // baked crossings plus the closing walk. Both are exactly what
    // assembleAreaRoute produces (a same-area area search yields a single
    // trivial step and just the closing walk). The closing route uses baked
    // crossings only — the one global teleport is already emitted, so the
    // sub-route must not teleport again (which would double-count and tangle
    // the cost accounting).
    bool PathAssembler::tryTeleportLanding(int32_t startX, int32_t startY, int32_t startPlane,
                                           const FrontierSeed &seed,
                                           int32_t goalX, int32_t goalY, int32_t goalArea,
                                           const CapabilitySnapshot *capabilities, Plan &outAlt)
    {
        outAlt.steps.clear();
        outAlt.cost = 0.0f;
        int32_t cx = startX;
        int32_t cy = startY;
        int32_t cp = startPlane;
        emitGlobalTeleport(seed.transitionIndex, cx, cy, cp, outAlt);
        const std::span<const FrontierSeed> noSeeds{};
        return assembleAreaRoute(cx, cy, cp, seed.destArea, goalX, goalY, goalArea,
                                 capabilities, noSeeds, outAlt);
    }

    // AreaSearch picks the first hop out of seed.destArea on area-cost alone,
    // so a single direct edge to goalArea wins over a two-edge chain even when
    // the chain's intra-area walk is an order of magnitude shorter — the
    // dungeon-cape resource-dungeon landing is exactly that pattern. For each
    // baked transition whose dest tile is near the goal and whose fromArea is
    // the seed's landing area, build and price the chained route and keep it
    // if it beats the incumbent. nearGoalEdgeScratch was filtered against goal
    // plane + global flag at scan time, so the body is just an area-match
    // filter, a bound check, and the realised-cost build.
    void PathAssembler::tryNearGoalExits(int32_t startX, int32_t startY, int32_t startPlane,
                                         const FrontierSeed &seed,
                                         int32_t goalX, int32_t goalY, int32_t goalArea,
                                         const CapabilitySnapshot *capabilities,
                                         Plan &ioBest, bool &ioHaveBest)
    {
        const std::span<const format::AreaEdgeRecord> edges = artifact->areaEdges();
        for (const NearGoalEdge &nge : nearGoalEdgeScratch)
        {
            if (nge.fromArea != seed.destArea)
            {
                continue;
            }
            // Lower bound on the realised chained plan: teleport cost
            // (seed.cost) + edge cost + admissible closing walk (the octile
            // distance from the edge's destination tile to the goal, baked
            // into closingBound at scan time). Skip the full
            // tryTeleportNearGoalExit — which runs a fresh assembleAreaRoute
            // internally — when this bound already can't beat the best plan.
            const float pairBound = seed.cost + nge.closingBound;
            if (ioHaveBest && pairBound >= ioBest.cost)
            {
                continue;
            }
            Plan alt;
            if (!tryTeleportNearGoalExit(startX, startY, startPlane, seed, edges[nge.edgeIdx],
                                         goalX, goalY, goalArea, capabilities, alt))
            {
                continue;
            }
            keepIfCheaper(alt, ioBest, ioHaveBest);
        }
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
        int32_t cursorX = startX;
        int32_t cursorY = startY;
        int32_t cursorPlane = startPlane;
        emitLeadingTransition(cursorX, cursorY, cursorPlane, outPlan);
        for (std::size_t i = 1; i < areaPath.steps.size(); ++i)
        {
            if (!appendTransitionHop(i, cursorX, cursorY, cursorPlane, outPlan))
            {
                return false;
            }
        }
        return appendWalkSegment(cursorX, cursorY, goalX, goalY, cursorPlane, goalArea, outPlan);
    }
}
