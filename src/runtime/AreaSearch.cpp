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
          areaCount(static_cast<uint32_t>(reader.areaNodes().size()))
    {
        buildAdjacency();
        bestCost.assign(areaCount, 0.0f);
        cameFromArea.assign(areaCount, -1);
        cameFromEdge.assign(areaCount, -1);
        settled.assign(areaCount, 0u);
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
        std::fill(bestCost.begin(), bestCost.end(), std::numeric_limits<float>::infinity());
        std::fill(cameFromArea.begin(), cameFromArea.end(), int32_t{-1});
        std::fill(cameFromEdge.begin(), cameFromEdge.end(), int32_t{-1});
        std::fill(settled.begin(), settled.end(), uint8_t{0});
        openHeap.clear();
    }

    void AreaSearch::relax(int32_t u, std::span<const format::AreaEdgeRecord> edges,
                           const AltHeuristic &heuristic)
    {
        const uint32_t ua = static_cast<uint32_t>(u);
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
            const float nd = bestCost[ua] + edges[i].cost;
            if (nd >= bestCost[static_cast<uint32_t>(v)])
            {
                continue;
            }
            bestCost[static_cast<uint32_t>(v)] = nd;
            cameFromArea[static_cast<uint32_t>(v)] = u;
            cameFromEdge[static_cast<uint32_t>(v)] = static_cast<int32_t>(i);
            openHeap.push_back({nd + heuristic.estimate(static_cast<uint32_t>(v)), v});
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
        for (uint32_t r = tx.requirementStart; r < end; ++r)
        {
            if (!currentSnapshot->meets(reqs[r]))
            {
                return false;
            }
        }
        return true;
    }

    void AreaSearch::reconstruct(int32_t startArea, int32_t goalArea, AreaPath &outPath) const
    {
        outPath.cost = bestCost[static_cast<uint32_t>(goalArea)];
        int32_t area = goalArea;
        while (area != startArea)
        {
            outPath.steps.push_back({area, cameFromEdge[static_cast<uint32_t>(area)]});
            area = cameFromArea[static_cast<uint32_t>(area)];
        }
        outPath.steps.push_back({startArea, -1});
        std::reverse(outPath.steps.begin(), outPath.steps.end());
    }

    bool AreaSearch::findPath(int32_t startArea, int32_t goalArea, AreaPath &outPath)
    {
        return findPath(startArea, goalArea, nullptr, outPath);
    }

    bool AreaSearch::findPath(int32_t startArea, int32_t goalArea,
                              const CapabilitySnapshot *capabilities, AreaPath &outPath)
    {
        outPath.steps.clear();
        outPath.cost = 0.0f;
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
        const AltHeuristic heuristic(*artifact, static_cast<uint32_t>(goalArea));
        resetScratch();

        bestCost[static_cast<uint32_t>(startArea)] = 0.0f;
        openHeap.push_back({heuristic.estimate(static_cast<uint32_t>(startArea)), startArea});
        while (!openHeap.empty())
        {
            std::pop_heap(openHeap.begin(), openHeap.end(), ByPriority{});
            const int32_t u = openHeap.back().area;
            openHeap.pop_back();
            if (settled[static_cast<uint32_t>(u)] != 0)
            {
                continue;
            }
            settled[static_cast<uint32_t>(u)] = 1;
            if (u == goalArea)
            {
                reconstruct(startArea, goalArea, outPath);
                return true;
            }
            relax(u, edges, heuristic);
        }
        return false;
    }
}
