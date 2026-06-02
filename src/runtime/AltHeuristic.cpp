#include "runtime/AltHeuristic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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
        float best = 0.0f;
        for (uint32_t l = 0; l < landmarks; ++l)
        {
            // d(area,goal) >= d(area,L) - d(goal,L), valid when both are finite.
            // std::isfinite filters both inf AND NaN — the latter would survive
            // !std::isinf and silently corrupt the bound.
            const float toL = artifact->distToLandmark(area, l);
            if (std::isfinite(toL) && std::isfinite(goalToLandmark[l]))
            {
                best = std::max(best, toL - goalToLandmark[l]);
            }
            // d(area,goal) >= d(L,goal) - d(L,area), valid when both are finite.
            const float fromL = artifact->distFromLandmark(l, area);
            if (std::isfinite(fromL) && std::isfinite(goalFromLandmark[l]))
            {
                best = std::max(best, goalFromLandmark[l] - fromL);
            }
        }
        return best;
    }
}
