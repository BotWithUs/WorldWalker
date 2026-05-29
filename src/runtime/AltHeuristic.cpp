#include "runtime/AltHeuristic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ww::runtime
{
    AltHeuristic::AltHeuristic(const format::ArtifactReader &reader, uint32_t goalArea)
        : artifact(&reader),
          landmarks(reader.hasAltLandmarks() ? reader.landmarkCount() : 0u)
    {
        goalToLandmark.resize(landmarks);
        goalFromLandmark.resize(landmarks);
        for (uint32_t l = 0; l < landmarks; ++l)
        {
            goalToLandmark[l] = reader.distToLandmark(goalArea, l);
            goalFromLandmark[l] = reader.distFromLandmark(l, goalArea);
        }
    }

    float AltHeuristic::estimate(uint32_t area) const
    {
        float best = 0.0f;
        for (uint32_t l = 0; l < landmarks; ++l)
        {
            // d(area,goal) >= d(area,L) - d(goal,L), valid when both are finite.
            const float toL = artifact->distToLandmark(area, l);
            if (!std::isinf(toL) && !std::isinf(goalToLandmark[l]))
            {
                best = std::max(best, toL - goalToLandmark[l]);
            }
            // d(area,goal) >= d(L,goal) - d(L,area), valid when both are finite.
            const float fromL = artifact->distFromLandmark(l, area);
            if (!std::isinf(fromL) && !std::isinf(goalFromLandmark[l]))
            {
                best = std::max(best, goalFromLandmark[l] - fromL);
            }
        }
        return best;
    }
}
