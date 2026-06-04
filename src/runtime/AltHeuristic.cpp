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
        // Phase 5: the area-side tables are pre-substituted at load
        // (toL: +inf -> -1e30, fromL: +inf -> +1e30). Mirror the substitution
        // on the goal side so estimate() can be branchless. The replacement
        // must yield bound <= 0 whenever EITHER side was originally +inf:
        //   goalToL  (subtrahend in bound1) : +inf -> +1e30 (so finite - +1e30 < 0)
        //   goalFromL(minuend in bound2)    : +inf -> -1e30 (so -1e30 - finite < 0)
        // Detection threshold is the same finite-vs-+inf comparison the prior
        // estimate() used, applied once per landmark per query instead of
        // four times per landmark per heap push.
        constexpr float kInf = std::numeric_limits<float>::infinity();
        constexpr float kBigPos = 1e30f;
        constexpr float kBigNeg = -1e30f;
        for (uint32_t l = 0; l < landmarks; ++l)
        {
            const float rawTo = artifact->distToLandmark(goalArea, l);
            const float rawFrom = artifact->distFromLandmark(l, goalArea);
            // Post-substitution area-side encodes unreachable as -1e30 (toL)
            // / +1e30 (fromL), so a finite check is "rawTo > -1e30" or
            // "rawFrom < +1e30". Equivalently a single finite-or-not compare
            // against +inf still works (since 1e30 < +inf) — keep it simple.
            goalToLandmark[l] = (rawTo > kBigNeg && rawTo < kInf) ? rawTo : kBigPos;
            goalFromLandmark[l] = (rawFrom < kBigPos && rawFrom < kInf) ? rawFrom : kBigNeg;
        }
    }

    float AltHeuristic::estimate(uint32_t area) const
    {
        // Branchless ALT reduction: bound1 = toL - goalToL, bound2 =
        // goalFromL - fromL. Sentinel substitutions at load (area side) and
        // prepare (goal side) make the per-landmark bound <= 0 whenever
        // either side was originally +inf, so max(0, bound) collapses the
        // unreachable case to 0 — identical contribution to the prior
        // branched form. The reduction autovectorizes at -O2 / /O2 because
        // there are no `if`s in the loop body and the area-major layout
        // (set up by ArtifactReader::transposeAltTable) keeps the per-area
        // reads contiguous over the landmark axis.
        float best = 0.0f;
        for (uint32_t l = 0; l < landmarks; ++l)
        {
            const float toL = artifact->distToLandmark(area, l);
            const float fromL = artifact->distFromLandmark(l, area);
            best = std::max(best, toL - goalToLandmark[l]);
            best = std::max(best, goalFromLandmark[l] - fromL);
        }
        return best;
    }
}
