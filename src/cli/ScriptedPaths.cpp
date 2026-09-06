#include "cli/ScriptedPaths.h"

#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"
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
#include <vector>

// Phase 6c — Scripted path correctness.
//
// The plan calls for canonical routes (Lumbridge→Varrock, ladder/stair,
// teleport-then-walk, F2P-gated teleport). Because the artifact under test
// is cut from whichever local cache snapshot the developer ran wwbuild
// against, world coordinates for landmarks vary; instead of pinning specific
// coords, this harness discovers a representative case per category from the
// artifact's own indices. Each case verifies the path-correctness property
// the original named route was supposed to prove:
//
//   long_walk        — a multi-hop walk-only plan stays in walking steps and
//                      every step lands on a standable tile.
//   plane_change     — when a ladder/stair-like area edge exists, the plan
//                      uses the transition's destination plane.
//   teleport_seeded  — when a global teleport dominates walking from the
//                      start area, the leading plan step is that transition.
//   capability_gate  — a requirement-bearing transition appears in the plan
//                      assembled with a snapshot tuned to its own requirement
//                      run, and does not appear in the plan assembled with an
//                      empty one.
namespace
{
    enum class Outcome { Pass, Fail, Skip };

    const char *outcomeTag(Outcome o)
    {
        switch (o)
        {
            case Outcome::Pass: return "PASS";
            case Outcome::Fail: return "FAIL";
            case Outcome::Skip: return "SKIP";
        }
        return "?";
    }

    struct CaseResult
    {
        const char *name;
        Outcome     outcome;
        const char *detail;     // optional one-line trailer
    };

    // Pick an in-area refinement target as far as possible from the centroid
    // within `radius`. Mirrors wwcli's existing `farthestInArea` helper so
    // 6c doesn't break if the choice ever changes; kept local to this TU.
    struct TilePoint { std::int32_t x; std::int32_t y; };

    TilePoint farthestInArea(ww::runtime::WorldView &view, int cx, int cy, int plane,
                             int area, int radius)
    {
        TilePoint best{cx, cy};
        int bestDist = 0;
        for (int dx = -radius; dx <= radius; ++dx)
        {
            for (int dy = -radius; dy <= radius; ++dy)
            {
                const int x = cx + dx;
                const int y = cy + dy;
                const int dist = std::max(std::abs(dx), std::abs(dy));
                if (dist > bestDist && view.isStandable(x, y, plane)
                    && view.areaAt(x, y, plane) == area)
                {
                    bestDist = dist;
                    best = {x, y};
                }
            }
        }
        return best;
    }

    // Category 1 — long walk. From the first area node's centroid, walk to
    // the farthest in-area standable tile inside a 24-tile radius. Asserts
    // the plan is all-walk and every step lands on a standable tile (the
    // assembler's contiguity is already verified by 6a/wwcli's main report,
    // so we re-check standability here as the 6c-specific guard).
    CaseResult longWalk(const ww::format::ArtifactReader &reader,
                        ww::runtime::WorldView &view,
                        ww::runtime::PathAssembler &assembler)
    {
        const auto nodes = reader.areaNodes();
        if (nodes.empty())
        {
            return {"long_walk", Outcome::Skip, "no area nodes"};
        }
        const ww::format::AreaNodeRecord &n0 = nodes[0];
        const int plane = static_cast<int>(n0.plane);
        const int area  = view.areaAt(n0.centroidX, n0.centroidY, plane);
        const TilePoint goal = farthestInArea(view, n0.centroidX, n0.centroidY, plane, area, 24);
        if (goal.x == n0.centroidX && goal.y == n0.centroidY)
        {
            return {"long_walk", Outcome::Skip, "no farther in-area tile within radius"};
        }
        ww::runtime::Plan plan;
        if (!assembler.assemble(n0.centroidX, n0.centroidY, plane, goal.x, goal.y, plane, plan))
        {
            return {"long_walk", Outcome::Fail, "assemble returned false"};
        }
        for (const ww::runtime::Step &s : plan.steps)
        {
            if (s.kind != ww::runtime::StepKind::Walk)
            {
                return {"long_walk", Outcome::Fail, "plan contains a transition step"};
            }
            if (!view.isStandable(s.targetX, s.targetY, static_cast<int>(s.plane)))
            {
                return {"long_walk", Outcome::Fail, "step lands on non-standable tile"};
            }
        }
        return {"long_walk", Outcome::Pass, nullptr};
    }

    // Category 2 — plane change. Find the first traversable area edge whose
    // underlying transition has originPlane != destPlane. Run a plan from a
    // standable interact-tile in fromArea to the transition's destination
    // and assert the plan visits both planes (i.e. the destination plane
    // appears in the step list).
    CaseResult planeChange(const ww::format::ArtifactReader &reader,
                           ww::runtime::WorldView &view,
                           ww::runtime::PathAssembler &assembler)
    {
        const auto edges = reader.areaEdges();
        const auto txs   = reader.transitions();
        const auto nodes = reader.areaNodes();
        for (const ww::format::AreaEdgeRecord &edge : edges)
        {
            if (edge.transitionIndex >= txs.size())
            {
                continue;
            }
            const ww::format::TransitionRecord &tx = txs[edge.transitionIndex];
            if (tx.originPlane == tx.destPlane)
            {
                continue;
            }
            if (static_cast<std::size_t>(edge.fromArea) >= nodes.size()
                || static_cast<std::size_t>(edge.toArea) >= nodes.size()
                || tx.requirementCount != 0u)
            {
                continue;
            }
            // Resolve a standable interact-tile in fromArea within radius 2 of
            // the origin through the same production scan
            // PathAssembler::resolveInteractTile uses, so the harness query
            // starts exactly where the transition step would.
            const int plane = static_cast<int>(tx.originPlane);
            std::int32_t startX = 0;
            std::int32_t startY = 0;
            const auto standableInArea = [&](std::int32_t x, std::int32_t y)
            {
                return view.isStandable(x, y, plane) && view.areaAt(x, y, plane) == edge.fromArea;
            };
            if (!ww::runtime::findNearestTile(tx.originX, tx.originY,
                                              ww::data::kTransitionApproachRadius, true,
                                              standableInArea, tx.originX, tx.originY,
                                              startX, startY))
            {
                continue;
            }
            ww::runtime::Plan plan;
            if (!assembler.assemble(startX, startY, plane, tx.destX, tx.destY,
                                    static_cast<int>(tx.destPlane), plan))
            {
                continue;
            }
            // Plan steps store the plane of each step's *target*; a
            // Transition step's target is the interact-tile on the origin
            // plane, and the chain itself moves the player to the dest
            // plane — there is no separate "you're now on dest plane" walk
            // step. So the plane-change property is "the plan must include
            // at least one Transition step" (a walk can't cross planes).
            bool hasTransitionStep = false;
            for (const ww::runtime::Step &s : plan.steps)
            {
                if (s.kind == ww::runtime::StepKind::Transition)
                {
                    hasTransitionStep = true;
                    break;
                }
            }
            return hasTransitionStep
                ? CaseResult{"plane_change", Outcome::Pass, nullptr}
                : CaseResult{"plane_change", Outcome::Fail,
                             "cross-plane plan has no Transition step"};
        }
        return {"plane_change", Outcome::Skip, "no traversable cross-plane edge"};
    }

    // Category 3 — teleport-seeded leading step. Walk from area[0]'s centroid
    // to a destination that's only cheaply reachable via a permissive global
    // teleport. Asserts the plan's first step is a Transition with the
    // global-origin flag set.
    CaseResult teleportSeeded(const ww::format::ArtifactReader &reader,
                              ww::runtime::WorldView &view,
                              ww::runtime::AreaSearch &areaSearch,
                              ww::runtime::PathAssembler &assembler)
    {
        const auto nodes = reader.areaNodes();
        if (nodes.empty())
        {
            return {"teleport_seeded", Outcome::Skip, "no area nodes"};
        }
        ww::runtime::CapabilitySnapshot permissive;
        ww::runtime::applyPermissiveRequirements(reader.requirements(), permissive);

        const ww::format::AreaNodeRecord &n0 = nodes[0];
        const int startPlane = static_cast<int>(n0.plane);
        const int startArea  = view.areaAt(n0.centroidX, n0.centroidY, startPlane);
        if (startArea < 0)
        {
            return {"teleport_seeded", Outcome::Skip, "start tile not in any area"};
        }

        const auto txs  = reader.transitions();
        const auto reqs = reader.requirements();
        for (std::uint32_t i = 0; i < txs.size(); ++i)
        {
            const ww::format::TransitionRecord &tx = txs[i];
            if ((tx.flags & ww::format::kTransitionFlagGlobalOrigin) == 0u)
            {
                continue;
            }
            const std::uint64_t end =
                static_cast<std::uint64_t>(tx.requirementStart) + tx.requirementCount;
            if (end > reqs.size())
            {
                continue;
            }
            if (!ww::runtime::meetsRequirements(&permissive,
                                                reqs.subspan(tx.requirementStart,
                                                             tx.requirementCount)))
            {
                continue;
            }
            const int destArea =
                view.areaAt(tx.destX, tx.destY, static_cast<int>(tx.destPlane));
            if (destArea < 0)
            {
                continue;
            }
            ww::runtime::AreaPath walkProbe;
            const bool walkOk = areaSearch.findPath(startArea, destArea, walkProbe);
            if (walkOk && walkProbe.cost <= tx.cost)
            {
                continue;                                      // walking already dominates
            }
            ww::runtime::Plan plan;
            const bool ok = assembler.assemble(n0.centroidX, n0.centroidY, startPlane,
                                               tx.destX, tx.destY,
                                               static_cast<int>(tx.destPlane), &permissive,
                                               plan);
            if (!ok || plan.steps.empty())
            {
                continue;
            }
            const ww::runtime::Step &lead = plan.steps.front();
            if (lead.kind != ww::runtime::StepKind::Transition)
            {
                return {"teleport_seeded", Outcome::Fail,
                        "leading step is a walk, not a transition"};
            }
            // The invariant is "the leading step is a global-origin
            // Transition", not "the leading step is *this specific*
            // transition" — when several globals dominate walking, the
            // assembler is free to pick the cheapest, and any of them
            // satisfies the property. Asserting transitionIndex == i would
            // false-FAIL on ties.
            if (lead.transitionIndex >= txs.size())
            {
                return {"teleport_seeded", Outcome::Fail,
                        "leading transition index out of range"};
            }
            const ww::format::TransitionRecord &leadTx = txs[lead.transitionIndex];
            if ((leadTx.flags & ww::format::kTransitionFlagGlobalOrigin) == 0u)
            {
                return {"teleport_seeded", Outcome::Fail,
                        "leading Transition is not global-origin"};
            }
            return {"teleport_seeded", Outcome::Pass, nullptr};
        }
        return {"teleport_seeded", Outcome::Skip, "no global dominates walking"};
    }

    // True when the run contains at least one Skill or Item requirement —
    // the categories where the empty (all-zero) snapshot reliably rejects.
    // Varbit / Varp requirements use exact equality, so a req like
    // "varbit X == 0" is accepted by an empty snapshot (varbit defaults to
    // 0); we exclude pure-varbit/varp runs from the empty-rejection
    // assertion so the test reports its invariant truthfully.
    bool runHasEmptyRejectingReq(std::span<const ww::format::RequirementRecord> run)
    {
        for (const ww::format::RequirementRecord &r : run)
        {
            const auto kind = static_cast<ww::data::RequirementKind>(r.kind);
            if (kind == ww::data::RequirementKind::Skill && r.amount > 0)
            {
                return true;
            }
            if (kind == ww::data::RequirementKind::Item && r.amount > 0)
            {
                return true;
            }
            if ((kind == ww::data::RequirementKind::Varbit
                 || kind == ww::data::RequirementKind::Varp)
                && r.amount != 0)
            {
                // The empty snapshot reads 0; a non-zero exact match is
                // rejected. Zero-required varbit/varp gates are accepted
                // by the empty snapshot and so are excluded here.
                return true;
            }
        }
        return false;
    }

    // True when `plan` routes through transition `index`.
    bool planUsesTransition(const ww::runtime::Plan &plan, std::uint32_t index)
    {
        for (const ww::runtime::Step &s : plan.steps)
        {
            if (s.kind == ww::runtime::StepKind::Transition && s.transitionIndex == index)
            {
                return true;
            }
        }
        return false;
    }

    // A requirement-bearing transition the planner might route through, with the
    // query that would exercise it. For a global the query starts at area[0]'s
    // centroid (a global is usable from anywhere); for a local-origin edge it
    // starts on a standable tile beside the origin.
    struct GatedCandidate
    {
        std::uint32_t index{};
        int startX{};
        int startY{};
        int startPlane{};
    };

    // Fill `out` with the next requirement-bearing transition at or after
    // `cursor` whose gate an empty snapshot actually rejects, advancing the
    // cursor past it. Returns false once the pool is exhausted.
    bool nextGatedCandidate(const ww::format::ArtifactReader &reader,
                            ww::runtime::WorldView &view, int centroidX, int centroidY,
                            int centroidPlane, std::uint32_t &ioCursor, GatedCandidate &out)
    {
        const auto txs  = reader.transitions();
        const auto reqs = reader.requirements();
        for (; ioCursor < txs.size(); ++ioCursor)
        {
            const ww::format::TransitionRecord &tx = txs[ioCursor];
            const std::uint64_t end =
                static_cast<std::uint64_t>(tx.requirementStart) + tx.requirementCount;
            if (tx.requirementCount == 0u || end > reqs.size())
            {
                continue;
            }
            if (!runHasEmptyRejectingReq(reqs.subspan(tx.requirementStart, tx.requirementCount)))
            {
                continue;  // an empty snapshot would accept it; nothing to gate on
            }
            out.index = ioCursor;
            if ((tx.flags & ww::format::kTransitionFlagGlobalOrigin) != 0u)
            {
                out.startX     = centroidX;
                out.startY     = centroidY;
                out.startPlane = centroidPlane;
                ++ioCursor;
                return true;
            }
            const int plane = static_cast<int>(tx.originPlane);
            const int area  = view.areaAt(tx.destX, tx.destY, static_cast<int>(tx.destPlane));
            std::int32_t sx = 0;
            std::int32_t sy = 0;
            const auto standable = [&](std::int32_t x, std::int32_t y)
            {
                return view.isStandable(x, y, plane) && view.areaAt(x, y, plane) != area;
            };
            if (!ww::runtime::findNearestTile(tx.originX, tx.originY,
                                              ww::data::kTransitionApproachRadius, true, standable,
                                              tx.originX, tx.originY, sx, sy))
            {
                continue;
            }
            out.startX     = sx;
            out.startY     = sy;
            out.startPlane = plane;
            ++ioCursor;
            return true;
        }
        return false;
    }

    // Category 4 — capability gate, through the planner.
    //
    // The point is that a Requirement predicate changes what the PLANNER
    // returns, so the planner is what gets driven: the same query is assembled
    // twice, once with an empty snapshot and once with a snapshot tuned to the
    // candidate transition's own requirement run, and the gated transition must
    // appear in the tuned plan and not in the empty one. Asserting only that a
    // tuned snapshot satisfies the run it was built from would be a tautology
    // that never touches the planner at all — which is what this case used to do.
    //
    // The tuned snapshot is built per-run rather than from the whole pool so a
    // same-id-different-value collision in an unrelated transition cannot make
    // this run unsatisfiable.
    CaseResult capabilityGate(const ww::format::ArtifactReader &reader,
                              ww::runtime::WorldView &view,
                              ww::runtime::PathAssembler &assembler)
    {
        const auto nodes = reader.areaNodes();
        if (nodes.empty())
        {
            return {"capability_gate", Outcome::Skip, "no area nodes"};
        }
        const auto txs  = reader.transitions();
        const auto reqs = reader.requirements();
        const ww::format::AreaNodeRecord &n0 = nodes[0];

        ww::runtime::CapabilitySnapshot empty;
        std::uint32_t cursor = 0;
        GatedCandidate candidate{};
        while (nextGatedCandidate(reader, view, n0.centroidX, n0.centroidY,
                                  static_cast<int>(n0.plane), cursor, candidate))
        {
            const ww::format::TransitionRecord &tx = txs[candidate.index];
            ww::runtime::CapabilitySnapshot tuned;
            ww::runtime::applyPermissiveRequirements(
                reqs.subspan(tx.requirementStart, tx.requirementCount), tuned);

            ww::runtime::Plan tunedPlan;
            const bool tunedOk = assembler.assemble(candidate.startX, candidate.startY,
                                                    candidate.startPlane, tx.destX, tx.destY,
                                                    static_cast<int>(tx.destPlane), &tuned,
                                                    tunedPlan);
            if (!tunedOk || !planUsesTransition(tunedPlan, candidate.index))
            {
                continue;  // the planner has a cheaper route; not a decisive case
            }
            ww::runtime::Plan emptyPlan;
            const bool emptyOk = assembler.assemble(candidate.startX, candidate.startY,
                                                    candidate.startPlane, tx.destX, tx.destY,
                                                    static_cast<int>(tx.destPlane), &empty,
                                                    emptyPlan);
            if (emptyOk && planUsesTransition(emptyPlan, candidate.index))
            {
                return {"capability_gate", Outcome::Fail,
                        "empty snapshot still routed through a gated transition"};
            }
            return {"capability_gate", Outcome::Pass, nullptr};
        }
        return {"capability_gate", Outcome::Skip,
                "no gated transition the planner routes through"};
    }
}

int runScriptedPaths(const char *wwaPath)
{
    try
    {
        const ww::format::ArtifactReader reader(wwaPath);
        ww::runtime::WorldView    view(reader);
        ww::runtime::AreaSearch   areaSearch(reader);
        ww::runtime::TileSearch   tileSearch(view);
        ww::runtime::PathAssembler assembler(reader, view, areaSearch, tileSearch);

        std::printf("scripted: %s\n", wwaPath);
        const std::vector<CaseResult> results = {
            longWalk(reader, view, assembler),
            planeChange(reader, view, assembler),
            teleportSeeded(reader, view, areaSearch, assembler),
            capabilityGate(reader, view, assembler),
        };
        int passed  = 0;
        int failed  = 0;
        int skipped = 0;
        for (const CaseResult &r : results)
        {
            std::printf("scripted: %-18s %s", r.name, outcomeTag(r.outcome));
            if (r.detail != nullptr)
            {
                std::printf(" — %s", r.detail);
            }
            std::printf("\n");
            switch (r.outcome)
            {
                case Outcome::Pass: ++passed;  break;
                case Outcome::Fail: ++failed;  break;
                case Outcome::Skip: ++skipped; break;
            }
        }
        std::printf("scripted: total %d pass / %d fail / %d skip\n", passed, failed, skipped);
        // A run where every case skipped exits 0 but has verified nothing about
        // the planner. Say so loudly rather than letting a green exit code
        // stand in for coverage that does not exist.
        if (passed == 0 && failed == 0)
        {
            std::printf("scripted: *** NOTHING WAS TESTED — all %d case(s) skipped;"
                        " this artifact exercises none of them ***\n", skipped);
        }
        return failed == 0 ? 0 : 1;
    }
    catch (const std::exception &e)
    {
        std::printf("scripted: failed: %s\n", e.what());
        return 1;
    }
}
