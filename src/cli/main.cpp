#include "c_api/worldwalker_c.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "runtime/AreaSearch.h"
#include "runtime/PathAssembler.h"
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
