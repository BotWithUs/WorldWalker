#include "runtime/InstanceMap.h"

#include <cstddef>
#include <cstdint>

namespace ww::runtime
{
    void InstanceMap::assign(int32_t originMapX, int32_t originMapY,
                             int32_t gridW, int32_t gridH,
                             const int32_t *descriptors, std::size_t descriptorCount)
    {
        clear();
        if (descriptors == nullptr || gridW <= 0 || gridH <= 0)
        {
            return;
        }
        // An origin outside the addressable band is refused rather than
        // installed. `originMapX << 3` on a garbage value wraps to a small
        // chunk index, which would make tiles near the wrapped position resolve
        // to real-looking source chunks — memory-safe, and exactly the
        // plausible-wrong-answer failure this file exists to avoid. A grid that
        // would extend past the band is refused for the same reason.
        if (originMapX < 0 || originMapY < 0
            || originMapX >= kMapsquaresPerAxis || originMapY >= kMapsquaresPerAxis
            || (originMapX << kMapsquareChunkShift) + gridW > kChunksPerAxis
            || (originMapY << kMapsquareChunkShift) + gridH > kChunksPerAxis)
        {
            return;
        }
        // 4 * gridW * gridH computed in 64-bit on purpose: the product of two
        // int32 grid dimensions can overflow before the count check below would
        // have a chance to reject it. The producer computes its own
        // requiredChunks the same way.
        const uint64_t required = static_cast<uint64_t>(kPlaneCount)
                                * static_cast<uint64_t>(gridW)
                                * static_cast<uint64_t>(gridH);
        if (descriptorCount < required)
        {
            // A short grid is refused outright rather than partially installed.
            // The agent leaves the tail of its chunk array stale rather than
            // clearing it every tick, so reading past the published count would
            // resolve to the PREVIOUS instance's tiles — plausible wrong
            // answers, which is the worst failure mode a pathfinder can have.
            return;
        }
        const std::size_t count = static_cast<std::size_t>(required);
        cells.assign(descriptors, descriptors + count);
        originX = originMapX;
        originY = originMapY;
        width = gridW;
        height = gridH;
    }

    void InstanceMap::clear()
    {
        // Keeps the vector's capacity so a context reused across runs does not
        // re-allocate the grid on every walk.
        cells.clear();
        originX = 0;
        originY = 0;
        width = 0;
        height = 0;
    }
}
