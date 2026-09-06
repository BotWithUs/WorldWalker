#include "c_api/worldwalker_c.h"
#include "cli/Bench.h"
#include "cli/CrossCheck.h"
#include "cli/DoorPaths.h"
#include "cli/ExecutorTests.h"
#include "cli/HarnessPicks.h"
#include "cli/InstanceTests.h"
#include "cli/PathExport.h"
#include "cli/ScriptedPaths.h"
#include "cli/WallShapeTests.h"
#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "runtime/AreaSearch.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/ContextPool.h"
#include "runtime/PathAssembler.h"
#include "runtime/RuntimeTeleports.h"
#include "runtime/SearchContext.h"
#include "runtime/TeleportPolicy.h"
#include "runtime/TileSearch.h"
#include "runtime/WorldView.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <vector>

// wwcli — WorldWalker dev harness (queries + benchmarks).
// `wwcli <artifact.wwa>` loads the artifact through the runtime reader and prints
// a per-section summary, decompressing the first collision square and area grid
// to prove the on-demand path works end-to-end. Query/benchmark commands land in
// later Phase 3/6 steps.
namespace
{
    void dumpCollision(const ww::format::ArtifactReader &reader)
    {
        const auto squares = reader.collisionSquares();
        std::printf("  collision: %zu squares", squares.size());
        if (!squares.empty())
        {
            std::vector<uint32_t> words;
            reader.decompressSquare(squares[0].squareX, squares[0].squareY, words);
            std::size_t nonZero = 0;
            for (uint32_t w : words)
            {
                nonZero += (w != 0) ? 1 : 0;
            }
            std::printf(" | square[0] (%u,%u) -> %zu words, %zu non-open",
                        squares[0].squareX, squares[0].squareY, words.size(), nonZero);
        }
        std::printf("\n");
    }

    void dumpAbstraction(const ww::format::ArtifactReader &reader)
    {
        const auto grids = reader.areaGrids();
        std::printf("  abstraction: %zu areas, %zu edges, %zu grids", reader.areaNodes().size(),
                    reader.areaEdges().size(), grids.size());
        if (!grids.empty())
        {
            std::vector<int32_t> ids;
            reader.decompressGrid(grids[0].squareX, grids[0].squareY, grids[0].plane, ids);
            std::size_t walkable = 0;
            for (int32_t id : ids)
            {
                walkable += (id >= 0) ? 1 : 0;
            }
            std::printf(" | grid[0] (%u,%u,p%u) -> %zu tiles, %zu in an area",
                        grids[0].squareX, grids[0].squareY, grids[0].plane, ids.size(), walkable);
        }
        std::printf("\n");
    }

    // Re-read every clip word of one square through the by-coordinate runtime
    // lookup and confirm it matches the directly-decompressed ground truth, so a
    // coordinate-math or cache bug surfaces here rather than in the planner.
    std::size_t crossCheckSquare(ww::runtime::WorldView &view,
                                 const ww::format::CollisionSquareEntry &sq,
                                 const std::vector<uint32_t> &words)
    {
        const int baseX = static_cast<int>(sq.squareX) * ww::format::kClipSize;
        const int baseY = static_cast<int>(sq.squareY) * ww::format::kClipSize;
        std::size_t mismatches = 0;
        for (std::size_t idx = 0; idx < words.size(); ++idx)
        {
            const int plane = static_cast<int>(idx / (ww::format::kClipSize * ww::format::kClipSize));
            const int lx = static_cast<int>((idx / ww::format::kClipSize) % ww::format::kClipSize);
            const int ly = static_cast<int>(idx % ww::format::kClipSize);
            if (view.clipAt(baseX + lx, baseY + ly, plane) != words[idx])
            {
                ++mismatches;
            }
        }
        return mismatches;
    }

    // Returns the number of clip-word mismatches (0 when the runtime view agrees
    // with the raw decompressed square).
    std::size_t dumpRuntimeLookup(const ww::format::ArtifactReader &reader)
    {
        ww::runtime::WorldView view(reader);
        const auto squares = reader.collisionSquares();
        if (squares.empty())
        {
            std::printf("  runtime: no collision squares to cross-check\n");
            return 0;
        }
        const ww::format::CollisionSquareEntry &sq = squares[0];
        std::vector<uint32_t> words;
        reader.decompressSquare(sq.squareX, sq.squareY, words);
        const std::size_t mismatches = crossCheckSquare(view, sq, words);

        const int baseX = static_cast<int>(sq.squareX) * ww::format::kClipSize;
        const int baseY = static_cast<int>(sq.squareY) * ww::format::kClipSize;
        std::printf("  runtime: square[0] (%u,%u) clip cross-check %zu tiles, %zu mismatches\n",
                    sq.squareX, sq.squareY, words.size(), mismatches);
        std::printf("  runtime: off-world clip=0x%08x | tile (%d,%d,p0) standable=%d area=%d\n",
                    view.clipAt(-1, baseY, 0), baseX, baseY,
                    view.isStandable(baseX, baseY, 0) ? 1 : 0, view.areaAt(baseX, baseY, 0));
        return mismatches;
    }

    // Confirm every hop's recorded AreaEdge actually connects the previous area
    // to this one, so a reconstruction or CSR-indexing bug surfaces here. Returns
    // the number of broken links (0 for a valid route).
    std::size_t checkContiguity(const ww::format::ArtifactReader &reader,
                                const ww::runtime::AreaPath &path)
    {
        const auto edges = reader.areaEdges();
        std::size_t broken = 0;
        for (std::size_t i = 1; i < path.steps.size(); ++i)
        {
            const int32_t e = path.steps[i].viaEdge;
            const bool ok = e >= 0 && static_cast<std::size_t>(e) < edges.size()
                            && edges[e].fromArea == path.steps[i - 1].area
                            && edges[e].toArea == path.steps[i].area;
            broken += ok ? 0u : 1u;
        }
        return broken;
    }

    // First area reachable in one edge from midArea that is neither midArea nor
    // `avoid`, or -1 if none. Excluding `avoid` (the start) skips the common
    // paired back-edge, so the harness poses a genuine multi-hop query.
    int32_t reachableTwoHop(const ww::format::ArtifactReader &reader, int32_t midArea, int32_t avoid)
    {
        for (const ww::format::AreaEdgeRecord &edge : reader.areaEdges())
        {
            if (edge.fromArea == midArea && edge.toArea != midArea && edge.toArea != avoid)
            {
                return edge.toArea;
            }
        }
        return -1;
    }

    // Returns the number of failed checks: broken links on a found route, or 1
    // when a route the caller says must exist (shouldReach) did not.
    std::size_t runQuery(ww::runtime::AreaSearch &search, const ww::format::ArtifactReader &reader,
                         int32_t start, int32_t goal, const char *label, bool shouldReach)
    {
        ww::runtime::AreaPath path;
        if (!search.findPath(start, goal, path))
        {
            std::printf("  search: %-6s %d->%d unreachable\n", label, start, goal);
            return shouldReach ? 1u : 0u;
        }
        const std::size_t broken = checkContiguity(reader, path);
        std::printf("  search: %-6s %d->%d ok, %zu areas, cost=%.1f, %zu broken links\n",
                    label, start, goal, path.steps.size(),
                    static_cast<double>(path.cost), broken);
        return broken;
    }

    // Drive the area-graph A* end to end on the runtime lookup layer: resolve a
    // tile to its area through WorldView, then run a self-query, a single-edge
    // hop, and a whole-graph endpoint query, checking each route's contiguity.
    std::size_t dumpAreaSearch(const ww::format::ArtifactReader &reader)
    {
        const auto nodes = reader.areaNodes();
        if (nodes.empty())
        {
            std::printf("  search: no area graph to search\n");
            return 0;
        }
        ww::runtime::WorldView view(reader);
        ww::runtime::AreaSearch search(reader);

        const ww::format::AreaNodeRecord &n0 = nodes[0];
        std::printf("  search: area[0] centroid (%d,%d,p%u) -> area %d via WorldView\n",
                    n0.centroidX, n0.centroidY, n0.plane,
                    view.areaAt(n0.centroidX, n0.centroidY, n0.plane));

        std::size_t failures = runQuery(search, reader, 0, 0, "self", true);
        const auto edges = reader.areaEdges();
        if (!edges.empty())
        {
            failures += runQuery(search, reader, edges[0].fromArea, edges[0].toArea, "edge0", true);
            const int32_t twoHop = reachableTwoHop(reader, edges[0].toArea, edges[0].fromArea);
            if (twoHop >= 0)
            {
                failures += runQuery(search, reader, edges[0].fromArea, twoHop, "2hop", true);
            }
        }
        failures += runQuery(search, reader, 0, static_cast<int32_t>(nodes.size()) - 1, "ends", false);
        return failures;
    }

    // Confirm every tile of a refined path is standable, in-constraint, and one
    // legal-distance step from its predecessor, so a reconstruction, area-bound,
    // or corner-cut bug surfaces here. Returns the number of broken tiles.
    std::size_t checkTilePath(ww::runtime::WorldView &view, const ww::runtime::TilePath &path,
                              int32_t plane, int32_t areaConstraint)
    {
        std::size_t broken = 0;
        for (std::size_t i = 0; i < path.tiles.size(); ++i)
        {
            const ww::runtime::TilePoint &t = path.tiles[i];
            const bool stand = view.isStandable(t.x, t.y, plane);
            const bool inArea = areaConstraint < 0 || view.areaAt(t.x, t.y, plane) == areaConstraint;
            bool adjacent = true;
            if (i > 0)
            {
                const int32_t dx = std::abs(t.x - path.tiles[i - 1].x);
                const int32_t dy = std::abs(t.y - path.tiles[i - 1].y);
                adjacent = dx <= 1 && dy <= 1 && (dx + dy) > 0;
            }
            broken += (stand && inArea && adjacent) ? 0u : 1u;
        }
        return broken;
    }

    // Returns the number of failed checks: broken tiles on a found path, or 1
    // when a path the caller says must exist (shouldReach) did not.
    std::size_t runTileQuery(ww::runtime::TileSearch &search, ww::runtime::WorldView &view,
                             int32_t sx, int32_t sy, int32_t gx, int32_t gy, int32_t plane,
                             int32_t areaConstraint, const char *label, bool shouldReach)
    {
        ww::runtime::TilePath path;
        if (!search.findPath(sx, sy, gx, gy, plane, areaConstraint, path))
        {
            std::printf("  tile:   %-6s (%d,%d)->(%d,%d) unreachable\n", label, sx, sy, gx, gy);
            return shouldReach ? 1u : 0u;
        }
        const std::size_t broken = checkTilePath(view, path, plane, areaConstraint);
        std::printf("  tile:   %-6s (%d,%d)->(%d,%d) ok, %zu tiles, cost=%.1f, %zu broken\n",
                    label, sx, sy, gx, gy, path.tiles.size(),
                    static_cast<double>(path.cost), broken);
        return broken;
    }

    // Drive the tile-level refinement A* end to end on the runtime lookup layer:
    // resolve area[0]'s centroid, then run a self-query, a within-area route to
    // the farthest reachable in-area tile (and the same goal unconstrained), and
    // an off-map goal that must come back unreachable.
    std::size_t dumpTileSearch(const ww::format::ArtifactReader &reader)
    {
        const auto nodes = reader.areaNodes();
        if (nodes.empty())
        {
            std::printf("  tile:   no area graph to refine\n");
            return 0;
        }
        ww::runtime::WorldView view(reader);
        ww::runtime::TileSearch search(view);

        const ww::format::AreaNodeRecord &n0 = nodes[0];
        const int32_t plane = static_cast<int32_t>(n0.plane);
        const int32_t area0 = view.areaAt(n0.centroidX, n0.centroidY, plane);

        std::size_t failures = runTileQuery(search, view, n0.centroidX, n0.centroidY,
                                            n0.centroidX, n0.centroidY, plane, area0, "self", true);
        const ww::runtime::TilePoint goal =
            ww::cli::farthestInArea(view, n0.centroidX, n0.centroidY, plane, area0, 24);
        failures += runTileQuery(search, view, n0.centroidX, n0.centroidY, goal.x, goal.y, plane,
                                 area0, "inarea", true);
        failures += runTileQuery(search, view, n0.centroidX, n0.centroidY, goal.x, goal.y, plane,
                                 -1, "free", true);
        failures += runTileQuery(search, view, n0.centroidX, n0.centroidY, n0.centroidX + 4096,
                                 n0.centroidY, plane, area0, "offmap", false);
        return failures;
    }

    // PathAssembler::resolveInteractTile's search radius. A local-origin
    // Transition step's target is the tile the player stands on to click the
    // loc, resolved within this many tiles (Chebyshev) of the record's origin,
    // so a step farther out than this did not come from that record.
    constexpr int32_t kInteractReach = 2;

    // Is this Transition step's target consistent with the record it names? For
    // a local origin, the interact tile must sit on the origin plane within
    // kInteractReach of the origin loc. A global teleport is cast in place, so
    // its step sits at the cursor and neither bound applies.
    bool transitionOriginOk(const ww::format::TransitionRecord &tx, const ww::runtime::Step &s)
    {
        if ((tx.flags & ww::format::kTransitionFlagGlobalOrigin) != 0u)
        {
            return true;
        }
        if (static_cast<int>(s.plane) != static_cast<int>(tx.originPlane))
        {
            return false;
        }
        return std::max(std::abs(s.targetX - tx.originX), std::abs(s.targetY - tx.originY))
               <= kInteractReach;
    }

    // Sanity-check an assembled Plan. Three properties, each one a bug the
    // assembler can produce:
    //   * every step lands on a standable tile;
    //   * a Transition step names a real TransitionRecord and stands where that
    //     record could actually be interacted with (see transitionOriginOk) —
    //     the "origin is reachable from the prior step" half that used to be
    //     claimed in this comment and never checked;
    //   * the plane changes only ACROSS a Transition. Walking cannot cross
    //     planes, so two consecutive steps on different planes with no
    //     Transition between them describe a move nothing can execute.
    // Returns the number of broken steps (0 for a valid plan).
    std::size_t checkPlan(ww::runtime::WorldView &view, const ww::format::ArtifactReader &reader,
                          const ww::runtime::Plan &plan)
    {
        std::size_t broken = 0;
        const auto transitions = reader.transitions();
        const ww::runtime::Step *previous = nullptr;
        for (const ww::runtime::Step &s : plan.steps)
        {
            bool ok = view.isStandable(s.targetX, s.targetY, static_cast<int>(s.plane));
            if (s.kind == ww::runtime::StepKind::Transition)
            {
                ok = ok && s.transitionIndex < transitions.size()
                     && transitionOriginOk(transitions[s.transitionIndex], s);
            }
            if (previous != nullptr && previous->plane != s.plane
                && previous->kind != ww::runtime::StepKind::Transition)
            {
                ok = false;
            }
            broken += ok ? 0u : 1u;
            previous = &s;
        }
        return broken;
    }

    // Returns the number of failed checks: broken steps on an assembled plan, or
    // 1 when a plan the caller says must exist (shouldReach) did not.
    std::size_t runPlanQuery(ww::runtime::PathAssembler &assembler, ww::runtime::WorldView &view,
                             const ww::format::ArtifactReader &reader, int32_t sx, int32_t sy,
                             int32_t sp, int32_t gx, int32_t gy, int32_t gp, const char *label,
                             bool shouldReach)
    {
        ww::runtime::Plan plan;
        if (!assembler.assemble(sx, sy, sp, gx, gy, gp, plan))
        {
            std::printf("  plan:   %-6s (%d,%d,p%d)->(%d,%d,p%d) unreachable\n", label, sx, sy, sp,
                        gx, gy, gp);
            return shouldReach ? 1u : 0u;
        }
        std::size_t walks = 0;
        std::size_t hops = 0;
        for (const ww::runtime::Step &s : plan.steps)
        {
            walks += s.kind == ww::runtime::StepKind::Walk ? 1u : 0u;
            hops += s.kind == ww::runtime::StepKind::Transition ? 1u : 0u;
        }
        const std::size_t broken = checkPlan(view, reader, plan);
        std::printf("  plan:   %-6s (%d,%d,p%d)->(%d,%d,p%d) ok, %zu steps (%zu walk + %zu hop),"
                    " cost=%.1f, %zu broken\n",
                    label, sx, sy, sp, gx, gy, gp, plan.steps.size(), walks, hops,
                    static_cast<double>(plan.cost), broken);
        return broken;
    }

    // Same as runPlanQuery but routes through the capability-gated assemble
    // overload (non-null snapshot), so it reproduces exactly what the executor's
    // planFrom does — requirement filtering against a real snapshot rather than
    // the "admit everything" null-caps path. Used by the teleports probe to
    // prove that an empty snapshot rejects every gated teleport while one with
    // the unlock varbits set admits them.
    void runPlanQueryCaps(ww::runtime::PathAssembler &assembler,
                          const ww::format::ArtifactReader &reader,
                          const ww::runtime::CapabilitySnapshot *caps,
                          int32_t sx, int32_t sy, int32_t sp,
                          int32_t gx, int32_t gy, int32_t gp, const char *label)
    {
        ww::runtime::Plan plan;
        if (!assembler.assemble(sx, sy, sp, gx, gy, gp, caps, plan))
        {
            std::printf("  plan:   %-10s (%d,%d,p%d)->(%d,%d,p%d) unreachable\n", label, sx, sy, sp,
                        gx, gy, gp);
            return;
        }
        std::size_t walks = 0;
        std::size_t hops = 0;
        for (const ww::runtime::Step &s : plan.steps)
        {
            walks += s.kind == ww::runtime::StepKind::Walk ? 1u : 0u;
            hops += s.kind == ww::runtime::StepKind::Transition ? 1u : 0u;
        }
        std::printf("  plan:   %-10s (%d,%d,p%d)->(%d,%d,p%d) ok, %zu steps (%zu walk + %zu hop),"
                    " cost=%.1f\n",
                    label, sx, sy, sp, gx, gy, gp, plan.steps.size(), walks, hops,
                    static_cast<double>(plan.cost));
        const auto txs = reader.transitions();
        for (const ww::runtime::Step &s : plan.steps)
        {
            if (s.kind != ww::runtime::StepKind::Transition)
            {
                continue;
            }
            if (s.transitionIndex < txs.size())
            {
                const ww::format::TransitionRecord &tx = txs[s.transitionIndex];
                std::printf("            hop tx%u kind=%u from=(%d,%d,p%d) dest=(%d,%d,p%u) cost=%.1f\n",
                            s.transitionIndex, tx.kind, s.targetX, s.targetY,
                            static_cast<int>(s.plane), tx.destX, tx.destY, tx.destPlane,
                            static_cast<double>(tx.cost));
            }
        }
    }

    // teleports — load the scripter-editable global teleports (spell + lodestone)
    // from a dataset dir onto the artifact, then report the appended set and
    // (optionally) plan start->goal so a route via a teleport is observable
    // without the game. Exercises the same loadGlobalTeleportsInto + frontier
    // seeding the runtime uses.
    int runTeleports(int argc, char **argv)
    {
        try
        {
            ww::format::ArtifactReader reader(argv[0]);
            const std::size_t added = ww::runtime::loadGlobalTeleportsInto(reader, argv[1]);
            const auto txs = reader.transitions();
            std::size_t globals = 0;
            for (const ww::format::TransitionRecord &tx : txs)
            {
                globals += (tx.flags & ww::format::kTransitionFlagGlobalOrigin) != 0u ? 1u : 0u;
            }
            std::printf("teleports: appended %zu from %s; artifact now %zu transitions, "
                        "%zu global-origin\n",
                        added, argv[1], txs.size(), globals);

            ww::runtime::WorldView view(reader);
            std::size_t seedable = 0;
            for (std::size_t i = 0; i < txs.size(); ++i)
            {
                const ww::format::TransitionRecord &tx = txs[i];
                if ((tx.flags & ww::format::kTransitionFlagGlobalOrigin) == 0u)
                {
                    continue;
                }
                const int32_t destArea =
                    view.areaAt(tx.destX, tx.destY, static_cast<int32_t>(tx.destPlane));
                seedable += destArea >= 0 ? 1u : 0u;
                std::printf("  tx%zu kind=%u dest=(%d,%d,p%u) area=%d cost=%.1f reqs=%u chain=%u%s\n",
                            i, tx.kind, tx.destX, tx.destY, tx.destPlane, destArea,
                            static_cast<double>(tx.cost), tx.requirementCount, tx.chainCount,
                            destArea < 0 ? "  [OFF-AREA: never seeded]" : "");
            }
            std::printf("teleports: %zu/%zu global dests land in a valid area (seedable)\n",
                        seedable, globals);

            if (argc >= 8)
            {
                ww::runtime::AreaSearch areaSearch(reader);
                ww::runtime::TileSearch tileSearch(view);
                ww::runtime::PathAssembler assembler(reader, view, areaSearch, tileSearch);
                const int sx = std::atoi(argv[2]), sy = std::atoi(argv[3]), sp = std::atoi(argv[4]);
                const int gx = std::atoi(argv[5]), gy = std::atoi(argv[6]), gp = std::atoi(argv[7]);
                // Baseline: null caps (admit everything) — the unfiltered path.
                // Informational probe: a missing route is reported, not a failure.
                static_cast<void>(runPlanQuery(assembler, view, reader, sx, sy, sp, gx, gy, gp,
                                               "tele", false));

                // Reproduce the executor's planFrom requirement gating. Collect
                // the distinct varbit ids any requirement references (lodestone
                // unlocks live here), then plan twice with a non-null snapshot:
                // once empty (every varbit reads 0 → gated teleports rejected,
                // the live bug) and once with all those varbits set to 1 (the
                // post-fix executor reads the player's true unlock state via the
                // readVarbits callback → gated teleports admitted).
                std::vector<int32_t> varbitIds;
                std::vector<int32_t> itemIds;
                for (const ww::format::RequirementRecord &r : reader.requirements())
                {
                    const auto kind = static_cast<ww::data::RequirementKind>(r.kind);
                    if (kind == ww::data::RequirementKind::Varbit
                        && std::find(varbitIds.begin(), varbitIds.end(), r.id) == varbitIds.end())
                    {
                        varbitIds.push_back(r.id);
                    }
                    else if (kind == ww::data::RequirementKind::Item
                             && std::find(itemIds.begin(), itemIds.end(), r.id) == itemIds.end())
                    {
                        itemIds.push_back(r.id);
                    }
                }
                ww::runtime::CapabilitySnapshot empty;
                runPlanQueryCaps(assembler, reader, &empty, sx, sy, sp, gx, gy, gp, "caps:empty");
                // Full-unlock snapshot: every gate satisfied (unlock varbits set,
                // every required item held) — mirrors what the executor's
                // planFrom builds for a fully-equipped player, so item-gated
                // teleports (dungeoneering cape, jewellery) are admitted.
                ww::runtime::CapabilitySnapshot unlocked;
                for (int32_t id : varbitIds)
                {
                    unlocked.setVarbit(id, 1);
                }
                for (int32_t id : itemIds)
                {
                    unlocked.setItemCount(id, 1000);
                }
                runPlanQueryCaps(assembler, reader, &unlocked, sx, sy, sp, gx, gy, gp, "caps:unlock");
            }
            return 0;
        }
        catch (const std::exception &e)
        {
            std::printf("teleports: failed: %s\n", e.what());
            return 1;
        }
    }

    // Edge filter for the capability exercise: only transitions that actually
    // carry a requirement run have anything for the gate to reject.
    bool acceptRequirementBearing(const ww::format::TransitionRecord &tx)
    {
        return tx.requirementCount != 0u;
    }

    // CapabilitySnapshot::meets() self-check against synthetic RequirementRecords
    // — exercises the predicate even when no req-bearing area edge is present in
    // the artifact under test. Returns true on the expected eight outcomes.
    bool capabilityPredicateSelfCheck()
    {
        ww::runtime::CapabilitySnapshot s;
        s.setSkillLevel(1, 70);
        s.setItemCount(2, 5);
        s.setVarbit(3, 4);
        s.setVarp(4, 9);
        const auto make = [](uint8_t kind, int32_t id, int32_t amount)
        {
            ww::format::RequirementRecord r{};
            r.kind = kind;
            r.id = id;
            r.amount = amount;
            return r;
        };
        return s.meets(make(0, 1, 60)) && !s.meets(make(0, 1, 80))
            && s.meets(make(1, 2, 5))  && !s.meets(make(1, 2, 6))
            && s.meets(make(2, 3, 4))  && !s.meets(make(2, 3, 5))
            && s.meets(make(3, 4, 9))  && !s.meets(make(3, 4, 10));
    }

    // Phase 3d-3 exercise: validate the predicate against synthetic records,
    // count how many transitions and how many area edges carry requirements
    // (most reqs live on global-origin transitions, which are NOT area edges —
    // they're the 3d-4 frontier-seeding case), and when a req-bearing area edge
    // exists, run AreaSearch unfiltered (nullptr snapshot) vs gated by an empty
    // CapabilitySnapshot so the gate must re-route or come back blocked.
    std::size_t dumpCapabilityFilter(const ww::format::ArtifactReader &reader,
                              ww::runtime::WorldView &view, ww::runtime::AreaSearch &areaSearch)
    {
        const bool isPredicateOk = capabilityPredicateSelfCheck();
        std::size_t failures = isPredicateOk ? 0u : 1u;
        std::printf("  caps:   predicate self-check: %s\n", isPredicateOk ? "ok" : "FAIL");

        const auto edges = reader.areaEdges();
        const auto txs = reader.transitions();
        std::size_t reqTxs = 0;
        std::size_t reqGlobalTxs = 0;
        for (const ww::format::TransitionRecord &tx : txs)
        {
            if (tx.requirementCount == 0u)
            {
                continue;
            }
            ++reqTxs;
            if ((tx.flags & ww::format::kTransitionFlagGlobalOrigin) != 0u)
            {
                ++reqGlobalTxs;
            }
        }
        std::size_t reqEdges = 0;
        for (const ww::format::AreaEdgeRecord &e : edges)
        {
            if (e.transitionIndex < txs.size() && txs[e.transitionIndex].requirementCount > 0u)
            {
                ++reqEdges;
            }
        }
        std::printf("  caps:   transitions w/ reqs: %zu (%zu global, %zu local); area edges w/ reqs: %zu / %zu\n",
                    reqTxs, reqGlobalTxs, reqTxs - reqGlobalTxs, reqEdges, edges.size());

        // Isolate the filter to a single known req-bearing edge instead of
        // relying on the route search to bump into one.
        ww::cli::CrossAreaPick reqPick{};
        if (!ww::cli::pickCrossAreaPair(reader, view, acceptRequirementBearing, reqPick))
        {
            std::printf("  caps:   no traversable req-bearing area edge to exercise filter end-to-end\n");
            return failures;
        }
        const std::size_t reqEdgeIdx = reqPick.edgeIndex;
        const ww::format::AreaEdgeRecord &re = edges[reqEdgeIdx];
        const ww::format::TransitionRecord &rtx = txs[re.transitionIndex];
        std::printf("  caps:   req-edge%zu area%d->area%d tx%u reqs=%u\n", reqEdgeIdx, re.fromArea,
                    re.toArea, re.transitionIndex, rtx.requirementCount);

        ww::runtime::AreaPath openPath;
        ww::runtime::AreaPath gatedPath;
        const bool openOk = areaSearch.findPath(re.fromArea, re.toArea, openPath);
        const ww::runtime::CapabilitySnapshot empty;
        const bool gatedOk = areaSearch.findPath(re.fromArea, re.toArea, &empty, gatedPath);
        failures += openOk ? 0u : 1u;  // the unfiltered route must exist; gating may block it
        std::printf("  caps:   open=%s(%zu steps, cost=%.1f) gated=%s", openOk ? "ok" : "X",
                    openPath.steps.size(), static_cast<double>(openPath.cost),
                    gatedOk ? "ok" : "blocked");
        if (gatedOk)
        {
            std::printf("(%zu steps, cost=%.1f)\n", gatedPath.steps.size(),
                        static_cast<double>(gatedPath.cost));
        }
        else
        {
            std::printf("\n");
        }
        return failures;
    }

    // isTeleportAllowed self-check against synthetic wilderness + no-tele boxes
    // — the predicate is exercised regardless of what the artifact under test
    // bakes. The wilderness box rises one level per 8 tiles starting at y=3200
    // (so y=3200..3207 is level 1, y=3360..3367 is level 21); the no-tele box
    // is a small unconditional block. Returns true on all eight expected
    // outcomes.
    bool teleportPolicySelfCheck()
    {
        ww::format::WildernessRegion w{};
        w.minX = 3000;
        w.minY = 3200;
        w.maxX = 3100;
        w.maxY = 3700;
        w.baseY = 3200;
        w.baseLevel = 1;
        w.stepY = 8;
        w.planeMin = 0;
        w.planeMax = 0;
        ww::format::NoTeleZone z{};
        z.minX = 3500;
        z.minY = 3500;
        z.maxX = 3520;
        z.maxY = 3520;
        z.planeMin = 0;
        z.planeMax = 0;
        const std::vector<ww::format::WildernessRegion> wild = {w};
        const std::vector<ww::format::NoTeleZone> noTele = {z};
        using ww::runtime::isTeleportAllowed;
        return isTeleportAllowed(noTele, wild, 20, 1000, 1000, 0)     // outside any box
            && isTeleportAllowed(noTele, wild, 20, 3050, 3200, 0)     // wild level 1
            && isTeleportAllowed(noTele, wild, 20, 3050, 3352, 0)     // wild level 20
            && !isTeleportAllowed(noTele, wild, 20, 3050, 3360, 0)    // wild level 21 > cutoff
            && isTeleportAllowed(noTele, wild, 30, 3050, 3360, 0)     // raised cutoff
            && !isTeleportAllowed(noTele, wild, 20, 3510, 3510, 0)    // inside no-tele
            && !isTeleportAllowed(noTele, wild, 20, 1000, 1000, 4)    // bad plane (high)
            && !isTeleportAllowed(noTele, wild, 20, 1000, 1000, -1);  // bad plane (low)
    }

    // First global the permissive snapshot accepts whose destArea is NOT
    // reachable from startArea by the unfiltered area-graph search at any cost
    // less than the teleport itself — i.e. seeding will win. Iterates because
    // the artifact mixes teleports of widely varying cost; some land in
    // walking-reachable areas and would be dominated, others land in components
    // that can only be entered via teleport. Returns false when no global both
    // satisfies the snapshot and dominates walking; then the harness reports the
    // pool counts and skips the e2e exercise.
    bool pickSeedDominatingGlobal(const ww::format::ArtifactReader &reader,
                                  ww::runtime::WorldView &view,
                                  ww::runtime::AreaSearch &areaSearch,
                                  int32_t startArea,
                                  const ww::runtime::CapabilitySnapshot &snapshot,
                                  uint32_t &outTxIndex, int32_t &outDestArea)
    {
        const auto txs = reader.transitions();
        const auto reqs = reader.requirements();
        ww::runtime::AreaPath walkProbe;
        for (uint32_t i = 0; i < txs.size(); ++i)
        {
            const ww::format::TransitionRecord &tx = txs[i];
            if ((tx.flags & ww::format::kTransitionFlagGlobalOrigin) == 0u)
            {
                continue;
            }
            const uint64_t end = static_cast<uint64_t>(tx.requirementStart) + tx.requirementCount;
            if (end > reqs.size())
            {
                continue;
            }
            if (!ww::runtime::meetsRequirements(&snapshot,
                                                reqs.subspan(tx.requirementStart,
                                                             tx.requirementCount)))
            {
                continue;
            }
            const int32_t destArea =
                view.areaAt(tx.destX, tx.destY, static_cast<int32_t>(tx.destPlane));
            if (destArea < 0)
            {
                continue;
            }
            const bool walkOk = areaSearch.findPath(startArea, destArea, walkProbe);
            if (walkOk && walkProbe.cost <= tx.cost)
            {
                continue;
            }
            outTxIndex = i;
            outDestArea = destArea;
            return true;
        }
        return false;
    }

    // Count global-origin transitions and how many are accepted by each of an
    // empty snapshot and the permissive snapshot built from the artifact.
    void countGlobalAcceptance(const ww::format::ArtifactReader &reader,
                               const ww::runtime::CapabilitySnapshot &permissive,
                               std::size_t &outTotal, std::size_t &outEmpty,
                               std::size_t &outPermissive)
    {
        const auto txs = reader.transitions();
        const auto reqs = reader.requirements();
        const ww::runtime::CapabilitySnapshot empty;
        outTotal = 0;
        outEmpty = 0;
        outPermissive = 0;
        for (const ww::format::TransitionRecord &tx : txs)
        {
            if ((tx.flags & ww::format::kTransitionFlagGlobalOrigin) == 0u)
            {
                continue;
            }
            ++outTotal;
            const uint64_t end = static_cast<uint64_t>(tx.requirementStart) + tx.requirementCount;
            if (end > reqs.size())
            {
                continue;
            }
            const auto run = reqs.subspan(tx.requirementStart, tx.requirementCount);
            if (ww::runtime::meetsRequirements(&empty, run))
            {
                ++outEmpty;
            }
            if (ww::runtime::meetsRequirements(&permissive, run))
            {
                ++outPermissive;
            }
        }
    }

    // Phase 3d-4 exercise: policy self-check, global-pool size and capability
    // acceptance breakdown, and (when an eligible global exists) an end-to-end
    // plan whose goal is the chosen teleport's destination — verifying that the
    // plan's first step is the seeded Transition.
    std::size_t dumpTeleportSeeding(const ww::format::ArtifactReader &reader,
                             ww::runtime::WorldView &view,
                             ww::runtime::AreaSearch &areaSearch,
                             ww::runtime::PathAssembler &assembler)
    {
        const bool isPolicyOk = teleportPolicySelfCheck();
        std::size_t failures = isPolicyOk ? 0u : 1u;
        std::printf("  tele:   policy self-check: %s\n", isPolicyOk ? "ok" : "FAIL");

        ww::runtime::CapabilitySnapshot permissive;
        ww::runtime::applyPermissiveRequirements(reader.requirements(), permissive);
        std::size_t globalsTotal = 0;
        std::size_t globalsEmpty = 0;
        std::size_t globalsPermissive = 0;
        countGlobalAcceptance(reader, permissive, globalsTotal, globalsEmpty, globalsPermissive);
        std::printf("  tele:   globals %zu (empty-snapshot accepts %zu, permissive %zu)\n",
                    globalsTotal, globalsEmpty, globalsPermissive);

        const auto nodes = reader.areaNodes();
        if (nodes.empty())
        {
            return failures;
        }
        const ww::format::AreaNodeRecord &startNode = nodes[0];
        const int32_t startPlane = static_cast<int32_t>(startNode.plane);
        const int32_t startArea =
            view.areaAt(startNode.centroidX, startNode.centroidY, startPlane);
        uint32_t txIndex = 0;
        int32_t destArea = -1;
        if (startArea < 0
            || !pickSeedDominatingGlobal(reader, view, areaSearch, startArea, permissive,
                                         txIndex, destArea))
        {
            std::printf("  tele:   no global dominates walking from start area for e2e\n");
            return failures;
        }
        const ww::format::TransitionRecord &chosenTx = reader.transitions()[txIndex];
        const int32_t goalX = chosenTx.destX;
        const int32_t goalY = chosenTx.destY;
        const int32_t goalPlane = static_cast<int32_t>(chosenTx.destPlane);
        const bool startAllowed =
            ww::runtime::isTeleportAllowed(reader, startNode.centroidX, startNode.centroidY,
                                           startPlane);
        std::printf("  tele:   pick tx%u cost=%.1f dest=(%d,%d,p%d) destArea=%d;"
                    " start=(%d,%d,p%d) %s\n",
                    txIndex, static_cast<double>(chosenTx.cost), goalX, goalY, goalPlane, destArea,
                    startNode.centroidX, startNode.centroidY, startPlane,
                    startAllowed ? "tele-allowed" : "tele-blocked");
        ww::runtime::Plan plan;
        const bool ok = assembler.assemble(startNode.centroidX, startNode.centroidY, startPlane,
                                           goalX, goalY, goalPlane, &permissive, plan);
        if (!ok)
        {
            std::printf("  tele:   e2e plan unreachable\n");
            return failures + 1u;
        }
        std::size_t walks = 0;
        std::size_t hops = 0;
        for (const ww::runtime::Step &s : plan.steps)
        {
            walks += s.kind == ww::runtime::StepKind::Walk ? 1u : 0u;
            hops += s.kind == ww::runtime::StepKind::Transition ? 1u : 0u;
        }
        const std::size_t broken = checkPlan(view, reader, plan);
        const ww::runtime::Step &leading = plan.steps.front();
        const bool leadingIsTele = leading.kind == ww::runtime::StepKind::Transition
            && leading.transitionIndex == txIndex;
        std::printf("  tele:   e2e %zu steps (%zu walk + %zu hop) cost=%.1f %zu broken leading=%s\n",
                    plan.steps.size(), walks, hops, static_cast<double>(plan.cost), broken,
                    leadingIsTele ? "Transition(seeded)"
                                  : (leading.kind == ww::runtime::StepKind::Transition
                                         ? "Transition(other)"
                                         : "Walk"));
        return failures + broken + (leadingIsTele ? 0u : 1u);
    }

    // Drive PathAssembler end to end: a same-area refinement, a cross-area route
    // (with at least one Transition step), and an off-map goal that must come back
    // unreachable. Validates step count, walk/hop breakdown, and contiguity.
    std::size_t dumpPathAssembly(const ww::format::ArtifactReader &reader)
    {
        const auto nodes = reader.areaNodes();
        if (nodes.empty())
        {
            std::printf("  plan:   no area graph to assemble against\n");
            return 0;
        }
        ww::runtime::WorldView view(reader);
        ww::runtime::AreaSearch areaSearch(reader);
        ww::runtime::TileSearch tileSearch(view);
        ww::runtime::PathAssembler assembler(reader, view, areaSearch, tileSearch);

        const ww::format::AreaNodeRecord &n0 = nodes[0];
        const int32_t plane = static_cast<int32_t>(n0.plane);
        const int32_t area0 = view.areaAt(n0.centroidX, n0.centroidY, plane);
        const ww::runtime::TilePoint goal =
            ww::cli::farthestInArea(view, n0.centroidX, n0.centroidY, plane, area0, 24);

        std::size_t failures = runPlanQuery(assembler, view, reader, n0.centroidX, n0.centroidY,
                                            plane, goal.x, goal.y, plane, "inarea", true);
        failures += runPlanQuery(assembler, view, reader, n0.centroidX, n0.centroidY, plane,
                                 n0.centroidX + 4096, n0.centroidY, plane, "offmap", false);

        ww::cli::CrossAreaPick pick{};
        if (ww::cli::pickCrossAreaPair(reader, view, ww::cli::acceptAnyTransition, pick))
        {
            const auto &e = reader.areaEdges()[pick.edgeIndex];
            const auto &tx = reader.transitions()[e.transitionIndex];
            std::printf("  plan:   edge%zu area%d->area%d via tx%u kind=%u origin=(%d,%d,p%u)"
                        " dest=(%d,%d,p%u)\n",
                        pick.edgeIndex, e.fromArea, e.toArea, e.transitionIndex, tx.kind,
                        tx.originX, tx.originY, tx.originPlane, tx.destX, tx.destY, tx.destPlane);
            failures += runPlanQuery(assembler, view, reader, pick.start.x, pick.start.y,
                                     pick.startPlane, pick.goal.x, pick.goal.y, pick.goalPlane,
                                     "cross", true);
        }
        else
        {
            std::printf("  plan:   no traversable cross-area edge available\n");
        }

        failures += dumpCapabilityFilter(reader, view, areaSearch);
        failures += dumpTeleportSeeding(reader, view, areaSearch, assembler);
        return failures;
    }

    // Exercise the bounded search-context pool: build a 2-slot pool, validate the
    // tryAcquire saturation pattern (two grants then a refusal), drop one
    // lease, verify the slot reopens, then drive a same-tile self-assemble
    // through the borrowed context to prove the bundled components wire up
    // correctly. Blocking acquire() is not exercised here — the deterministic
    // harness avoids sleep-based thread sync; the executor drives it for real.
    std::size_t dumpContextPool(const ww::format::ArtifactReader &reader)
    {
        constexpr std::size_t kPoolSize = 2;
        ww::runtime::ContextPool pool(reader, kPoolSize);
        std::printf("  pool:   size=%zu free=%zu\n", pool.size(), pool.freeCount());

        ww::runtime::ContextLease l1 = pool.tryAcquire();
        ww::runtime::ContextLease l2 = pool.tryAcquire();
        ww::runtime::ContextLease l3 = pool.tryAcquire();
        std::printf("  pool:   tryAcquire seq=%d,%d,%d (expect 1,1,0) free=%zu\n",
                    l1 ? 1 : 0, l2 ? 1 : 0, l3 ? 1 : 0, pool.freeCount());
        if (!l1 || !l2 || l3)
        {
            std::printf("  pool:   FAIL acquisition pattern\n");
            return 1;
        }

        l1.reset();  // returns slot 1
        ww::runtime::ContextLease l4 = pool.tryAcquire();
        std::printf("  pool:   reacquire after release=%d free=%zu\n",
                    l4 ? 1 : 0, pool.freeCount());
        std::size_t failures = l4 ? 0u : 1u;

        const auto nodes = reader.areaNodes();
        if (l4 && !nodes.empty())
        {
            const ww::format::AreaNodeRecord &n0 = nodes[0];
            const int32_t plane = static_cast<int32_t>(n0.plane);
            ww::runtime::Plan plan;
            const bool ok = l4->assembler.assemble(n0.centroidX, n0.centroidY, plane,
                                                   n0.centroidX, n0.centroidY, plane, plan);
            std::printf("  pool:   borrowed self-assemble ok=%d steps=%zu cost=%.1f\n",
                        ok ? 1 : 0, plan.steps.size(), static_cast<double>(plan.cost));
            failures += ok ? 0u : 1u;
        }

        l4.reset();
        l2.reset();
        std::printf("  pool:   final free=%zu (expect %zu)\n", pool.freeCount(), kPoolSize);
        failures += pool.freeCount() == kPoolSize ? 0u : 1u;
        return failures;
    }

    std::size_t dumpArtifact(const ww::format::ArtifactReader &reader, const char *artifactPath)
    {
        const ww::format::ArtifactInfo &info = reader.info();
        std::printf("  formatVersion=%u cacheRevision=%u datasetHash=0x%08x\n", info.formatVersion,
                    info.cacheRevision, info.datasetHash);
        dumpCollision(reader);
        std::printf("  transitions: %zu records, %zu requirements, %zu chain steps\n",
                    reader.transitions().size(), reader.requirements().size(),
                    reader.chainSteps().size());
        dumpAbstraction(reader);
        std::printf("  alt: %u landmarks over %u areas\n", reader.landmarkCount(),
                    reader.altAreaCount());
        std::printf("  teleport: %zu wilderness regions, %zu no-tele zones (cutoff=%u)\n",
                    reader.wildernessRegions().size(), reader.noTeleZones().size(),
                    reader.wildernessCutoff());
        std::size_t failures = dumpRuntimeLookup(reader);
        failures += dumpAreaSearch(reader);
        failures += dumpTileSearch(reader);
        failures += dumpPathAssembly(reader);
        failures += dumpContextPool(reader);
        failures += ww::cli::runExecutorTests(reader, artifactPath);
        return failures;
    }
}


// ---- Subcommand dispatch ---------------------------------------------------
//
// One table, iterated once. Each row carries the name, the number of arguments
// that must follow it, the usage string, and the entry point — so the usage text
// and the arity check cannot drift apart the way a strcmp chain with its own
// hand-maintained usage block did.
//
// Every `run` takes the argv slice AFTER the subcommand token, with `argc` the
// count of that slice, so a row's minArgs and its handler's indices agree.
namespace
{
    int runCrossCheckCmd(int, char **argv)
    {
        return runCrossCheck(argv[0], argv[1]);
    }

    int runWallShapeTestsCmd(int, char **)
    {
        return runWallShapeTests();
    }

    int runInstanceTestsCmd(int, char **)
    {
        return runInstanceTests();
    }

    int runScriptedPathsCmd(int, char **argv)
    {
        return runScriptedPaths(argv[0]);
    }

    int runDoorPathsCmd(int, char **argv)
    {
        return runDoorPaths(argv[0]);
    }

    int runDoorProbeCmd(int, char **argv)
    {
        return runDoorProbe(argv[0], std::atoi(argv[1]));
    }

    int runTxNearCmd(int, char **argv)
    {
        return runTxNear(argv[0], std::atoi(argv[1]), std::atoi(argv[2]), std::atoi(argv[3]));
    }

    int runBenchCmd(int, char **argv)
    {
        return runBench(argv[0]);
    }

    struct Subcommand
    {
        const char *name;
        int         minArgs;   // arguments required after the token
        const char *usage;
        int (*run)(int argc, char **argv);
    };

    constexpr Subcommand kSubcommands[] = {
        {"crosscheck", 2, "wwcli crosscheck <artifact.wwa> <collision_map.bin>",
         runCrossCheckCmd},
        {"walltest",   0, "wwcli walltest", runWallShapeTestsCmd},
        {"instance",   0, "wwcli instance", runInstanceTestsCmd},
        {"scripted",   1, "wwcli scripted <artifact.wwa>", runScriptedPathsCmd},
        {"doors",      1, "wwcli doors <artifact.wwa>", runDoorPathsCmd},
        {"bench",      1, "wwcli bench <artifact.wwa>", runBenchCmd},
        {"teleports",  2, "wwcli teleports <artifact.wwa> <dataset_dir>"
                          " [<sx> <sy> <sp> <gx> <gy> <gp>]", runTeleports},
        {"doorprobe",  2, "wwcli doorprobe <artifact.wwa> <txIndex>", runDoorProbeCmd},
        {"txnear",     4, "wwcli txnear <artifact.wwa> <x> <y> <radius>", runTxNearCmd},
        {"path",       7, "wwcli path <artifact.wwa> <fromX> <fromY> <fromPlane>"
                          " <toX> <toY> <toPlane> [--out path.json] [--teleports dir]",
         runPathExport},
    };

    void printUsage()
    {
        std::printf("usage: wwcli <artifact.wwa>\n");
        for (const Subcommand &cmd : kSubcommands)
        {
            std::printf("       %s\n", cmd.usage);
        }
    }

    // The bare `wwcli <artifact.wwa>` form: load the artifact and print the full
    // harness report. Returns 1 when any check failed so a regression fails the
    // run rather than only printing a line.
    int runArtifactReport(const char *artifactPath)
    {
        try
        {
            const ww::format::ArtifactReader reader(artifactPath);
            std::printf("artifact: %s\n", artifactPath);
            const std::size_t failures = dumpArtifact(reader, artifactPath);
            std::printf("harness: %zu check(s) failed\n", failures);
            return failures > 0 ? 1 : 0;
        }
        catch (const std::exception &e)
        {
            std::printf("failed to load artifact: %s\n", e.what());
            return 1;
        }
    }
}

int main(int argc, char **argv)
{
    std::printf("wwcli - WorldWalker dev harness\n");
    std::printf("artifact format version: %u\n", static_cast<unsigned>(WW_ARTIFACT_FORMAT_VERSION));
    if (argc < 2)
    {
        printUsage();
        return 0;
    }

    for (const Subcommand &cmd : kSubcommands)
    {
        if (std::strcmp(argv[1], cmd.name) != 0)
        {
            continue;
        }
        if (argc - 2 < cmd.minArgs)
        {
            std::printf("usage: %s\n", cmd.usage);
            return 1;
        }
        return cmd.run(argc - 2, argv + 2);
    }

    // Not a subcommand. The only other accepted form is a path to an artifact,
    // so a token that names no readable file is a mistyped subcommand — say so
    // and exit 2, rather than reporting it as a failed artifact load, which read
    // as "your artifact is broken" for what was really "no such command".
    if (!std::filesystem::exists(argv[1]))
    {
        std::printf("wwcli: unknown subcommand '%s' (and no such file)\n", argv[1]);
        printUsage();
        return 2;
    }
    return runArtifactReport(argv[1]);
}
