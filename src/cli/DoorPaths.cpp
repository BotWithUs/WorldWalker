#include "cli/DoorPaths.h"

#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "format/ClipFlags.h"
#include "runtime/AreaSearch.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/PathAssembler.h"
#include "runtime/TileSearch.h"
#include "runtime/WorldView.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <span>

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

    // --- Interact-side diagnostic -------------------------------------------
    // For a FAILing door, ask whether a wider (radius-2) origin scan would let
    // the area-graph builder wire an edge: is there a standable, wall-free
    // interact side within radius 2 whose area differs from dest? Run against
    // the baked WorldView (the same collision + area data the builder used), so
    // it predicts a rebuild without one. "no edge" means widening the builder
    // radius would NOT recover the door — the origin object is moated by blocked
    // tiles and needs a different fix than a radius bump.

    // True when a unit at (x, y) cannot step toward (dx, dy) because that
    // direction is wall-blocked on the source tile. Mirrors AreaGraph.cpp's
    // wallBlocksStepFrom; (dx, dy) in {-1, 0, 1}^2 \ {(0, 0)}.
    bool wallBlocksStep(ww::runtime::WorldView &view, int x, int y, int plane, int dx, int dy)
    {
        const uint32_t flags = view.clipAt(x, y, plane);
        uint32_t mask = 0;
        if      (dx ==  0 && dy ==  1) { mask = ww::format::CLIP_WALL_N;  }
        else if (dx ==  1 && dy ==  1) { mask = ww::format::CLIP_WALL_NE; }
        else if (dx ==  1 && dy ==  0) { mask = ww::format::CLIP_WALL_E;  }
        else if (dx ==  1 && dy == -1) { mask = ww::format::CLIP_WALL_SE; }
        else if (dx ==  0 && dy == -1) { mask = ww::format::CLIP_WALL_S;  }
        else if (dx == -1 && dy == -1) { mask = ww::format::CLIP_WALL_SW; }
        else if (dx == -1 && dy ==  0) { mask = ww::format::CLIP_WALL_W;  }
        else if (dx == -1 && dy ==  1) { mask = ww::format::CLIP_WALL_NW; }
        return (flags & mask) != 0u;
    }

    // The interact-from area at offset (ox, oy), or -1 if unusable. Mirror of
    // AreaGraph.cpp::reachableSideArea against WorldView.
    int reachableSideArea(ww::runtime::WorldView &view, int ox, int oy, int originX, int originY,
                          int plane)
    {
        int curX = originX + ox;
        int curY = originY + oy;
        if (!view.isStandable(curX, curY, plane))
        {
            return -1;
        }
        const int area = view.areaAt(curX, curY, plane);
        if (area < 0)
        {
            return -1;
        }
        while (std::max(std::abs(curX - originX), std::abs(curY - originY)) > 1)
        {
            const int sx = (originX > curX) - (originX < curX);
            const int sy = (originY > curY) - (originY < curY);
            if (wallBlocksStep(view, curX, curY, plane, sx, sy))
            {
                return -1;
            }
            curX += sx;
            curY += sy;
            if (!view.isStandable(curX, curY, plane))
            {
                return -1;
            }
        }
        const int fx = (originX > curX) - (originX < curX);
        const int fy = (originY > curY) - (originY < curY);
        if (wallBlocksStep(view, curX, curY, plane, fx, fy))
        {
            return -1;
        }
        return area;
    }

    // First radius-2 interact side whose area differs from destArea, or -1.
    // A non-negative result means the fixed builder would emit an AreaEdge
    // fromArea -> destArea for this door (i.e. the fix recovers it).
    int predictRecoveredFromArea(ww::runtime::WorldView &view, int originX, int originY, int plane,
                                 int destArea)
    {
        for (int ox = -2; ox <= 2; ++ox)
        {
            for (int oy = -2; oy <= 2; ++oy)
            {
                if (ox == 0 && oy == 0)
                {
                    continue;
                }
                const int a = reachableSideArea(view, ox, oy, originX, originY, plane);
                if (a >= 0 && a != destArea)
                {
                    return a;
                }
            }
        }
        return -1;
    }

    // First standable tile within radius 2 of (ox, oy), scanned ring by ring so
    // the closest approach wins — the same order PathAssembler::resolveInteractTile
    // uses. Returns false when the door has no walkable approach on this plane.
    bool resolveApproach(ww::runtime::WorldView &view, int ox, int oy, int plane,
                         int &outX, int &outY)
    {
        for (int r = 0; r <= 2; ++r)
        {
            for (int dy = -r; dy <= r; ++dy)
            {
                for (int dx = -r; dx <= r; ++dx)
                {
                    if (std::max(std::abs(dx), std::abs(dy)) != r)
                    {
                        continue;
                    }
                    if (view.isStandable(ox + dx, oy + dy, plane))
                    {
                        outX = ox + dx;
                        outY = oy + dy;
                        return true;
                    }
                }
            }
        }
        return false;
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

    void buildPermissiveSnapshot(const ww::format::ArtifactReader &reader,
                                 ww::runtime::CapabilitySnapshot &out)
    {
        for (const ww::format::RequirementRecord &r : reader.requirements())
        {
            switch (static_cast<ww::data::RequirementKind>(r.kind))
            {
                case ww::data::RequirementKind::Skill:
                    if (out.skillLevel(r.id) < r.amount) { out.setSkillLevel(r.id, r.amount); }
                    break;
                case ww::data::RequirementKind::Item:
                    if (out.itemCount(r.id) < r.amount) { out.setItemCount(r.id, r.amount); }
                    break;
                case ww::data::RequirementKind::Varbit:
                    out.setVarbit(r.id, r.amount);
                    break;
                case ww::data::RequirementKind::Varp:
                    out.setVarp(r.id, r.amount);
                    break;
            }
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
        buildPermissiveSnapshot(reader, snapshot);

        const auto txs = reader.transitions();
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
            if (!resolveApproach(view, tx.originX, tx.originY, plane, approachX, approachY))
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
