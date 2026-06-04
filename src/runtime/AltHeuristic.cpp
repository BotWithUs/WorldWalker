#include "runtime/AltHeuristic.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace ww::runtime
{
    AltHeuristic::AltHeuristic(const format::ArtifactReader &reader)
        : artifact(&reader),
          landmarks(reader.hasAltLandmarks() ? reader.landmarkCount() : 0u)
    {
        // Allocate the two goal-distance caches once at context construction
        // so subsequent prepare() calls overwrite in place.
        goalToLandmark.resize(landmarks);
        goalFromLandmark.resize(landmarks);
    }

    void AltHeuristic::prepare(uint32_t goalArea)
    {
        for (uint32_t l = 0; l < landmarks; ++l)
        {
            goalToLandmark[l] = artifact->distToLandmark(goalArea, l);
            goalFromLandmark[l] = artifact->distFromLandmark(l, goalArea);
        }
    }

    float AltHeuristic::estimate(uint32_t area) const
    {
        // Sentinel compare (`x < +INF`) instead of std::isfinite. Both filter
        // +INF identically (the unreachable encoding); both treat NaN as
        // unfinite (NaN < anything is false), so the filter is semantically
        // equivalent — but the compare is a single fcmp the autovectorizer
        // can fold into the surrounding max, while isfinite is a function
        // call that breaks the tight loop. Since both ALT tables are stored
        // area-major (see ArtifactReader::transposeAltTable), the two
        // distToLandmark / distFromLandmark reads in this loop walk
        // contiguous landmark-wide windows for `area` — cache-friendly.
        constexpr float kInf = std::numeric_limits<float>::infinity();
        float best = 0.0f;
        for (uint32_t l = 0; l < landmarks; ++l)
        {
            // d(area,goal) >= d(area,L) - d(goal,L), valid when both are finite.
            const float toL = artifact->distToLandmark(area, l);
            const float goalToL = goalToLandmark[l];
            if (toL < kInf && goalToL < kInf)
            {
                best = std::max(best, toL - goalToL);
            }
            // d(area,goal) >= d(L,goal) - d(L,area), valid when both are finite.
            const float fromL = artifact->distFromLandmark(l, area);
            const float goalFromL = goalFromLandmark[l];
            if (fromL < kInf && goalFromL < kInf)
            {
                best = std::max(best, goalFromL - fromL);
            }
        }
        return best;
    }
}
