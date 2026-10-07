#include "format/F2pLand.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ww::format
{
    namespace
    {
        constexpr int32_t kSquareShift = 6;
        constexpr int32_t kSquareSize = 1 << kSquareShift;

        bool isOnPlane(const F2pBoxRecord &box, int32_t plane)
        {
            return plane >= box.planeMin && plane <= box.planeMax;
        }

        bool contains(const F2pBoxRecord &box, int32_t x, int32_t y, int32_t plane)
        {
            return isOnPlane(box, plane) && x >= box.minX && x <= box.maxX
                && y >= box.minY && y <= box.maxY;
        }

        bool intersects(const F2pBoxRecord &box, int32_t minX, int32_t minY, int32_t maxX,
                        int32_t maxY, int32_t plane)
        {
            return isOnPlane(box, plane) && box.minX <= maxX && box.maxX >= minX
                && box.minY <= maxY && box.maxY >= minY;
        }

        bool covers(const F2pBoxRecord &box, int32_t minX, int32_t minY, int32_t maxX,
                    int32_t maxY, int32_t plane)
        {
            return isOnPlane(box, plane) && box.minX <= minX && box.maxX >= maxX
                && box.minY <= minY && box.maxY >= maxY;
        }

        bool anyContains(std::span<const F2pBoxRecord> boxes, int32_t x, int32_t y, int32_t plane)
        {
            for (const F2pBoxRecord &box : boxes)
            {
                if (contains(box, x, y, plane))
                {
                    return true;
                }
            }
            return false;
        }
    }

    F2pLand::F2pLand(std::span<const F2pBoxRecord> zones, std::span<const F2pBoxRecord> holes)
        : zoneList(zones.begin(), zones.end()),
          holeList(holes.begin(), holes.end())
    {
        if (zoneList.empty())
        {
            return;  // no land at all: every query answers Members without a table
        }
        // A square no zone touches is Members, the table's default, so only
        // the squares under some zone are classified: a few hundred, not the
        // quarter million of the whole table.
        squareTable.assign(static_cast<std::size_t>(kClipPlanes) * kSquaresPerAxis
                               * kSquaresPerAxis,
                           static_cast<uint8_t>(LandClass::Members));
        const auto clampSquare = [](int32_t tile)
        {
            return std::clamp(tile >> kSquareShift, 0, kSquaresPerAxis - 1);
        };
        for (const F2pBoxRecord &zone : zoneList)
        {
            const int32_t lastPlane = std::min<int32_t>(zone.planeMax, kClipPlanes - 1);
            for (int32_t plane = zone.planeMin; plane <= lastPlane; ++plane)
            {
                for (int32_t sy = clampSquare(zone.minY); sy <= clampSquare(zone.maxY); ++sy)
                {
                    for (int32_t sx = clampSquare(zone.minX); sx <= clampSquare(zone.maxX); ++sx)
                    {
                        squareTable[slotOf(sx, sy, plane)] =
                            static_cast<uint8_t>(classifySquare(sx, sy, plane));
                    }
                }
            }
        }
    }

    std::size_t F2pLand::slotOf(int32_t squareX, int32_t squareY, int32_t plane)
    {
        return (static_cast<std::size_t>(plane) * kSquaresPerAxis + static_cast<std::size_t>(squareY))
                   * kSquaresPerAxis
             + static_cast<std::size_t>(squareX);
    }

    // Exact for the two definite answers: a square one zone covers and no hole
    // touches is all free-to-play, and a square no zone touches, or one hole
    // covers, is all members. A square tiled by several zones reads Mixed and
    // is answered tile by tile, which is slower but never wrong.
    LandClass F2pLand::classifySquare(int32_t squareX, int32_t squareY, int32_t plane) const
    {
        const int32_t minX = squareX << kSquareShift;
        const int32_t minY = squareY << kSquareShift;
        const int32_t maxX = minX + kSquareSize - 1;
        const int32_t maxY = minY + kSquareSize - 1;
        bool isTouchingZone = false;
        bool isCoveredByZone = false;
        for (const F2pBoxRecord &zone : zoneList)
        {
            isTouchingZone = isTouchingZone || intersects(zone, minX, minY, maxX, maxY, plane);
            isCoveredByZone = isCoveredByZone || covers(zone, minX, minY, maxX, maxY, plane);
        }
        bool isTouchingHole = false;
        for (const F2pBoxRecord &hole : holeList)
        {
            if (covers(hole, minX, minY, maxX, maxY, plane))
            {
                return LandClass::Members;
            }
            isTouchingHole = isTouchingHole || intersects(hole, minX, minY, maxX, maxY, plane);
        }
        if (!isTouchingZone)
        {
            return LandClass::Members;
        }
        if (isCoveredByZone && !isTouchingHole)
        {
            return LandClass::FreeToPlay;
        }
        return LandClass::Mixed;
    }

    LandClass F2pLand::squareClass(int32_t squareX, int32_t squareY, int32_t plane) const
    {
        if (squareTable.empty() || plane < 0 || plane >= kClipPlanes || squareX < 0
            || squareY < 0 || squareX >= kSquaresPerAxis || squareY >= kSquaresPerAxis)
        {
            return LandClass::Members;
        }
        return static_cast<LandClass>(squareTable[slotOf(squareX, squareY, plane)]);
    }

    bool F2pLand::isFreeToPlayByBoxes(int32_t x, int32_t y, int32_t plane) const
    {
        return anyContains(zoneList, x, y, plane) && !anyContains(holeList, x, y, plane);
    }

    bool F2pLand::isFreeToPlay(int32_t x, int32_t y, int32_t plane) const
    {
        if (x < 0 || y < 0)
        {
            return false;
        }
        switch (squareClass(x >> kSquareShift, y >> kSquareShift, plane))
        {
            case LandClass::FreeToPlay:
                return true;
            case LandClass::Members:
                return false;
            case LandClass::Mixed:
                return isFreeToPlayByBoxes(x, y, plane);
        }
        return false;
    }

    LandClass F2pLand::classify(int32_t minX, int32_t minY, int32_t maxX, int32_t maxY,
                                int32_t plane) const
    {
        if (minX < 0 || minY < 0 || maxX < minX || maxY < minY)
        {
            return LandClass::Mixed;
        }
        bool isAnyFree = false;
        bool isAnyMembers = false;
        for (int32_t sy = minY >> kSquareShift; sy <= (maxY >> kSquareShift); ++sy)
        {
            for (int32_t sx = minX >> kSquareShift; sx <= (maxX >> kSquareShift); ++sx)
            {
                const LandClass c = squareClass(sx, sy, plane);
                if (c == LandClass::Mixed)
                {
                    return LandClass::Mixed;
                }
                isAnyFree = isAnyFree || c == LandClass::FreeToPlay;
                isAnyMembers = isAnyMembers || c == LandClass::Members;
            }
        }
        if (isAnyFree && isAnyMembers)
        {
            return LandClass::Mixed;
        }
        return isAnyFree ? LandClass::FreeToPlay : LandClass::Members;
    }

    bool isMembersNetwork(MoveCategory category)
    {
        switch (category)
        {
            case MoveCategory::Charters:
            case MoveCategory::Gliders:
            case MoveCategory::FairyRings:
            case MoveCategory::SpiritTrees:
            case MoveCategory::MagicCarpets:
                return true;
            default:
                return false;
        }
    }

    bool isMembersOnly(const TransitionRecord &tx, MoveCategory category, int8_t override,
                       const F2pLand *land)
    {
        if (override >= 0)
        {
            return override != 0;
        }
        if (isMembersNetwork(category))
        {
            return true;
        }
        if (land == nullptr)
        {
            return false;
        }
        const bool isGlobal = (tx.flags & kTransitionFlagGlobalOrigin) != 0u;
        if (!isGlobal && !land->isFreeToPlay(tx.originX, tx.originY, tx.originPlane))
        {
            return true;
        }
        return !land->isFreeToPlay(tx.destX, tx.destY, tx.destPlane);
    }
}
