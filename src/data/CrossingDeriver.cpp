#include "data/CrossingDeriver.h"

#include "data/CrossingStamp.h"
#include "data/Transitions.h"
#include "format/ClipFlags.h"

#include <cstdint>

namespace ww::data
{
    namespace
    {
        // One of the eight wall edges of a tile, paired with the step (dx, dy) to
        // the neighbour across it. A door's blocked edge means you cross it by
        // interacting with the door rather than walking. Mirrors the wall-bit
        // layout in format::ClipFlags and the executor's wall checks.
        struct WallEdge
        {
            uint32_t bit;
            int dx;
            int dy;
        };

        constexpr WallEdge kWallEdges[] = {
            {format::CLIP_WALL_N,  0,  1},
            {format::CLIP_WALL_NE, 1,  1},
            {format::CLIP_WALL_E,  1,  0},
            {format::CLIP_WALL_SE, 1, -1},
            {format::CLIP_WALL_S,  0, -1},
            {format::CLIP_WALL_SW,-1, -1},
            {format::CLIP_WALL_W, -1,  0},
            {format::CLIP_WALL_NW,-1,  1},
        };

        // Wall bits the door loc itself contributes to its tile, from the
        // producer's shape/rotation tables (NXTCacheLibrary MapSquare.cpp):
        // straight walls (shape 0) own one edge, diagonal blockers (shapes
        // 1/3) one corner, corner walls (shape 2) two edges. Other shapes own
        // no wall bit here. Gating the hop emission on this mask keeps a
        // foreign bit on the door tile — a perpendicular wall loc, or a
        // neighbour's wall reflected onto it — from minting a phantom
        // crossing "through" a wall the door does not open.
        uint32_t doorEdgeMask(uint8_t shape, uint8_t rotation)
        {
            static constexpr uint32_t kStraight[4] = {
                format::CLIP_WALL_W, format::CLIP_WALL_N,
                format::CLIP_WALL_E, format::CLIP_WALL_S,
            };
            static constexpr uint32_t kDiagonal[4] = {
                format::CLIP_WALL_NW, format::CLIP_WALL_NE,
                format::CLIP_WALL_SE, format::CLIP_WALL_SW,
            };
            static constexpr uint32_t kCorner[4] = {
                format::CLIP_WALL_W | format::CLIP_WALL_N,
                format::CLIP_WALL_N | format::CLIP_WALL_E,
                format::CLIP_WALL_E | format::CLIP_WALL_S,
                format::CLIP_WALL_S | format::CLIP_WALL_W,
            };
            const uint32_t r = rotation & 3u;
            switch (shape)
            {
                case 0:
                    return kStraight[r];
                case 1:
                case 3:
                    return kDiagonal[r];
                case 2:
                    return kCorner[r];
                default:
                    return 0u;
            }
        }

        // Build one raw Transport transition origin->dest carrying the door loc
        // already stamped onto `loc` (see stampCrossingLoc).
        Transition makeDoorHop(const Transition &loc, int fromX, int fromY,
                               int toX, int toY, int plane)
        {
            Transition t = loc;
            t.kind = TransitionKind::Transport;
            t.isGlobalOrigin = false;
            t.originX = fromX;
            t.originY = fromY;
            t.originPlane = static_cast<uint8_t>(plane);
            t.destX = toX;
            t.destY = toY;
            t.destPlane = static_cast<uint8_t>(plane);
            return t;
        }
    }

    TransitionModel deriveDoorTransitions(const std::vector<ww::build::Crossing> &crossings,
                                          const ww::build::CollisionLookup &lookup,
                                          CrossingReport *outReport)
    {
        TransitionModel result;
        CrossingReport report;

        for (const ww::build::Crossing &c : crossings)
        {
            if (c.kind != static_cast<uint8_t>(ww::build::CrossingKind::Door))
            {
                continue;
            }
            ++report.doorCrossings;

            // The clickable loc, stamped once and copied into every hop. A door
            // with no option slot cannot be opened by the executor, so no hop is
            // derived through it (the planner would route into a stall).
            Transition loc;
            if (!stampCrossingLoc(c, loc))
            {
                ++report.noOption;
                continue;
            }

            const int lx = c.worldX;
            const int ly = c.worldY;
            const int plane = c.plane;

            // The door loc must sit on a standable tile to be an origin we can
            // walk to and click. Object-footprint doors (blocked loc tile) carry
            // no wall bits anyway and are left to the dataset.
            if (!lookup.isWalkable(lx, ly, plane))
            {
                ++report.blockedOrigin;
                continue;
            }

            const uint32_t clip = lookup.clipAt(lx, ly, plane);
            const uint32_t ownEdges = doorEdgeMask(c.shape, c.rotation);
            bool any = false;
            for (const WallEdge &e : kWallEdges)
            {
                if ((clip & e.bit) == 0u)
                {
                    continue;
                }
                // Only the door's own edge(s) are crossable by clicking it; a
                // foreign blocked edge on the same tile stays a wall.
                if ((ownEdges & e.bit) == 0u)
                {
                    ++report.foreignEdgeSkipped;
                    continue;
                }
                const int nx = lx + e.dx;
                const int ny = ly + e.dy;
                if (!lookup.isWalkable(nx, ny, plane))
                {
                    continue;
                }
                // Both directions: stand on either side, click the same door loc.
                result.transitions.push_back(makeDoorHop(loc, lx, ly, nx, ny, plane));
                result.transitions.push_back(makeDoorHop(loc, nx, ny, lx, ly, plane));
                report.emitted += 2;
                any = true;
            }
            if (!any)
            {
                ++report.noEdge;
            }
        }

        if (outReport != nullptr)
        {
            *outReport = report;
        }
        return result;
    }
}
