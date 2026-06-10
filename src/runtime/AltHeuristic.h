#ifndef WORLDWALKER_RUNTIME_ALTHEURISTIC_H
#define WORLDWALKER_RUNTIME_ALTHEURISTIC_H

#include "format/ArtifactReader.h"

#include <cstdint>
#include <vector>

namespace ww::runtime
{
    // ALT (A*, Landmarks, Triangle-inequality) lower bound on the directed
    // area-graph cost from any area to one fixed goal area.
    //
    // The artifact bakes, for a small set of landmark areas, the cost from each
    // landmark to every area (distFromLandmark) and from every area to each
    // landmark (distToLandmark), precomputed over the baked crossing graph.
    // Global teleports are NOT in those tables: wwbuild drops them before the
    // area graph is built (they load from runtime JSON instead), and the
    // runtime uses them only as start-frontier seeds — never as mid-search
    // edges — so walk-only tables remain valid lower bounds. For a fixed goal
    // this caches the two per-landmark goal distances, then estimate() takes
    // the tightest triangle-inequality bound over all landmarks.
    //
    // The bound omits intra-area walk cost — and so does the search it guides —
    // so it is a true lower bound on that cost metric: admissible, and robust to
    // per-query edge removal, since dropping edges only lengthens true paths.
    // When the artifact bakes no landmarks, estimate() returns 0, degrading A* to
    // Dijkstra rather than failing. The borrowed reader must outlive the
    // heuristic.
    //
    // Per-query allocation budget: the two `landmarks`-sized goal-distance
    // caches are owned by this class and resized once at construction. Each
    // prepare(goalArea) refills them in place — no fresh allocations per query.
    class AltHeuristic
    {
    public:
        explicit AltHeuristic(const format::ArtifactReader &reader);

        AltHeuristic(const AltHeuristic &) = delete;
        AltHeuristic &operator=(const AltHeuristic &) = delete;
        AltHeuristic(AltHeuristic &&) = default;
        AltHeuristic &operator=(AltHeuristic &&) = default;

        // Bind to a new goal area: refills the per-landmark goal-distance
        // caches. Cheap (O(landmarks)); call at the top of each findPath
        // instead of constructing a fresh heuristic.
        void prepare(uint32_t goalArea);

        // Admissible lower-bound tick cost from `area` to the prepared goal area.
        float estimate(uint32_t area) const;

    private:
        const format::ArtifactReader *artifact;
        uint32_t landmarks;
        std::vector<float> goalToLandmark;    // distToLandmark(goal, L)
        std::vector<float> goalFromLandmark;  // distFromLandmark(L, goal)
    };
}

#endif  // WORLDWALKER_RUNTIME_ALTHEURISTIC_H
