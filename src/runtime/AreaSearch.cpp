#include "runtime/AreaSearch.h"

#include "format/Artifact.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace ww::runtime
{
    namespace
    {
        // Orders the open vector as a binary min-heap on f-score: the std heap
        // functions surface the *greatest* element under the comparator, so
        // "greater priority" here means "worse", keeping the cheapest on top.
        struct ByPriority
        {
            bool operator()(const OpenEntry &a, const OpenEntry &b) const
            {
                return a.priority > b.priority;
            }
        };
    }

    AreaSearch::AreaSearch(const format::ArtifactReader &reader)
        : artifact(&reader),
          areaCount(static_cast<uint32_t>(reader.areaNodes().size())),
          heuristic(reader)
    {
        buildAdjacency();
        // Contents of the AoS scratch are gated by per-entry epochStamp, so
        // the initial values of the other fields are irrelevant — only the
        // epoch stamp itself must start at zero so the first query
        // (epoch == 1) sees every entry as stale. Zero-init the whole row
        // for cleanliness.
        scratch.assign(areaCount, AreaScratch{});
    }

    // Counting sort of the fromArea-sorted edge list into CSR offsets: edges are
    // grouped by ascending fromArea, so a prefix sum of per-area counts yields
    // each area's contiguous edge slice in the global edge array.
    void AreaSearch::buildAdjacency()
    {
        edgeOffset.assign(static_cast<std::size_t>(areaCount) + 1u, 0u);
        const std::span<const format::AreaEdgeRecord> edges = artifact->areaEdges();
        for (const format::AreaEdgeRecord &edge : edges)
        {
            if (isValidArea(edge.fromArea))
            {
                ++edgeOffset[static_cast<uint32_t>(edge.fromArea) + 1u];
            }
        }
        for (uint32_t a = 0; a < areaCount; ++a)
        {
            edgeOffset[a + 1u] += edgeOffset[a];
        }
    }

    void AreaSearch::resetScratch()
    {
        // Bump the epoch so every stale entry in scratch reads as unset
        // (via bestCostOf / isSettled). On wraparound (UINT32_MAX -> 0) the
        // stamps would collide with a fresh epoch == 0, so zero the stamps
        // and restart at epoch == 1. Wraparound never happens in practice
        // (4 billion queries on one AreaSearch) but the branch is one
        // compare and keeps correctness independent of usage.
        ++epoch;
        if (epoch == 0u)
        {
            for (AreaScratch &s : scratch)
            {
                s.epochStamp = 0u;
            }
            epoch = 1u;
        }
        openHeap.clear();
    }

    void AreaSearch::relaxOpen(int32_t u, std::span<const format::AreaEdgeRecord> edges,
                                const AltHeuristic &h)
    {
        const uint32_t ua = static_cast<uint32_t>(u);
        // u was just popped from the heap, so its bestCost is by construction
        // current — read scratch[ua].bestCost directly without the bestCostOf
        // gate. Stale-vs-current is only ambiguous for unvisited neighbours.
        const float uCost = scratch[ua].bestCost;
        for (uint32_t i = edgeOffset[ua]; i < edgeOffset[ua + 1u]; ++i)
        {
            const int32_t v = edges[i].toArea;
            if (!isValidArea(v))
            {
                continue;
            }
            const float nd = uCost + edges[i].cost;
            const uint32_t vu = static_cast<uint32_t>(v);
            if (nd >= bestCostOf(vu))
            {
                continue;
            }
            AreaScratch &dst = scratch[vu];
            dst.bestCost = nd;
            dst.cameFromArea = u;
            dst.cameFromEdge = static_cast<int32_t>(i);
            dst.settled = 0u;
            dst.epochStamp = epoch;
            openHeap.push_back({nd + h.estimate(vu), v});
            std::push_heap(openHeap.begin(), openHeap.end(), ByPriority{});
        }
    }

    void AreaSearch::relaxFiltered(int32_t u, std::span<const format::AreaEdgeRecord> edges,
                                    const AltHeuristic &h)
    {
        const uint32_t ua = static_cast<uint32_t>(u);
        const float uCost = scratch[ua].bestCost;
        for (uint32_t i = edgeOffset[ua]; i < edgeOffset[ua + 1u]; ++i)
        {
            const int32_t v = edges[i].toArea;
            if (!isValidArea(v))
            {
                continue;
            }
            if (!meetsTransitionRequirements(edges[i].transitionIndex))
            {
                continue;
            }
            const float nd = uCost + edges[i].cost;
            const uint32_t vu = static_cast<uint32_t>(v);
            if (nd >= bestCostOf(vu))
            {
                continue;
            }
            AreaScratch &dst = scratch[vu];
            dst.bestCost = nd;
            dst.cameFromArea = u;
            dst.cameFromEdge = static_cast<int32_t>(i);
            dst.settled = 0u;
            dst.epochStamp = epoch;
            openHeap.push_back({nd + h.estimate(vu), v});
            std::push_heap(openHeap.begin(), openHeap.end(), ByPriority{});
        }
    }

    // Look up the edge's TransitionRecord and require every RequirementRecord in
    // its run to be satisfied. A null snapshot accepts all edges; a malformed
    // record range or bad transitionIndex is treated as failure so the search
    // never relaxes an edge whose gate cannot be evaluated.
    bool AreaSearch::meetsTransitionRequirements(uint32_t transitionIndex) const
    {
        if (currentSnapshot == nullptr)
        {
            return true;
        }
        const std::span<const format::TransitionRecord> transitions = artifact->transitions();
        if (transitionIndex >= transitions.size())
        {
            return false;
        }
        const format::TransitionRecord &tx = transitions[transitionIndex];
        if (tx.requirementCount == 0u)
        {
            return true;
        }
        const std::span<const format::RequirementRecord> reqs = artifact->requirements();
        const uint64_t end = static_cast<uint64_t>(tx.requirementStart) + tx.requirementCount;
        if (end > reqs.size())
        {
            return false;
        }
        return meetsRequirements(currentSnapshot,
                                 reqs.subspan(tx.requirementStart, tx.requirementCount),
                                 static_cast<data::TransitionKind>(tx.kind));
    }

    // Push every valid FrontierSeed onto the open heap as an alternative entry
    // to startArea: arrive in destArea at seed.cost (with cameFromArea sentinel
    // -2 marking "via teleport" and cameFromEdge holding the seed's transition
    // index so reconstruct can recover the leading teleport). A seed costing at
    // least as much as the current bestCost is ignored — walking already wins,
    // or another seed already dominates this destination.
    //
    // Defensive re-validation: the caller (PathAssembler::buildGlobalTeleportSeeds)
    // already filters for kTransitionFlagGlobalOrigin and capability requirements,
    // but we re-check both here so a future caller that hand-rolls seeds cannot
    // bypass the gate. The cost of a per-seed bounds + flag + requirement check
    // is negligible against the search itself, and it eliminates a class of
    // "seeded unauthorized teleport" bugs at the boundary.
    void AreaSearch::seedFrontier(std::span<const FrontierSeed> seeds,
                                  const AltHeuristic &h)
    {
        const std::span<const format::TransitionRecord> transitions = artifact->transitions();
        for (const FrontierSeed &seed : seeds)
        {
            if (!isValidArea(seed.destArea))
            {
                continue;
            }
            if (seed.transitionIndex >= transitions.size())
            {
                continue;
            }
            const format::TransitionRecord &tx = transitions[seed.transitionIndex];
            if ((tx.flags & format::kTransitionFlagGlobalOrigin) == 0u)
            {
                continue;
            }
            if (!meetsTransitionRequirements(seed.transitionIndex))
            {
                continue;
            }
            const uint32_t a = static_cast<uint32_t>(seed.destArea);
            if (seed.cost >= bestCostOf(a))
            {
                continue;
            }
            AreaScratch &dst = scratch[a];
            dst.bestCost = seed.cost;
            dst.cameFromArea = -2;
            dst.cameFromEdge = static_cast<int32_t>(seed.transitionIndex);
            dst.settled = 0u;
            dst.epochStamp = epoch;
            openHeap.push_back({seed.cost + h.estimate(a), seed.destArea});
            std::push_heap(openHeap.begin(), openHeap.end(), ByPriority{});
        }
    }

    // Trace the predecessor chain from goal back to either startArea (normal
    // walk-out path) or a frontier-seeded destination (cameFromArea sentinel
    // -2; the chain terminates there and outPath.leadingTransition captures the
    // seed's transition index). The reversed steps front the route's entry area
    // — startArea or the teleport's destArea — so PathAssembler can drive its
    // cursor from a single uniform front-to-back iteration.
    void AreaSearch::reconstruct(int32_t startArea, int32_t goalArea, AreaPath &outPath) const
    {
        outPath.cost = scratch[static_cast<uint32_t>(goalArea)].bestCost;
        int32_t area = goalArea;
        while (area != startArea)
        {
            const AreaScratch &s = scratch[static_cast<uint32_t>(area)];
            const int32_t prev = s.cameFromArea;
            if (prev == -2)
            {
                outPath.leadingTransition = s.cameFromEdge;
                outPath.steps.push_back({area, -1});
                std::reverse(outPath.steps.begin(), outPath.steps.end());
                return;
            }
            outPath.steps.push_back({area, s.cameFromEdge});
            area = prev;
        }
        outPath.steps.push_back({startArea, -1});
        std::reverse(outPath.steps.begin(), outPath.steps.end());
    }

    bool AreaSearch::findPath(int32_t startArea, int32_t goalArea, AreaPath &outPath)
    {
        return findPath(startArea, goalArea, nullptr, {}, outPath);
    }

    bool AreaSearch::findPath(int32_t startArea, int32_t goalArea,
                              const CapabilitySnapshot *capabilities, AreaPath &outPath)
    {
        return findPath(startArea, goalArea, capabilities, {}, outPath);
    }

    bool AreaSearch::findPath(int32_t startArea, int32_t goalArea,
                              const CapabilitySnapshot *capabilities,
                              std::span<const FrontierSeed> seeds, AreaPath &outPath)
    {
        outPath.steps.clear();
        outPath.cost = 0.0f;
        outPath.leadingTransition = -1;
        currentSnapshot = capabilities;
        if (!isValidArea(startArea) || !isValidArea(goalArea))
        {
            return false;
        }
        if (startArea == goalArea)
        {
            outPath.steps.push_back({startArea, -1});
            return true;
        }

        const std::span<const format::AreaEdgeRecord> edges = artifact->areaEdges();
        heuristic.prepare(static_cast<uint32_t>(goalArea));
        resetScratch();

        const uint32_t startU = static_cast<uint32_t>(startArea);
        AreaScratch &startSlot = scratch[startU];
        startSlot.bestCost = 0.0f;
        startSlot.cameFromArea = -1;
        startSlot.cameFromEdge = -1;
        startSlot.settled = 0u;
        startSlot.epochStamp = epoch;
        openHeap.push_back({heuristic.estimate(startU), startArea});
        seedFrontier(seeds, heuristic);
        // Specialise the inner loop on the null-snapshot case so an
        // unfiltered query (bench, same-area baseline) skips the per-edge
        // requirement check entirely. relax{Open,Filtered} are otherwise
        // byte-for-byte identical, so tie-breaking is preserved.
        const bool unfiltered = (currentSnapshot == nullptr);
        while (!openHeap.empty())
        {
            std::pop_heap(openHeap.begin(), openHeap.end(), ByPriority{});
            const int32_t u = openHeap.back().area;
            openHeap.pop_back();
            const uint32_t uu = static_cast<uint32_t>(u);
            if (isSettled(uu))
            {
                continue;
            }
            AreaScratch &uSlot = scratch[uu];
            uSlot.settled = 1u;
            uSlot.epochStamp = epoch;
            if (u == goalArea)
            {
                reconstruct(startArea, goalArea, outPath);
                return true;
            }
            if (unfiltered)
            {
                relaxOpen(u, edges, heuristic);
            }
            else
            {
                relaxFiltered(u, edges, heuristic);
            }
        }
        return false;
    }
}
