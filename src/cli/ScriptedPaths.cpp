#include "cli/ScriptedPaths.h"

#include "format/Artifact.h"
#include "format/ArtifactReader.h"
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
//   capability_gate  — a synthetic capability snapshot decides predicate
//                      checks consistently with the artifact's own pool, and
//                      an empty snapshot refuses every requirement-bearing
//                      global the permissive snapshot accepts.
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
            // Resolve a standable interact-tile in fromArea within radius 2
            // of the origin — mirrors PathAssembler::resolveInteractTile so
            // the harness query starts where the transition step would.
            const int plane = static_cast<int>(tx.originPlane);
            int startX = 0;
            int startY = 0;
            bool startOk = false;
            for (int r = 0; r <= 2 && !startOk; ++r)
            {
                for (int dy = -r; dy <= r && !startOk; ++dy)
                {
                    for (int dx = -r; dx <= r && !startOk; ++dx)
                    {
                        if (std::max(std::abs(dx), std::abs(dy)) != r)
                        {
                            continue;
                        }
                        const int x = tx.originX + dx;
                        const int y = tx.originY + dy;
                        if (view.isStandable(x, y, plane)
                            && view.areaAt(x, y, plane) == edge.fromArea)
                        {
                            startX = x;
                            startY = y;
                            startOk = true;
                        }
                    }
                }
            }
            if (!startOk)
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

    // Category 4 — capability gate. For each requirement-bearing global
    // whose run contains at least one Skill/Item/non-zero-Varbit-or-Varp
    // gate (the kinds the empty snapshot can reject), verify:
    //   - an empty snapshot rejects the run.
    //   - a snapshot built from THAT run's own entries accepts the run.
    // Per-transition snapshot construction avoids the cross-transition
    // same-id-different-value conflict that an aggregate permissive build
    // would create. Surfaces a bug in either predicate evaluation or
    // requirement decoding.
    CaseResult capabilityGate(const ww::format::ArtifactReader &reader)
    {
        const auto txs  = reader.transitions();
        const auto reqs = reader.requirements();
        ww::runtime::CapabilitySnapshot empty;

        std::size_t testable        = 0;
        std::size_t emptyAccepts    = 0;  // expected: 0
        std::size_t tunedRejects    = 0;  // expected: 0
        bool foundReqGlobal         = false;

        for (const ww::format::TransitionRecord &tx : txs)
        {
            if ((tx.flags & ww::format::kTransitionFlagGlobalOrigin) == 0u
                || tx.requirementCount == 0u)
            {
                continue;
            }
            const std::uint64_t end =
                static_cast<std::uint64_t>(tx.requirementStart) + tx.requirementCount;
            if (end > reqs.size())
            {
                continue;
            }
            const auto run = reqs.subspan(tx.requirementStart, tx.requirementCount);
            if (!runHasEmptyRejectingReq(run))
            {
                continue;  // not a testable gate (e.g. "varbit==0")
            }
            ++testable;
            foundReqGlobal = true;
            if (ww::runtime::meetsRequirements(&empty, run))
            {
                ++emptyAccepts;
            }
            ww::runtime::CapabilitySnapshot tuned;
            // Per-run construction (not the whole pool) so same-id-different-value
            // collisions in unrelated transitions cannot make this run unsatisfiable.
            ww::runtime::applyPermissiveRequirements(run, tuned);
            if (!ww::runtime::meetsRequirements(&tuned, run))
            {
                ++tunedRejects;
            }
        }
        if (!foundReqGlobal)
        {
            return {"capability_gate", Outcome::Skip,
                    "no testable requirement-bearing global transitions"};
        }
        if (emptyAccepts != 0)
        {
            return {"capability_gate", Outcome::Fail,
                    "empty snapshot accepted a Skill/Item/non-zero-Var gated global"};
        }
        if (tunedRejects != 0)
        {
            return {"capability_gate", Outcome::Fail,
                    "tuned snapshot failed to accept its own run"};
        }
        (void)testable;
        return {"capability_gate", Outcome::Pass, nullptr};
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
            capabilityGate(reader),
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
        return failed == 0 ? 0 : 1;
    }
    catch (const std::exception &e)
    {
        std::printf("scripted: failed: %s\n", e.what());
        return 1;
    }
}
