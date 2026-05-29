#ifndef WORLDWALKER_RUNTIME_AREASEARCH_H
#define WORLDWALKER_RUNTIME_AREASEARCH_H

#include "format/ArtifactReader.h"
#include "runtime/AltHeuristic.h"

#include <cstdint>
#include <span>
#include <vector>

namespace ww::runtime
{
    // One hop of an area route: the area entered, and the artifact AreaEdge index
    // traversed to enter it. viaEdge is -1 for the start area (no edge).
    struct AreaPathStep
    {
        int32_t area;
        int32_t viaEdge;
    };

    // An ordered area-level route: areas from start (front) to goal (back), and
    // the total traversed transition tick cost. steps is empty when no route was
    // found.
    struct AreaPath
    {
        std::vector<AreaPathStep> steps;
        float cost{};
    };

    // A priority-queue entry: an area keyed by its A* f-score. Namespace-scope so
    // the .cpp's heap comparator can name it.
    struct OpenEntry
    {
        float priority;
        int32_t area;
    };

    // ALT-guided A* over the baked directed area graph (AreaNodeRecord +
    // AreaEdgeRecord). Produces the area-level skeleton that a later tile-level
    // refinement pass walks within. The cost metric is the sum of traversed
    // AreaEdge transition costs; intra-area walk cost is deliberately omitted so
    // the search stays consistent with the ALT tables (which omit it too) and is
    // added back during tile refinement.
    //
    // One instance per search context (ADR 0007): the constructor builds a
    // compressed (CSR) adjacency index over the immutable edge list once, then
    // findPath reuses per-query scratch across calls. Not thread-safe; the
    // borrowed reader must outlive the search.
    class AreaSearch
    {
    public:
        explicit AreaSearch(const format::ArtifactReader &reader);

        AreaSearch(const AreaSearch &) = delete;
        AreaSearch &operator=(const AreaSearch &) = delete;
        AreaSearch(AreaSearch &&) = default;
        AreaSearch &operator=(AreaSearch &&) = default;

        // Least-cost area route from startArea to goalArea. Returns false (and
        // leaves outPath.steps empty) when either id is out of range or no route
        // exists; a start == goal query yields a single-step path at cost 0.
        bool findPath(int32_t startArea, int32_t goalArea, AreaPath &outPath);

    private:
        void buildAdjacency();
        void resetScratch();
        void relax(int32_t u, std::span<const format::AreaEdgeRecord> edges,
                   const AltHeuristic &heuristic);
        void reconstruct(int32_t startArea, int32_t goalArea, AreaPath &outPath) const;

        bool isValidArea(int32_t area) const
        {
            return area >= 0 && static_cast<uint32_t>(area) < areaCount;
        }

        const format::ArtifactReader *artifact;
        uint32_t areaCount;
        std::vector<uint32_t> edgeOffset;   // CSR: area a's edges are [edgeOffset[a], edgeOffset[a+1])
        std::vector<float> bestCost;        // g-score per area (scratch)
        std::vector<int32_t> cameFromArea;  // predecessor area (scratch)
        std::vector<int32_t> cameFromEdge;  // AreaEdge index entered through (scratch)
        std::vector<uint8_t> settled;       // closed-set flag (scratch)
        std::vector<OpenEntry> openHeap;    // binary min-heap of the open set (scratch)
    };
}

#endif  // WORLDWALKER_RUNTIME_AREASEARCH_H
