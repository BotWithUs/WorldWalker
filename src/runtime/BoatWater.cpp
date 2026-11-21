#include "runtime/BoatWater.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <limits>

namespace ww::runtime::boat_water
{
    namespace
    {
        // One row's stretch of water: tiles firstX..lastX of row y on plane.
        struct WaterRun
        {
            int32_t plane;
            int32_t y;
            int32_t firstX;
            int32_t lastX;
        };

        // Sorted by plane, y and firstX, as the generator writes them.
        constexpr WaterRun kRuns[] = {
#include "runtime/BoatWaterRuns.inc"
        };

        // The bounding box of every run on one plane.
        struct Bounds
        {
            int32_t minX{std::numeric_limits<int32_t>::max()};
            int32_t minY{std::numeric_limits<int32_t>::max()};
            int32_t maxX{std::numeric_limits<int32_t>::min()};
            int32_t maxY{std::numeric_limits<int32_t>::min()};
            bool isEmpty{true};
        };

        constexpr Bounds boundsOf(int32_t plane)
        {
            Bounds bounds;
            for (const WaterRun &run : kRuns)
            {
                if (run.plane != plane)
                {
                    continue;
                }
                bounds.minX = std::min(bounds.minX, run.firstX);
                bounds.maxX = std::max(bounds.maxX, run.lastX);
                bounds.minY = std::min(bounds.minY, run.y);
                bounds.maxY = std::max(bounds.maxY, run.y);
                bounds.isEmpty = false;
            }
            return bounds;
        }

        constexpr int32_t kPlanes = 4;

        constexpr Bounds kBounds[kPlanes] = {boundsOf(0), boundsOf(1), boundsOf(2), boundsOf(3)};

        bool isRunBefore(const WaterRun &run, int32_t plane, int32_t y, int32_t x)
        {
            if (run.plane != plane)
            {
                return run.plane < plane;
            }
            if (run.y != y)
            {
                return run.y < y;
            }
            return run.firstX <= x;
        }
    }

    bool covers(int32_t x, int32_t y, int32_t plane)
    {
        if (plane < 0 || plane >= kPlanes)
        {
            return false;
        }
        const Bounds &bounds = kBounds[plane];
        return !bounds.isEmpty && x >= bounds.minX && x <= bounds.maxX && y >= bounds.minY
            && y <= bounds.maxY;
    }

    bool isWater(int32_t x, int32_t y, int32_t plane)
    {
        if (!covers(x, y, plane))
        {
            return false;
        }
        // The first run that starts past x on this row, or on a later row;
        // the one before it is the only run that can hold x.
        const auto after = std::partition_point(std::begin(kRuns), std::end(kRuns),
                                                [&](const WaterRun &run)
                                                {
                                                    return isRunBefore(run, plane, y, x);
                                                });
        if (after == std::begin(kRuns))
        {
            return false;
        }
        const WaterRun &run = *std::prev(after);
        return run.plane == plane && run.y == y && x >= run.firstX && x <= run.lastX;
    }

    bool nearestWater(int32_t x, int32_t y, int32_t plane, int32_t reach, int32_t &outX,
                      int32_t &outY)
    {
        for (int32_t ring = 0; ring <= reach; ++ring)
        {
            for (int32_t dy = -ring; dy <= ring; ++dy)
            {
                for (int32_t dx = -ring; dx <= ring; ++dx)
                {
                    const bool isOnRing = std::abs(dx) == ring || std::abs(dy) == ring;
                    if (isOnRing && isWater(x + dx, y + dy, plane))
                    {
                        outX = x + dx;
                        outY = y + dy;
                        return true;
                    }
                }
            }
        }
        return false;
    }
}
