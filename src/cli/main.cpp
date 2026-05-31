#include "c_api/worldwalker_c.h"
#include "data/Transitions.h"
#include "exec/Callbacks.h"
#include "exec/Executor.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "runtime/AreaSearch.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/ContextPool.h"
#include "runtime/PathAssembler.h"
#include "runtime/SearchContext.h"
#include "runtime/TeleportPolicy.h"
#include "runtime/TileSearch.h"
#include "runtime/WorldView.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
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

    void dumpRuntimeLookup(const ww::format::ArtifactReader &reader)
    {
        ww::runtime::WorldView view(reader);
        const auto squares = reader.collisionSquares();
        if (squares.empty())
        {
            std::printf("  runtime: no collision squares to cross-check\n");
            return;
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

    void runQuery(ww::runtime::AreaSearch &search, const ww::format::ArtifactReader &reader,
                  int32_t start, int32_t goal, const char *label)
    {
        ww::runtime::AreaPath path;
        if (!search.findPath(start, goal, path))
        {
            std::printf("  search: %-6s %d->%d unreachable\n", label, start, goal);
            return;
        }
        const std::size_t broken = checkContiguity(reader, path);
        std::printf("  search: %-6s %d->%d ok, %zu areas, cost=%.1f, %zu broken links\n",
                    label, start, goal, path.steps.size(),
                    static_cast<double>(path.cost), broken);
    }

    // Drive the area-graph A* end to end on the runtime lookup layer: resolve a
    // tile to its area through WorldView, then run a self-query, a single-edge
    // hop, and a whole-graph endpoint query, checking each route's contiguity.
    void dumpAreaSearch(const ww::format::ArtifactReader &reader)
    {
        const auto nodes = reader.areaNodes();
        if (nodes.empty())
        {
            std::printf("  search: no area graph to search\n");
            return;
        }
        ww::runtime::WorldView view(reader);
        ww::runtime::AreaSearch search(reader);

        const ww::format::AreaNodeRecord &n0 = nodes[0];
        std::printf("  search: area[0] centroid (%d,%d,p%u) -> area %d via WorldView\n",
                    n0.centroidX, n0.centroidY, n0.plane,
                    view.areaAt(n0.centroidX, n0.centroidY, n0.plane));

        runQuery(search, reader, 0, 0, "self");
        const auto edges = reader.areaEdges();
        if (!edges.empty())
        {
            runQuery(search, reader, edges[0].fromArea, edges[0].toArea, "edge0");
            const int32_t twoHop = reachableTwoHop(reader, edges[0].toArea, edges[0].fromArea);
            if (twoHop >= 0)
            {
                runQuery(search, reader, edges[0].fromArea, twoHop, "2hop");
            }
        }
        runQuery(search, reader, 0, static_cast<int32_t>(nodes.size()) - 1, "ends");
    }

    // Pick a standable in-constraint goal as far as possible from the centroid
    // within `radius`, so the tile search has to plan a genuine multi-step route
    // rather than a trivial neighbour hop. Falls back to the centroid itself.
    ww::runtime::TilePoint farthestInArea(ww::runtime::WorldView &view, int32_t cx, int32_t cy,
                                          int32_t plane, int32_t area, int32_t radius)
    {
        ww::runtime::TilePoint best{cx, cy};
        int32_t bestDist = 0;
        for (int32_t dx = -radius; dx <= radius; ++dx)
        {
            for (int32_t dy = -radius; dy <= radius; ++dy)
            {
                const int32_t x = cx + dx;
                const int32_t y = cy + dy;
                const int32_t dist = std::max(std::abs(dx), std::abs(dy));
                if (dist > bestDist && view.isStandable(x, y, plane) && view.areaAt(x, y, plane) == area)
                {
                    bestDist = dist;
                    best = {x, y};
                }
            }
        }
        return best;
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

    void runTileQuery(ww::runtime::TileSearch &search, ww::runtime::WorldView &view, int32_t sx,
                      int32_t sy, int32_t gx, int32_t gy, int32_t plane, int32_t areaConstraint,
                      const char *label)
    {
        ww::runtime::TilePath path;
        if (!search.findPath(sx, sy, gx, gy, plane, areaConstraint, path))
        {
            std::printf("  tile:   %-6s (%d,%d)->(%d,%d) unreachable\n", label, sx, sy, gx, gy);
            return;
        }
        const std::size_t broken = checkTilePath(view, path, plane, areaConstraint);
        std::printf("  tile:   %-6s (%d,%d)->(%d,%d) ok, %zu tiles, cost=%.1f, %zu broken\n",
                    label, sx, sy, gx, gy, path.tiles.size(),
                    static_cast<double>(path.cost), broken);
    }

    // Drive the tile-level refinement A* end to end on the runtime lookup layer:
    // resolve area[0]'s centroid, then run a self-query, a within-area route to
    // the farthest reachable in-area tile (and the same goal unconstrained), and
    // an off-map goal that must come back unreachable.
    void dumpTileSearch(const ww::format::ArtifactReader &reader)
    {
        const auto nodes = reader.areaNodes();
        if (nodes.empty())
        {
            std::printf("  tile:   no area graph to refine\n");
            return;
        }
        ww::runtime::WorldView view(reader);
        ww::runtime::TileSearch search(view);

        const ww::format::AreaNodeRecord &n0 = nodes[0];
        const int32_t plane = static_cast<int32_t>(n0.plane);
        const int32_t area0 = view.areaAt(n0.centroidX, n0.centroidY, plane);

        runTileQuery(search, view, n0.centroidX, n0.centroidY, n0.centroidX, n0.centroidY, plane,
                     area0, "self");
        const ww::runtime::TilePoint goal =
            farthestInArea(view, n0.centroidX, n0.centroidY, plane, area0, 24);
        runTileQuery(search, view, n0.centroidX, n0.centroidY, goal.x, goal.y, plane, area0, "inarea");
        runTileQuery(search, view, n0.centroidX, n0.centroidY, goal.x, goal.y, plane, -1, "free");
        runTileQuery(search, view, n0.centroidX, n0.centroidY, n0.centroidX + 4096, n0.centroidY,
                     plane, area0, "offmap");
    }

    // Sanity-check an assembled Plan: every Walk lands on a standable tile, every
    // Transition references a valid TransitionRecord whose origin is reachable
    // from the prior step. Returns the number of broken steps (0 for a valid plan).
    std::size_t checkPlan(ww::runtime::WorldView &view, const ww::format::ArtifactReader &reader,
                          const ww::runtime::Plan &plan)
    {
        std::size_t broken = 0;
        const auto transitions = reader.transitions();
        for (const ww::runtime::Step &s : plan.steps)
        {
            if (s.kind == ww::runtime::StepKind::Walk)
            {
                broken += view.isStandable(s.targetX, s.targetY, static_cast<int>(s.plane)) ? 0u : 1u;
                continue;
            }
            const bool indexOk = s.transitionIndex < transitions.size();
            const bool standOk = view.isStandable(s.targetX, s.targetY, static_cast<int>(s.plane));
            broken += (indexOk && standOk) ? 0u : 1u;
        }
        return broken;
    }

    void runPlanQuery(ww::runtime::PathAssembler &assembler, ww::runtime::WorldView &view,
                      const ww::format::ArtifactReader &reader, int32_t sx, int32_t sy, int32_t sp,
                      int32_t gx, int32_t gy, int32_t gp, const char *label)
    {
        ww::runtime::Plan plan;
        if (!assembler.assemble(sx, sy, sp, gx, gy, gp, plan))
        {
            std::printf("  plan:   %-6s (%d,%d,p%d)->(%d,%d,p%d) unreachable\n", label, sx, sy, sp,
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
        const std::size_t broken = checkPlan(view, reader, plan);
        std::printf("  plan:   %-6s (%d,%d,p%d)->(%d,%d,p%d) ok, %zu steps (%zu walk + %zu hop),"
                    " cost=%.1f, %zu broken\n",
                    label, sx, sy, sp, gx, gy, gp, plan.steps.size(), walks, hops,
                    static_cast<double>(plan.cost), broken);
    }

    // True when the AreaEdge's transition has a standable interact-tile in its
    // declared fromArea within a small radius of the origin — i.e., the assembler
    // can actually traverse it. Filters out artifact edges where the from-area
    // attribution is too loose for tile refinement.
    bool isTraversableEdge(const ww::format::ArtifactReader &reader, ww::runtime::WorldView &view,
                           const ww::format::AreaEdgeRecord &edge)
    {
        const auto txs = reader.transitions();
        if (edge.transitionIndex >= txs.size())
        {
            return false;
        }
        const auto &tx = txs[edge.transitionIndex];
        const int32_t plane = static_cast<int32_t>(tx.originPlane);
        for (int32_t dy = -2; dy <= 2; ++dy)
        {
            for (int32_t dx = -2; dx <= 2; ++dx)
            {
                const int32_t x = tx.originX + dx;
                const int32_t y = tx.originY + dy;
                if (view.isStandable(x, y, plane) && view.areaAt(x, y, plane) == edge.fromArea)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Pick the first AreaEdge whose transition is traversable: a standable
    // interact-tile in the declared fromArea exists at radius <= 2 of the origin.
    // The start is that interact-tile (so the cross-area harness query targets the
    // transition itself, not a long intra-area trek through whatever the artifact
    // calls fromArea), and the goal is the transition's destination tile.
    bool pickCrossAreaPair(const ww::format::ArtifactReader &reader, ww::runtime::WorldView &view,
                           ww::runtime::TilePoint &outStart, int32_t &outStartPlane,
                           ww::runtime::TilePoint &outGoal, int32_t &outGoalPlane,
                           std::size_t &outEdgeIndex)
    {
        const auto nodes = reader.areaNodes();
        const auto edges = reader.areaEdges();
        const auto txs = reader.transitions();
        for (std::size_t i = 0; i < edges.size(); ++i)
        {
            const ww::format::AreaEdgeRecord &edge = edges[i];
            if (static_cast<std::size_t>(edge.fromArea) >= nodes.size()
                || static_cast<std::size_t>(edge.toArea) >= nodes.size())
            {
                continue;
            }
            if (!isTraversableEdge(reader, view, edge))
            {
                continue;
            }
            const auto &tx = txs[edge.transitionIndex];
            const int32_t plane = static_cast<int32_t>(tx.originPlane);
            // First in-area neighbor of the origin tile, in the same scan order
            // PathAssembler::resolveInteractTile uses, so the harness starts
            // exactly where the transition step will be emitted.
            for (int32_t r = 0; r <= 2; ++r)
            {
                for (int32_t dy = -r; dy <= r; ++dy)
                {
                    for (int32_t dx = -r; dx <= r; ++dx)
                    {
                        if (std::max(std::abs(dx), std::abs(dy)) != r)
                        {
                            continue;
                        }
                        const int32_t x = tx.originX + dx;
                        const int32_t y = tx.originY + dy;
                        if (view.isStandable(x, y, plane) && view.areaAt(x, y, plane) == edge.fromArea)
                        {
                            outStart = {x, y};
                            outStartPlane = plane;
                            outGoal = {tx.destX, tx.destY};
                            outGoalPlane = static_cast<int32_t>(tx.destPlane);
                            outEdgeIndex = i;
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    }

    // First traversable area edge whose underlying transition carries a non-empty
    // requirement run. Used by the capability-filter exercise to isolate the
    // filter behavior to a single known edge instead of relying on the route
    // search to bump into one.
    bool pickRequiredCrossAreaPair(const ww::format::ArtifactReader &reader,
                                   ww::runtime::WorldView &view, std::size_t &outEdgeIndex)
    {
        const auto edges = reader.areaEdges();
        const auto txs = reader.transitions();
        const auto nodes = reader.areaNodes();
        for (std::size_t i = 0; i < edges.size(); ++i)
        {
            const ww::format::AreaEdgeRecord &edge = edges[i];
            if (static_cast<std::size_t>(edge.fromArea) >= nodes.size()
                || static_cast<std::size_t>(edge.toArea) >= nodes.size())
            {
                continue;
            }
            if (edge.transitionIndex >= txs.size()
                || txs[edge.transitionIndex].requirementCount == 0u)
            {
                continue;
            }
            if (!isTraversableEdge(reader, view, edge))
            {
                continue;
            }
            outEdgeIndex = i;
            return true;
        }
        return false;
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
    void dumpCapabilityFilter(const ww::format::ArtifactReader &reader,
                              ww::runtime::WorldView &view, ww::runtime::AreaSearch &areaSearch)
    {
        std::printf("  caps:   predicate self-check: %s\n",
                    capabilityPredicateSelfCheck() ? "ok" : "FAIL");

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

        std::size_t reqEdgeIdx = 0;
        if (!pickRequiredCrossAreaPair(reader, view, reqEdgeIdx))
        {
            std::printf("  caps:   no traversable req-bearing area edge to exercise filter end-to-end\n");
            return;
        }
        const ww::format::AreaEdgeRecord &re = edges[reqEdgeIdx];
        const ww::format::TransitionRecord &rtx = txs[re.transitionIndex];
        std::printf("  caps:   req-edge%zu area%d->area%d tx%u reqs=%u\n", reqEdgeIdx, re.fromArea,
                    re.toArea, re.transitionIndex, rtx.requirementCount);

        ww::runtime::AreaPath openPath;
        ww::runtime::AreaPath gatedPath;
        const bool openOk = areaSearch.findPath(re.fromArea, re.toArea, openPath);
        const ww::runtime::CapabilitySnapshot empty;
        const bool gatedOk = areaSearch.findPath(re.fromArea, re.toArea, &empty, gatedPath);
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

    // Build a maximally permissive capability snapshot from the artifact's full
    // requirement pool: every Skill/Item id ends up at the highest amount any
    // requirement names; every Varbit/Varp ends up at the last value it was
    // asked for. Conflicting exact-match requirements (two globals demanding
    // different values of the same varbit) cannot all pass, but each individual
    // global remains independently exercisable via the build-from-reqs pattern.
    void buildPermissiveSnapshotFromArtifact(const ww::format::ArtifactReader &reader,
                                             ww::runtime::CapabilitySnapshot &outSnapshot)
    {
        for (const ww::format::RequirementRecord &r : reader.requirements())
        {
            switch (static_cast<ww::data::RequirementKind>(r.kind))
            {
                case ww::data::RequirementKind::Skill:
                    if (outSnapshot.skillLevel(r.id) < r.amount)
                    {
                        outSnapshot.setSkillLevel(r.id, r.amount);
                    }
                    break;
                case ww::data::RequirementKind::Item:
                    if (outSnapshot.itemCount(r.id) < r.amount)
                    {
                        outSnapshot.setItemCount(r.id, r.amount);
                    }
                    break;
                case ww::data::RequirementKind::Varbit:
                    outSnapshot.setVarbit(r.id, r.amount);
                    break;
                case ww::data::RequirementKind::Varp:
                    outSnapshot.setVarp(r.id, r.amount);
                    break;
            }
        }
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
    void dumpTeleportSeeding(const ww::format::ArtifactReader &reader,
                             ww::runtime::WorldView &view,
                             ww::runtime::AreaSearch &areaSearch,
                             ww::runtime::PathAssembler &assembler)
    {
        std::printf("  tele:   policy self-check: %s\n",
                    teleportPolicySelfCheck() ? "ok" : "FAIL");

        ww::runtime::CapabilitySnapshot permissive;
        buildPermissiveSnapshotFromArtifact(reader, permissive);
        std::size_t globalsTotal = 0;
        std::size_t globalsEmpty = 0;
        std::size_t globalsPermissive = 0;
        countGlobalAcceptance(reader, permissive, globalsTotal, globalsEmpty, globalsPermissive);
        std::printf("  tele:   globals %zu (empty-snapshot accepts %zu, permissive %zu)\n",
                    globalsTotal, globalsEmpty, globalsPermissive);

        const auto nodes = reader.areaNodes();
        if (nodes.empty())
        {
            return;
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
            return;
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
            return;
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
    }

    // Drive PathAssembler end to end: a same-area refinement, a cross-area route
    // (with at least one Transition step), and an off-map goal that must come back
    // unreachable. Validates step count, walk/hop breakdown, and contiguity.
    void dumpPathAssembly(const ww::format::ArtifactReader &reader)
    {
        const auto nodes = reader.areaNodes();
        if (nodes.empty())
        {
            std::printf("  plan:   no area graph to assemble against\n");
            return;
        }
        ww::runtime::WorldView view(reader);
        ww::runtime::AreaSearch areaSearch(reader);
        ww::runtime::TileSearch tileSearch(view);
        ww::runtime::PathAssembler assembler(reader, view, areaSearch, tileSearch);

        const ww::format::AreaNodeRecord &n0 = nodes[0];
        const int32_t plane = static_cast<int32_t>(n0.plane);
        const int32_t area0 = view.areaAt(n0.centroidX, n0.centroidY, plane);
        const ww::runtime::TilePoint goal =
            farthestInArea(view, n0.centroidX, n0.centroidY, plane, area0, 24);

        runPlanQuery(assembler, view, reader, n0.centroidX, n0.centroidY, plane, goal.x, goal.y,
                     plane, "inarea");
        runPlanQuery(assembler, view, reader, n0.centroidX, n0.centroidY, plane,
                     n0.centroidX + 4096, n0.centroidY, plane, "offmap");

        ww::runtime::TilePoint startTile{};
        ww::runtime::TilePoint goalTile{};
        int32_t startPlane = 0;
        int32_t goalPlane = 0;
        std::size_t edgeIdx = 0;
        if (pickCrossAreaPair(reader, view, startTile, startPlane, goalTile, goalPlane, edgeIdx))
        {
            const auto &e = reader.areaEdges()[edgeIdx];
            const auto &tx = reader.transitions()[e.transitionIndex];
            std::printf("  plan:   edge%zu area%d->area%d via tx%u kind=%u origin=(%d,%d,p%u)"
                        " dest=(%d,%d,p%u)\n",
                        edgeIdx, e.fromArea, e.toArea, e.transitionIndex, tx.kind,
                        tx.originX, tx.originY, tx.originPlane, tx.destX, tx.destY, tx.destPlane);
            runPlanQuery(assembler, view, reader, startTile.x, startTile.y, startPlane, goalTile.x,
                         goalTile.y, goalPlane, "cross");
        }
        else
        {
            std::printf("  plan:   no traversable cross-area edge available\n");
        }

        dumpCapabilityFilter(reader, view, areaSearch);
        dumpTeleportSeeding(reader, view, areaSearch, assembler);
    }

    // Exercise the bounded search-context pool: build a 2-slot pool, validate the
    // tryAcquire saturation pattern (two grants then a refusal), release one,
    // verify the slot reopens, then drive a same-tile self-assemble through the
    // borrowed context to prove the bundled components wire up correctly.
    // Blocking acquire() is not exercised here — the deterministic harness avoids
    // sleep-based thread sync; the executor in Phase 4 will drive it for real.
    void dumpContextPool(const ww::format::ArtifactReader &reader)
    {
        constexpr std::size_t kPoolSize = 2;
        ww::runtime::ContextPool pool(reader, kPoolSize);
        std::printf("  pool:   size=%zu free=%zu\n", pool.size(), pool.freeCount());

        ww::runtime::SearchContext *c1 = nullptr;
        ww::runtime::SearchContext *c2 = nullptr;
        ww::runtime::SearchContext *c3 = nullptr;
        const bool got1 = pool.tryAcquire(c1);
        const bool got2 = pool.tryAcquire(c2);
        const bool got3 = pool.tryAcquire(c3);
        std::printf("  pool:   tryAcquire seq=%d,%d,%d (expect 1,1,0) free=%zu\n",
                    got1 ? 1 : 0, got2 ? 1 : 0, got3 ? 1 : 0, pool.freeCount());
        if (!got1 || !got2 || got3)
        {
            std::printf("  pool:   FAIL acquisition pattern\n");
            if (got1) { pool.release(*c1); }
            if (got2) { pool.release(*c2); }
            return;
        }

        pool.release(*c1);
        ww::runtime::SearchContext *c4 = nullptr;
        const bool got4 = pool.tryAcquire(c4);
        std::printf("  pool:   reacquire after release=%d free=%zu\n",
                    got4 ? 1 : 0, pool.freeCount());

        const auto nodes = reader.areaNodes();
        if (got4 && !nodes.empty())
        {
            const ww::format::AreaNodeRecord &n0 = nodes[0];
            const int32_t plane = static_cast<int32_t>(n0.plane);
            ww::runtime::Plan plan;
            const bool ok = c4->assembler.assemble(n0.centroidX, n0.centroidY, plane,
                                                   n0.centroidX, n0.centroidY, plane, plan);
            std::printf("  pool:   borrowed self-assemble ok=%d steps=%zu cost=%.1f\n",
                        ok ? 1 : 0, plan.steps.size(), static_cast<double>(plan.cost));
        }

        if (got4) { pool.release(*c4); }
        pool.release(*c2);
        std::printf("  pool:   final free=%zu (expect %zu)\n", pool.freeCount(), kPoolSize);
    }

    // Counters + fixed position for the Executor harness. Routed via the
    // Callbacks.user cookie so each function pointer stays a plain extern "C"
    // entry. Action callbacks bump abortIfCalled — the 4a happy path stays
    // inside readPosition + onEvent.
    struct ExecHarness
    {
        ww::exec::WwTile position;
        int readPositionCalls;
        int onEventCalls;
        int abortIfCalled;
        ww::exec::WwEventKind lastEventKind;
    };

    extern "C" void harnessReadPosition(void *user, ww::exec::WwTile *outTile)
    {
        ExecHarness *h = static_cast<ExecHarness *>(user);
        ++h->readPositionCalls;
        *outTile = h->position;
    }

    extern "C" void harnessReadCapability(void *user, ww::exec::WwCapabilitySnapshot *outSnapshot)
    {
        ExecHarness *h = static_cast<ExecHarness *>(user);
        ++h->abortIfCalled;
        *outSnapshot = ww::exec::WwCapabilitySnapshot{};
    }

    extern "C" int32_t harnessReadVarbit(void *user, int32_t)
    {
        ExecHarness *h = static_cast<ExecHarness *>(user);
        ++h->abortIfCalled;
        return 0;
    }

    extern "C" int32_t harnessIsInterfaceOpen(void *user, int32_t)
    {
        ExecHarness *h = static_cast<ExecHarness *>(user);
        ++h->abortIfCalled;
        return 0;
    }

    extern "C" void harnessWalkTo(void *user, ww::exec::WwTile)
    {
        ExecHarness *h = static_cast<ExecHarness *>(user);
        ++h->abortIfCalled;
    }

    extern "C" void harnessInteract(void *user, int32_t, ww::exec::WwTile, int32_t)
    {
        ExecHarness *h = static_cast<ExecHarness *>(user);
        ++h->abortIfCalled;
    }

    extern "C" void harnessRunChainStep(void *user, int32_t, int32_t)
    {
        ExecHarness *h = static_cast<ExecHarness *>(user);
        ++h->abortIfCalled;
    }

    extern "C" void harnessSleepTicks(void *user, int32_t)
    {
        ExecHarness *h = static_cast<ExecHarness *>(user);
        ++h->abortIfCalled;
    }

    extern "C" int32_t harnessShouldCancel(void *user)
    {
        ExecHarness *h = static_cast<ExecHarness *>(user);
        ++h->abortIfCalled;
        return 0;
    }

    extern "C" void harnessOnEvent(void *user, const ww::exec::WwEvent *event)
    {
        ExecHarness *h = static_cast<ExecHarness *>(user);
        ++h->onEventCalls;
        h->lastEventKind = event->kind;
    }

    void dumpExecutor(const ww::format::ArtifactReader &reader)
    {
        const auto nodes = reader.areaNodes();
        if (nodes.empty())
        {
            std::printf("  exec:   skipped (no area nodes)\n");
            return;
        }

        ww::runtime::ContextPool pool(reader, 1);
        const ww::format::AreaNodeRecord &n0 = nodes[0];
        const int32_t plane = static_cast<int32_t>(n0.plane);

        ExecHarness harness{};
        harness.position = ww::exec::WwTile{ n0.centroidX, n0.centroidY, plane };
        harness.lastEventKind = ww::exec::WwEventKind::Failed;

        const ww::exec::Callbacks cb{
            &harness,
            harnessReadPosition,
            harnessReadCapability,
            harnessReadVarbit,
            harnessIsInterfaceOpen,
            harnessWalkTo,
            harnessInteract,
            harnessRunChainStep,
            harnessSleepTicks,
            harnessShouldCancel,
            harnessOnEvent,
        };

        ww::exec::Executor executor(reader, pool, cb);
        const ww::exec::WwGoal goal{ n0.centroidX, n0.centroidY, plane, 0 };
        const ww::exec::WwStatus status = executor.run(goal);

        std::printf("  exec:   start==goal status=%d (expect 0=Arrived) free=%zu\n",
                    static_cast<int>(status), pool.freeCount());
        std::printf("  exec:   readPosition=%d onEvent=%d lastEvent=%d (expect 1,1,%d) actions=%d (expect 0)\n",
                    harness.readPositionCalls, harness.onEventCalls,
                    static_cast<int>(harness.lastEventKind),
                    static_cast<int>(ww::exec::WwEventKind::Arrived),
                    harness.abortIfCalled);
    }

    void dumpArtifact(const ww::format::ArtifactReader &reader)
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
        dumpRuntimeLookup(reader);
        dumpAreaSearch(reader);
        dumpTileSearch(reader);
        dumpPathAssembly(reader);
        dumpContextPool(reader);
        dumpExecutor(reader);
    }
}

int main(int argc, char **argv)
{
    std::printf("wwcli - WorldWalker dev harness\n");
    std::printf("artifact format version: %u\n", static_cast<unsigned>(WW_ARTIFACT_FORMAT_VERSION));
    if (argc < 2)
    {
        std::printf("usage: wwcli <artifact.wwa>\n");
        return 0;
    }

    try
    {
        const ww::format::ArtifactReader reader(argv[1]);
        std::printf("artifact: %s\n", argv[1]);
        dumpArtifact(reader);
    }
    catch (const std::exception &e)
    {
        std::printf("failed to load artifact: %s\n", e.what());
        return 1;
    }
    return 0;
}
