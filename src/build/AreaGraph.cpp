#include "build/AreaGraph.h"

#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/ClipFlags.h"
#include "format/WallApproach.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ww::build
{
    namespace
    {
        using ww::data::Transition;
        using ww::data::TransitionModel;

        constexpr int32_t kUnassigned = -1;

        // Pack a (square, plane) into a grid key. squareX 0..127, squareY 0..255,
        // plane 0..3 all fit well inside the shifted fields. Out-of-range
        // squares would alias other keys (256 << 4 == 1 << 12), so callers
        // taking unvalidated world coordinates must range-check first.
        uint32_t gridKey(int squareX, int squareY, int plane)
        {
            return (static_cast<uint32_t>(squareX) << 12)
                 | (static_cast<uint32_t>(squareY) << 4)
                 | static_cast<uint32_t>(plane);
        }

        // World-axis extent the grid keys can address: 256 squares of 64 tiles.
        constexpr int kWorldAxisTiles = 256 * format::kClipSize;

        uint64_t packTile(int x, int y)
        {
            return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32)
                 | static_cast<uint64_t>(static_cast<uint32_t>(y));
        }

        // Lazily-allocated per-(square, plane) area-id grids, addressed by world
        // tile. Absent tiles read as kUnassigned; writing one materializes its grid.
        class AreaMap
        {
        public:
            int32_t areaAt(int worldX, int worldY, int plane) const
            {
                // Dataset transitions carry unvalidated coordinates; a junk
                // dest past the addressable grid must read as unassigned, not
                // alias another square's key and resolve to a real area.
                if (worldX < 0 || worldY < 0
                    || worldX >= kWorldAxisTiles || worldY >= kWorldAxisTiles)
                {
                    return kUnassigned;
                }
                const auto it = index.find(gridKey(worldX >> 6, worldY >> 6, plane));
                if (it == index.end())
                {
                    return kUnassigned;
                }
                return grids[it->second].ids[localIndex(worldX, worldY)];
            }

            void setArea(int worldX, int worldY, int plane, int32_t areaId)
            {
                grids[gridIndex(worldX >> 6, worldY >> 6, plane)].ids[localIndex(worldX, worldY)] = areaId;
            }

            std::vector<AreaGrid> takeGrids()
            {
                return std::move(grids);
            }

        private:
            static std::size_t localIndex(int worldX, int worldY)
            {
                return static_cast<std::size_t>(worldX & 63) * format::kClipSize
                     + static_cast<std::size_t>(worldY & 63);
            }

            std::size_t gridIndex(int squareX, int squareY, int plane)
            {
                const uint32_t key = gridKey(squareX, squareY, plane);
                const auto it = index.find(key);
                if (it != index.end())
                {
                    return it->second;
                }
                const std::size_t pos = grids.size();
                AreaGrid grid;
                grid.squareX = squareX;
                grid.squareY = squareY;
                grid.plane = plane;
                grid.ids.assign(static_cast<std::size_t>(format::kClipSize) * format::kClipSize, kUnassigned);
                grids.push_back(std::move(grid));
                index.emplace(key, pos);
                return pos;
            }

            std::unordered_map<uint32_t, std::size_t> index;
            std::vector<AreaGrid> grids;
        };

        // Whether you can move one cardinal step from (x, y) to (x+dx, y+dy) on
        // `plane`: the destination must be standable and no wall edge may block the
        // crossing (a wall on either endpoint blocks it). The source is assumed
        // walkable — the flood fill only expands from already-assigned tiles.
        // `src` is the source tile's clip word, read once by the caller for all
        // four directions rather than re-fetched per direction.
        bool canStep(const CollisionLookup &lookup, uint32_t src, int x, int y, int plane,
                     int dx, int dy)
        {
            const uint32_t dst = lookup.clipAt(x + dx, y + dy, plane);
            if ((dst & format::kClipStandBlockedMask) != 0u)
            {
                return false;
            }
            uint32_t srcWall = 0u;
            uint32_t dstWall = 0u;
            if (dy == 1)
            {
                srcWall = format::CLIP_WALL_N;
                dstWall = format::CLIP_WALL_S;
            }
            else if (dy == -1)
            {
                srcWall = format::CLIP_WALL_S;
                dstWall = format::CLIP_WALL_N;
            }
            else if (dx == 1)
            {
                srcWall = format::CLIP_WALL_E;
                dstWall = format::CLIP_WALL_W;
            }
            else
            {
                srcWall = format::CLIP_WALL_W;
                dstWall = format::CLIP_WALL_E;
            }
            return (src & srcWall) == 0u && (dst & dstWall) == 0u;
        }

        void extendBounds(AreaNode &node, int x, int y)
        {
            node.minX = std::min(node.minX, x);
            node.minY = std::min(node.minY, y);
            node.maxX = std::max(node.maxX, x);
            node.maxY = std::max(node.maxY, y);
        }

        // Claim every tile cardinally walk-connected to the seed, accumulating the
        // node's tile count, centroid, and bounding box.
        AreaNode floodFill(const CollisionLookup &lookup, AreaMap &map,
                           int seedX, int seedY, int plane, int32_t areaId)
        {
            static constexpr int dx[4] = {0, 0, 1, -1};
            static constexpr int dy[4] = {1, -1, 0, 0};

            AreaNode node;
            node.plane = static_cast<uint8_t>(plane);
            node.minX = seedX;
            node.minY = seedY;
            node.maxX = seedX;
            node.maxY = seedY;
            int64_t sumX = 0;
            int64_t sumY = 0;

            std::vector<uint64_t> stack;
            stack.push_back(packTile(seedX, seedY));
            map.setArea(seedX, seedY, plane, areaId);

            while (!stack.empty())
            {
                const uint64_t cur = stack.back();
                stack.pop_back();
                const int x = static_cast<int>(static_cast<uint32_t>(cur >> 32));
                const int y = static_cast<int>(static_cast<uint32_t>(cur & 0xFFFFFFFFu));

                ++node.tileCount;
                sumX += x;
                sumY += y;
                extendBounds(node, x, y);

                const uint32_t src = lookup.clipAt(x, y, plane);
                for (int d = 0; d < 4; ++d)
                {
                    const int nx = x + dx[d];
                    const int ny = y + dy[d];
                    if (!canStep(lookup, src, x, y, plane, dx[d], dy[d]))
                    {
                        continue;
                    }
                    if (map.areaAt(nx, ny, plane) != kUnassigned)
                    {
                        continue;
                    }
                    map.setArea(nx, ny, plane, areaId);
                    stack.push_back(packTile(nx, ny));
                }
            }

            node.centroidX = static_cast<int32_t>(sumX / node.tileCount);
            node.centroidY = static_cast<int32_t>(sumY / node.tileCount);
            return node;
        }

        void labelSquarePlane(const SquareClip &sq, int plane, const CollisionLookup &lookup,
                              AreaMap &map, std::vector<AreaNode> &outNodes)
        {
            const int baseX = sq.squareX * format::kClipSize;
            const int baseY = sq.squareY * format::kClipSize;
            for (int lx = 0; lx < format::kClipSize; ++lx)
            {
                for (int ly = 0; ly < format::kClipSize; ++ly)
                {
                    const int wx = baseX + lx;
                    const int wy = baseY + ly;
                    if (!lookup.isWalkable(wx, wy, plane))
                    {
                        continue;
                    }
                    if (map.areaAt(wx, wy, plane) != kUnassigned)
                    {
                        continue;
                    }
                    const int32_t areaId = static_cast<int32_t>(outNodes.size());
                    outNodes.push_back(floodFill(lookup, map, wx, wy, plane, areaId));
                }
            }
        }

        // The planeMask gate below only chooses where fills are SEEDED. A fill
        // crosses mapsquare seams freely, and an all-zero plane is uniformly
        // standable with no walls, so every open plane adjacent to a seeded
        // one is swallowed whole and gets a grid anyway (in a real bake ~22k
        // of ~32k grids sit on planes whose mask bit is clear). That is the
        // intended outcome: the mask cannot tell open ground from void, since
        // both are all-zero words, so it must not be used as a walkability
        // rule here or in CollisionLookup. Do not "fix" this by blocking
        // mask-clear planes; it would blockade genuinely open terrain.
        void labelAreas(const CollisionModel &collision, const CollisionLookup &lookup,
                        AreaMap &map, std::vector<AreaNode> &outNodes)
        {
            for (const SquareClip &sq : collision.squares)
            {
                for (int plane = 0; plane < format::kClipPlanes; ++plane)
                {
                    if ((sq.planeMask & (1u << plane)) != 0u)
                    {
                        labelSquarePlane(sq, plane, lookup, map, outNodes);
                    }
                }
            }
        }

        // The clip accessor format::WallApproach templates over. Its contract
        // (CLIP_BLOCKED for an unaddressable tile) is exactly what
        // CollisionLookup::clipAt already promises.
        auto clipAccessor(const CollisionLookup &lookup)
        {
            return [&lookup](int x, int y, int plane) { return lookup.clipAt(x, y, plane); };
        }

        // Areas touching a transition's origin object — the tiles you could
        // stand on to interact. The object tile is often blocked, so the
        // neighbourhood within data::kTransitionApproachRadius is scanned; a
        // door on a boundary yields the side(s) reachable WITHOUT crossing a
        // wall. An earlier version ignored wall flags and so emitted AreaEdges
        // through the un-reachable side of doors, sending the runtime to walk
        // to the wrong side first.
        std::set<int32_t> collectOriginAreas(const AreaMap &map, const CollisionLookup &lookup,
                                             const Transition &t)
        {
            constexpr int radius = ww::data::kTransitionApproachRadius;
            const int plane = static_cast<int>(t.originPlane);
            std::set<int32_t> areas;
            for (int ox = -radius; ox <= radius; ++ox)
            {
                for (int oy = -radius; oy <= radius; ++oy)
                {
                    const int candX = t.originX + ox;
                    const int candY = t.originY + oy;
                    // The origin tile itself is a legitimate approach whenever
                    // it is walkable, and for a door hop out of CrossingDeriver
                    // it is THE approach: that origin is the walkable tile
                    // beside the door, verified standable at emit time. It has
                    // no approach to seal, so it only has to be in an area
                    // (which is exactly the walkability test).
                    if (ox != 0 || oy != 0)
                    {
                        // Walk from the candidate back toward the origin,
                        // wall-checking each step. If it is sealed, the door /
                        // wall sits between the candidate and the object —
                        // exclude this side.
                        if (format::approachSealed(clipAccessor(lookup), candX, candY,
                                                   t.originX, t.originY, plane))
                        {
                            continue;
                        }
                    }
                    const int32_t a = map.areaAt(candX, candY, plane);
                    if (a >= 0)
                    {
                        areas.insert(a);
                    }
                }
            }
            return areas;
        }

        bool emitEdges(const std::set<int32_t> &fromAreas, int32_t destArea,
                       std::size_t transitionIndex, float cost,
                       std::vector<AreaEdge> &outEdges, AreaGraphReport &report)
        {
            bool emitted = false;
            for (int32_t fromArea : fromAreas)
            {
                if (fromArea == destArea)
                {
                    ++report.intraAreaSkipped;
                    continue;
                }
                AreaEdge edge;
                edge.fromArea = fromArea;
                edge.toArea = destArea;
                edge.transitionIndex = static_cast<uint32_t>(transitionIndex);
                edge.cost = cost;
                outEdges.push_back(edge);
                emitted = true;
            }
            return emitted;
        }

        void resolveAdjacency(const TransitionModel &transitions, const AreaMap &map,
                              const CollisionLookup &lookup,
                              std::vector<AreaEdge> &outEdges,
                              AreaGraphReport &report)
        {
            for (std::size_t i = 0; i < transitions.transitions.size(); ++i)
            {
                const Transition &t = transitions.transitions[i];
                if (t.isGlobalOrigin)
                {
                    // Not an area-graph edge: the runtime seeds global
                    // teleports at the search frontier instead (ADR 0009).
                    // main.cpp drops them all before this point today, so this
                    // branch only counts.
                    ++report.globalSkipped;
                    continue;
                }
                const int32_t destArea = map.areaAt(t.destX, t.destY, t.destPlane);
                if (destArea < 0)
                {
                    ++report.unresolvedDest;
                    continue;
                }
                std::set<int32_t> fromAreas = collectOriginAreas(map, lookup, t);
                if (fromAreas.empty())
                {
                    ++report.unresolvedOrigin;
                    continue;
                }

                // A vertical transition (stairs/ladder) is a fully-blocked loc
                // tile, so it is the flood-fill block itself — not a wall flag —
                // that separates the structure the stairs live in from the open
                // ground beside it. collectOriginAreas only consults wall flags,
                // so it admits BOTH the room the stairs are in and the adjacent
                // outdoors, and the planner then "climbs" from the wrong side of
                // the wall (it never reaches the usable tile and stalls). The
                // room that actually owns the staircase is the one directly
                // beneath the upper landing: the (already-snapped) destination
                // tile projected onto the origin plane. When that area is one of
                // the collected sides it is the only legal approach — drop the
                // rest. Same-plane transitions (doors) keep a directional wall
                // flag and are left untouched.
                if (t.originPlane != t.destPlane)
                {
                    const int32_t ownerArea =
                        map.areaAt(t.destX, t.destY, static_cast<int>(t.originPlane));
                    if (ownerArea >= 0 && fromAreas.count(ownerArea) != 0)
                    {
                        fromAreas = {ownerArea};
                        ++report.verticalApproachPinned;
                    }
                }

                if (emitEdges(fromAreas, destArea, i, t.cost, outEdges, report))
                {
                    ++report.resolvedTransitions;
                }
                else
                {
                    // Both endpoints resolved, but every approach side is the
                    // destination area already, so walking suffices and no edge
                    // exists to emit. Counted separately from intraAreaSkipped
                    // (which counts dropped edges, several per transition):
                    // without this the transition appeared in no bucket of the
                    // adjacency report at all, so the report did not add up.
                    ++report.intraAreaOnly;
                }
            }
        }

        bool edgeLess(const AreaEdge &a, const AreaEdge &b)
        {
            if (a.fromArea != b.fromArea)
            {
                return a.fromArea < b.fromArea;
            }
            if (a.toArea != b.toArea)
            {
                return a.toArea < b.toArea;
            }
            return a.transitionIndex < b.transitionIndex;
        }

        bool gridLess(const AreaGrid &a, const AreaGrid &b)
        {
            if (a.squareY != b.squareY)
            {
                return a.squareY < b.squareY;
            }
            if (a.squareX != b.squareX)
            {
                return a.squareX < b.squareX;
            }
            return a.plane < b.plane;
        }

        std::size_t largestTileCount(const std::vector<AreaNode> &nodes)
        {
            std::size_t largest = 0;
            for (const AreaNode &node : nodes)
            {
                largest = std::max(largest, static_cast<std::size_t>(node.tileCount));
            }
            return largest;
        }
    }

    AreaGraphModel buildAreaGraph(const CollisionModel &collision, const CollisionLookup &lookup,
                                  const ww::data::TransitionModel &transitions,
                                  AreaGraphReport *outReport)
    {
        AreaMap map;
        AreaGraphModel model;
        AreaGraphReport report;

        labelAreas(collision, lookup, map, model.nodes);
        resolveAdjacency(transitions, map, lookup, model.edges, report);
        std::sort(model.edges.begin(), model.edges.end(), edgeLess);

        model.grids = map.takeGrids();
        std::sort(model.grids.begin(), model.grids.end(), gridLess);

        report.areaCount = model.nodes.size();
        report.edgeCount = model.edges.size();
        report.gridCount = model.grids.size();
        report.largestArea = largestTileCount(model.nodes);

        if (outReport != nullptr)
        {
            *outReport = report;
        }
        return model;
    }
}
