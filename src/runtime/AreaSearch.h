#ifndef WORLDWALKER_RUNTIME_AREASEARCH_H
#define WORLDWALKER_RUNTIME_AREASEARCH_H

#include "format/ArtifactReader.h"
#include "runtime/AltHeuristic.h"
#include "runtime/CapabilitySnapshot.h"

#include <cstdint>
#include <limits>
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
    // found. leadingTransition is -1 for routes that walk out of the start area;
    // otherwise it is the artifact-relative TransitionRecord index of a global-
    // origin teleport seeded at the search frontier — the route then begins at
    // that transition's destination area (the steps.front() entry), and the
    // teleport's own cost is already folded into AreaPath.cost.
    struct AreaPath
    {
        std::vector<AreaPathStep> steps;
        float cost{};
        int32_t leadingTransition{-1};
    };

    // One global-origin teleport made available at the search frontier of a
    // findPath call: arrive in `destArea` for `cost` ticks by executing the
    // transition at `transitionIndex` (artifact-relative). The caller is
    // responsible for the upstream filtering — teleport-allowed at the start
    // tile, capability requirements satisfied — so the search itself simply
    // treats each seed as a candidate alternative to walking out of startArea.
    struct FrontierSeed
    {
        int32_t  destArea;
        float    cost;
        uint32_t transitionIndex;
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
        // The two-arg overload accepts every transition and seeds nothing; the
        // three-arg overload additionally filters edges whose underlying
        // TransitionRecord has Requirements the borrowed CapabilitySnapshot does
        // not satisfy (nullptr is equivalent to the two-arg overload); the four-
        // arg overload additionally seeds the open set with one alternative entry
        // per FrontierSeed at its declared cost, so a teleport that lands closer
        // to goal can beat walking out of startArea. The seeds span is borrowed
        // for the call only and may be empty.
        bool findPath(int32_t startArea, int32_t goalArea, AreaPath &outPath);
        bool findPath(int32_t startArea, int32_t goalArea,
                      const CapabilitySnapshot *capabilities, AreaPath &outPath);
        bool findPath(int32_t startArea, int32_t goalArea,
                      const CapabilitySnapshot *capabilities,
                      std::span<const FrontierSeed> seeds, AreaPath &outPath);

    private:
        void buildAdjacency();
        void resetScratch();
        // Each helper takes its heuristic by ref so the implementation in
        // .cpp can name the parameter `h` and not collide with the member.
        // (No name-hiding warning under /W4 /WX.)
        //
        // Two specialisations of relax — the unfiltered variant skips the
        // per-edge meetsTransitionRequirements check entirely, removing a
        // predictable branch from the inner loop on the bench path
        // (ww_query without a CapabilitySnapshot, the same-area baseline).
        void relaxOpen(int32_t u, std::span<const format::AreaEdgeRecord> edges,
                       const AltHeuristic &h);
        void relaxFiltered(int32_t u, std::span<const format::AreaEdgeRecord> edges,
                           const AltHeuristic &h);
        void seedFrontier(std::span<const FrontierSeed> seeds, const AltHeuristic &h);
        void reconstruct(int32_t startArea, int32_t goalArea, AreaPath &outPath) const;
        bool meetsTransitionRequirements(uint32_t transitionIndex) const;

        bool isValidArea(int32_t area) const
        {
            return area >= 0 && static_cast<uint32_t>(area) < areaCount;
        }

        // Per-area scratch packed into a single 20-byte record so a relaxation
        // touches one cache line instead of five. Live iff `epochStamp` ==
        // current epoch — a generation-counter trick so resetScratch is O(1)
        // (bump epoch + clear openHeap) instead of std::fills over the full
        // areaCount every query. bestCostOf/isSettled read INF / false on a
        // stale stamp; writes set the stamp to the current epoch so subsequent
        // reads see the new value. Wraparound (epoch hits UINT32_MAX) is
        // handled by zeroing all epochStamp fields and restarting at epoch = 1.
        struct AreaScratch
        {
            float    bestCost;      // g-score (live iff epochStamp == epoch)
            int32_t  cameFromArea;  // predecessor area (-1 unset, -2 frontier-seeded)
            int32_t  cameFromEdge;  // AreaEdge index entered through; or, when cameFromArea==-2, the seed's transitionIndex
            uint32_t epochStamp;    // generation tag — fields above are stale unless == epoch
            uint8_t  settled;       // closed-set flag (live iff epochStamp == epoch)
            uint8_t  pad[3];        // zero-filled
        };
        static_assert(sizeof(AreaScratch) == 20,
                      "AreaScratch must be 20 bytes (one cache line per relaxation)");

        float bestCostOf(uint32_t a) const
        {
            return scratch[a].epochStamp == epoch
                       ? scratch[a].bestCost
                       : std::numeric_limits<float>::infinity();
        }
        bool isSettled(uint32_t a) const
        {
            return scratch[a].epochStamp == epoch && scratch[a].settled != 0u;
        }

        const format::ArtifactReader *artifact;
        uint32_t areaCount;
        std::vector<uint32_t> edgeOffset;   // CSR: area a's edges are [edgeOffset[a], edgeOffset[a+1])
        std::vector<AreaScratch> scratch;   // per-area packed scratch (live iff epochStamp == epoch)
        uint32_t epoch{0};                  // bumped each findPath; entries with epochStamp != epoch read as "unset"
        std::vector<OpenEntry> openHeap;    // binary min-heap of the open set (scratch)
        AltHeuristic heuristic;             // landmark bound; owns its per-query goal-distance scratch
        const CapabilitySnapshot *currentSnapshot{nullptr};  // borrowed for one findPath; nullptr accepts all edges
    };
}

#endif  // WORLDWALKER_RUNTIME_AREASEARCH_H
