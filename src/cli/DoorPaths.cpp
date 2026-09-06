#include "cli/DoorPaths.h"

#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "format/ClipFlags.h"
#include "format/WallApproach.h"
#include "runtime/AreaSearch.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/PathAssembler.h"
#include "runtime/TileScan.h"
#include "runtime/TileSearch.h"
#include "runtime/WorldView.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <span>
#include <unordered_map>
#include <vector>

namespace
{
    // A door must hop no farther than this (Chebyshev, origin->dest). Doors and
    // gates step you onto the tile just past the wall; the dest may have snapped
    // a tile or two during the bake, so the bound is loose enough to admit those
    // without letting long Transport links (mine-cart rides, levers that fling
    // you across the map) masquerade as doors.
    constexpr int kMaxDoorHop = 4;

    // Drive every area-splitting door end to end (no sampling cap). Per-door
    // lines are emitted only for failures and anomalies so a full sweep stays
    // readable; the trailing summary carries the verdict.

    int chebyshev(int ax, int ay, int bx, int by)
    {
        return std::max(std::abs(ax - bx), std::abs(ay - by));
    }

    // True when tx is a "door" in the graph sense: a local-origin Transport
    // that stays on one plane and hops a short distance — you interact with an
    // object to cross a same-floor barrier. In the curated dataset this single
    // category (all shape 10) covers literal doors/gates ("Open"/"Enter"),
    // wilderness-wall crossings, and stepping-stone / interactive-scenery
    // shortcuts; shape does NOT distinguish them, so it is not filtered on.
    // Ladders/stairs (plane change), teleports (global origin), and long
    // Transport rides are excluded.
    bool isDoor(const ww::format::TransitionRecord &tx)
    {
        if (static_cast<ww::data::TransitionKind>(tx.kind) != ww::data::TransitionKind::Transport)
        {
            return false;
        }
        if ((tx.flags & ww::format::kTransitionFlagGlobalOrigin) != 0u)
        {
            return false;
        }
        if (tx.originPlane != tx.destPlane)
        {
            return false;
        }
        const int hop = chebyshev(tx.originX, tx.originY, tx.destX, tx.destY);
        return hop >= 1 && hop <= kMaxDoorHop;
    }

    // Clip accessor in the shape format::WallApproach's templates want: any
    // callable answering the tile's clip word, with CLIP_BLOCKED for a tile it
    // cannot address. WorldView::clipAt already does exactly that.
    auto clipAccessor(ww::runtime::WorldView &view)
    {
        return [&view](int x, int y, int plane) { return view.clipAt(x, y, plane); };
    }

    // --- Interact-side diagnostic -------------------------------------------
    // For a FAILing door, ask whether the area-graph builder could wire an edge
    // at all: is there a standable interact side within the shared approach
    // radius, reachable from the origin without crossing a wall, whose area
    // differs from dest? This mirrors AreaGraph::collectOriginAreas — through
    // the same ww::format::approachSealed it calls, rather than a local
    // re-derivation of the wall rules that had already drifted from it — run
    // against the baked WorldView, i.e. the same collision + area data the
    // builder used. "no edge" means the origin object is moated by blocked or
    // sealed tiles and needs a different fix.
    int predictRecoveredFromArea(ww::runtime::WorldView &view, int originX, int originY, int plane,
                                 int destArea)
    {
        constexpr int radius = ww::data::kTransitionApproachRadius;
        for (int ox = -radius; ox <= radius; ++ox)
        {
            for (int oy = -radius; oy <= radius; ++oy)
            {
                const int candX = originX + ox;
                const int candY = originY + oy;
                // The origin tile itself has no approach to seal; it only has to
                // be walkable, which is what having an area tests.
                if ((ox != 0 || oy != 0)
                    && ww::format::approachSealed(clipAccessor(view), candX, candY,
                                                  originX, originY, plane))
                {
                    continue;
                }
                const int a = view.areaAt(candX, candY, plane);
                if (a >= 0 && a != destArea)
                {
                    return a;
                }
            }
        }
        return -1;
    }

    // "Any area will do" for resolveApproach, used only for a door the graph
    // baked no edge for, which therefore has no declared fromArea to pin to.
    constexpr int kAnyArea = -1;

    // The approach tile the planner will stand on to click this door: exactly
    // what PathAssembler::resolveInteractTile resolves for the AreaEdge the
    // planner would route over — standable AND in that edge's declared
    // fromArea, over kTransitionApproachRadius, ranked from the origin.
    //
    // Pinning to fromArea is what keeps the nearest-tile tie-break honest. A
    // predicate of merely "standable and in some area" lets the closest tile win
    // even when it sits in a one-tile pocket the edge does not reach, and the
    // harness then reports a door the planner can cross as one it cannot.
    bool resolveApproach(ww::runtime::WorldView &view, int originX, int originY, int plane,
                         int fromArea, int &outX, int &outY)
    {
        const auto standableInArea = [&](int32_t x, int32_t y)
        {
            if (!view.isStandable(x, y, plane))
            {
                return false;
            }
            const int32_t area = view.areaAt(x, y, plane);
            return fromArea == kAnyArea ? area >= 0 : area == fromArea;
        };
        int32_t x = 0;
        int32_t y = 0;
        if (!ww::runtime::findNearestTile(originX, originY,
                                          ww::data::kTransitionApproachRadius, true,
                                          standableInArea, originX, originY, x, y))
        {
            return false;
        }
        outX = x;
        outY = y;
        return true;
    }

    // transitionIndex -> the AreaEdge rows the graph baked for it. A transition
    // can own more than one when its object is approachable from several areas
    // (AreaGraph::collectOriginAreas emits one edge per side), and the planner
    // may route over any of them.
    using EdgeIndex = std::unordered_multimap<uint32_t, std::size_t>;

    EdgeIndex indexEdgesByTransition(std::span<const ww::format::AreaEdgeRecord> edges)
    {
        EdgeIndex index;
        index.reserve(edges.size());
        for (std::size_t i = 0; i < edges.size(); ++i)
        {
            index.emplace(edges[i].transitionIndex, i);
        }
        return index;
    }

    // Where the planner would stand to click this door. Prefer the fromArea of
    // an AreaEdge the graph actually baked for this transition — that is the
    // tile PathAssembler::resolveInteractTile picks when it routes over that
    // edge, so the harness starts exactly where the plan will.
    //
    // A door the graph baked no edge for has no such area. It is driven anyway,
    // from any standable in-area tile, so it fails loudly instead of dropping
    // out of the sweep — an unwired door is precisely the defect this harness
    // exists to surface.
    bool resolveDoorApproach(ww::runtime::WorldView &view,
                             std::span<const ww::format::AreaEdgeRecord> edges,
                             const EdgeIndex &edgeIndex, uint32_t doorIndex,
                             const ww::format::TransitionRecord &tx, int &outX, int &outY)
    {
        const int plane = static_cast<int>(tx.originPlane);
        const auto range = edgeIndex.equal_range(doorIndex);
        for (auto it = range.first; it != range.second; ++it)
        {
            if (resolveApproach(view, tx.originX, tx.originY, plane,
                                edges[it->second].fromArea, outX, outY))
            {
                return true;
            }
        }
        if (range.first != range.second)
        {
            return false;   // wired, but no side of it is standable
        }
        return resolveApproach(view, tx.originX, tx.originY, plane, kAnyArea, outX, outY);
    }

    // True when `plan` crosses a door: it contains a Transition step whose record
    // is a same-plane Transport. exactIndex is set when that step is this very
    // door (the planner usually picks it, but an equivalent neighbouring door
    // also satisfies the traversal property, so PASS does not hinge on it).
    bool planCrossesDoor(const ww::runtime::Plan &plan,
                         std::span<const ww::format::TransitionRecord> txs,
                         uint32_t doorIndex, bool &exactIndex)
    {
        exactIndex = false;
        bool crossed = false;
        for (const ww::runtime::Step &s : plan.steps)
        {
            if (s.kind != ww::runtime::StepKind::Transition || s.transitionIndex >= txs.size())
            {
                continue;
            }
            const ww::format::TransitionRecord &tx = txs[s.transitionIndex];
            if (static_cast<ww::data::TransitionKind>(tx.kind) == ww::data::TransitionKind::Transport
                && tx.originPlane == tx.destPlane)
            {
                crossed = true;
                exactIndex = exactIndex || (s.transitionIndex == doorIndex);
            }
        }
        return crossed;
    }

    struct Totals
    {
        int doors        = 0;  // same-plane short local Transports
        int splitting    = 0;  // doors whose two sides are in different areas
        int tested       = 0;  // driven end to end (capped)
        int passed       = 0;  // plan crossed a door
        int failed       = 0;
        int exact        = 0;  // plan used this very door
        int walkBlocked  = 0;  // walk-only control could not cross
    };

    // Drive one area-splitting door end to end. approach -> dest must route
    // through a door; the walk-only control (no transitions) must fail, proving
    // the wall actually separates the two sides.
    void testDoor(ww::runtime::WorldView &view, ww::runtime::TileSearch &tileSearch,
                  ww::runtime::PathAssembler &assembler,
                  const ww::runtime::CapabilitySnapshot &snapshot,
                  std::span<const ww::format::TransitionRecord> txs, uint32_t doorIndex,
                  int approachX, int approachY, Totals &totals)
    {
        const ww::format::TransitionRecord &tx = txs[doorIndex];
        const int plane = static_cast<int>(tx.originPlane);

        ww::runtime::TilePath walkOnly;
        const bool walkable = tileSearch.findPath(approachX, approachY, tx.destX, tx.destY, plane,
                                                  -1, walkOnly);
        if (!walkable)
        {
            ++totals.walkBlocked;
        }

        ww::runtime::Plan plan;
        const bool ok = assembler.assemble(approachX, approachY, plane, tx.destX, tx.destY,
                                           static_cast<int>(tx.destPlane), &snapshot, plan);
        bool exact = false;
        const bool crossed = ok && planCrossesDoor(plan, txs, doorIndex, exact);

        ++totals.tested;
        totals.passed += crossed ? 1 : 0;
        totals.failed += crossed ? 0 : 1;
        totals.exact  += exact ? 1 : 0;

        // Only surface the interesting cases: a door the planner failed to
        // cross (FAIL), or one whose walk-only route was unexpectedly OPEN
        // (the two sides were not truly wall-separated — informative, not a
        // failure, since the plan still reached the goal).
        if (!crossed)
        {
            const int destArea = view.areaAt(tx.destX, tx.destY, static_cast<int>(tx.destPlane));
            const int recovered = predictRecoveredFromArea(view, tx.originX, tx.originY, plane,
                                                           destArea);
            std::printf("doors:  FAIL tx%-5u obj=%-6d (%d,%d,p%d)->(%d,%d) approach=(%d,%d)"
                        " walk-only=%-7s plan=%zu steps cost=%.1f%s | radius-2 reach: %s\n",
                        doorIndex, tx.objectId, tx.originX, tx.originY, plane, tx.destX,
                        tx.destY, approachX, approachY, walkable ? "OPEN" : "BLOCKED",
                        plan.steps.size(), static_cast<double>(plan.cost),
                        exact ? " [this door]" : "",
                        recovered >= 0 ? "side found (radius bump would wire it)" : "no edge");
            if (recovered >= 0)
            {
                std::printf("doors:       -> would emit AreaEdge area%d -> area%d\n", recovered,
                            destArea);
            }
        }
        else if (walkable)
        {
            std::printf("doors:  note tx%-5u obj=%-6d (%d,%d,p%d)->(%d,%d) crossed but"
                        " walk-only was OPEN (sides not wall-separated)\n",
                        doorIndex, tx.objectId, tx.originX, tx.originY, plane, tx.destX, tx.destY);
        }
    }
}

namespace
{
    // Print a (2r+1)x(2r+1) grid centred on (cx, cy): each cell shows the tile's
    // area id, "###" when unstandable, or "  ." off-area. North (+Y) is printed
    // at the top so it reads like the map. The centre tile is wrapped in [].
    void dumpGrid(ww::runtime::WorldView &view, int cx, int cy, int plane, int r,
                  const char *label)
    {
        std::printf("probe:  %s (%d,%d,p%d) area=%d stand=%d clip=0x%08x\n", label, cx, cy, plane,
                    view.areaAt(cx, cy, plane), view.isStandable(cx, cy, plane) ? 1 : 0,
                    view.clipAt(cx, cy, plane));
        for (int dy = r; dy >= -r; --dy)
        {
            std::printf("probe:  ");
            for (int dx = -r; dx <= r; ++dx)
            {
                const int x = cx + dx;
                const int y = cy + dy;
                const bool centre = dx == 0 && dy == 0;
                char cell[8];
                if (!view.isStandable(x, y, plane))
                {
                    std::snprintf(cell, sizeof(cell), "###");
                }
                else
                {
                    const int a = view.areaAt(x, y, plane);
                    std::snprintf(cell, sizeof(cell), a >= 0 ? "%3d" : "  .", a);
                }
                std::printf(centre ? "[%s]" : " %s ", cell);
            }
            std::printf("\n");
        }
    }
}

int runDoorProbe(const char *wwaPath, int txIndex)
{
    try
    {
        const ww::format::ArtifactReader reader(wwaPath);
        ww::runtime::WorldView view(reader);
        const auto txs = reader.transitions();
        if (txIndex < 0 || static_cast<std::size_t>(txIndex) >= txs.size())
        {
            std::printf("probe:  tx index %d out of range (0..%zu)\n", txIndex, txs.size());
            return 1;
        }
        const ww::format::TransitionRecord &tx = txs[txIndex];
        std::printf("probe:  tx%d kind=%u obj=%d origin=(%d,%d,p%u) dest=(%d,%d,p%u)\n", txIndex,
                    tx.kind, tx.objectId, tx.originX, tx.originY, tx.originPlane, tx.destX, tx.destY,
                    tx.destPlane);
        dumpGrid(view, tx.originX, tx.originY, static_cast<int>(tx.originPlane), 3, "origin");
        dumpGrid(view, tx.destX, tx.destY, static_cast<int>(tx.destPlane), 3, "dest");
        return 0;
    }
    catch (const std::exception &e)
    {
        std::printf("probe:  failed: %s\n", e.what());
        return 1;
    }
}

int runTxNear(const char *wwaPath, int x, int y, int radius)
{
    try
    {
        const ww::format::ArtifactReader reader(wwaPath);
        ww::runtime::WorldView view(reader);

        std::printf("txnear: tile (%d,%d) radius=%d\n", x, y, radius);
        for (int p = 0; p <= 3; ++p)
        {
            std::printf("txnear:   plane %d: stand=%d area=%d clip=0x%08x\n", p,
                        view.isStandable(x, y, p) ? 1 : 0, view.areaAt(x, y, p),
                        view.clipAt(x, y, p));
        }

        const auto txs = reader.transitions();
        int hits = 0;
        for (uint32_t i = 0; i < txs.size(); ++i)
        {
            const ww::format::TransitionRecord &tx = txs[i];
            const int od = chebyshev(tx.originX, tx.originY, x, y);
            const int dd = chebyshev(tx.destX, tx.destY, x, y);
            if (od > radius && dd > radius)
            {
                continue;
            }
            ++hits;
            const int oa = view.areaAt(tx.originX, tx.originY, static_cast<int>(tx.originPlane));
            const int da = view.areaAt(tx.destX, tx.destY, static_cast<int>(tx.destPlane));
            std::printf("txnear:   tx%u kind=%u obj=%d opt=%d flags=0x%x origin=(%d,%d,p%u)a%d"
                        " dest=(%d,%d,p%u)a%d cost=%.1f [od=%d dd=%d]\n",
                        i, tx.kind, tx.objectId, tx.optionIndex, tx.flags, tx.originX, tx.originY,
                        tx.originPlane, oa, tx.destX, tx.destY, tx.destPlane, da,
                        static_cast<double>(tx.cost), od, dd);
        }
        std::printf("txnear: %d transition(s) within radius\n", hits);

        // Scan the bbox for tiles carrying crossing/blocker clip bits the
        // deriver keys on — a ladder/stair the cache marked but the bake never
        // turned into a transition shows up here as a CLIP_PLANE_CHANGE /
        // CLIP_CLIMBOVER / CLIP_AGILITY tile with no matching tx above.
        struct Bit { uint32_t mask; const char *name; };
        const Bit bits[] = {
            {0x10000000u, "PLANE_CHANGE"}, {0x20000000u, "CLIMBOVER"},
            {0x08000000u, "AGILITY"},      {0x04000000u, "DOOR"},
        };
        for (int p = 0; p <= 3; ++p)
        {
            for (int yy = y + radius; yy >= y - radius; --yy)
            {
                for (int xx = x - radius; xx <= x + radius; ++xx)
                {
                    const uint32_t c = view.clipAt(xx, yy, p);
                    for (const Bit &b : bits)
                    {
                        if ((c & b.mask) != 0u)
                        {
                            std::printf("txnear:   clip %-12s (%d,%d,p%d) a%d clip=0x%08x\n",
                                        b.name, xx, yy, p, view.areaAt(xx, yy, p), c);
                        }
                    }
                }
            }
        }
        return 0;
    }
    catch (const std::exception &e)
    {
        std::printf("txnear: failed: %s\n", e.what());
        return 1;
    }
}

int runDoorPaths(const char *wwaPath)
{
    try
    {
        const ww::format::ArtifactReader reader(wwaPath);
        ww::runtime::WorldView     view(reader);
        ww::runtime::AreaSearch    areaSearch(reader);
        ww::runtime::TileSearch    tileSearch(view);
        ww::runtime::PathAssembler assembler(reader, view, areaSearch, tileSearch);

        ww::runtime::CapabilitySnapshot snapshot;
        ww::runtime::applyPermissiveRequirements(reader.requirements(), snapshot);

        const auto txs   = reader.transitions();
        const auto edges = reader.areaEdges();
        const EdgeIndex edgeIndex = indexEdgesByTransition(edges);
        std::printf("doors:  %s\n", wwaPath);

        Totals totals;
        for (uint32_t i = 0; i < txs.size(); ++i)
        {
            const ww::format::TransitionRecord &tx = txs[i];
            if (!isDoor(tx))
            {
                continue;
            }
            ++totals.doors;

            const int plane = static_cast<int>(tx.originPlane);
            int approachX = 0;
            int approachY = 0;
            if (!resolveDoorApproach(view, edges, edgeIndex, i, tx, approachX, approachY))
            {
                continue;
            }
            // A door is only a discriminating test when its two sides sit in
            // different areas — then no pure walk can cross, so the planner is
            // forced through the door. Same-area "doors" (open archways) don't
            // prove anything about transition traversal.
            const int fromArea = view.areaAt(approachX, approachY, plane);
            const int toArea   = view.areaAt(tx.destX, tx.destY, plane);
            if (fromArea < 0 || toArea < 0 || fromArea == toArea)
            {
                continue;
            }
            ++totals.splitting;
            testDoor(view, tileSearch, assembler, snapshot, txs, i, approachX, approachY, totals);
        }

        std::printf("doors:  found %d same-plane local doors, %d split two areas\n",
                    totals.doors, totals.splitting);
        std::printf("doors:  tested all %d: %d pass / %d fail; %d used the exact door,"
                    " %d had walk-only blocked\n",
                    totals.tested, totals.passed, totals.failed, totals.exact,
                    totals.walkBlocked);
        return totals.failed == 0 ? 0 : 1;
    }
    catch (const std::exception &e)
    {
        std::printf("doors:  failed: %s\n", e.what());
        return 1;
    }
}
