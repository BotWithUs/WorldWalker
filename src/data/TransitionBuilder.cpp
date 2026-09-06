#include "data/TransitionBuilder.h"

#include "build/CollisionLookup.h"
#include "data/TransitionCost.h"
#include "data/Transitions.h"
#include "runtime/TileScan.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <unordered_set>
#include <utility>

namespace ww::data
{
    namespace
    {
        using ww::build::CollisionLookup;

        // Endpoint snap radius. Per-kind tick costs and computeCost() now live in
        // data/TransitionCost.h (shared with the runtime teleport loader so both
        // paths cost identically).
        constexpr int kSnapRadius = 5;

        // Move (x, y) to the closest standable tile within `radius` (Chebyshev).
        // The ring order and the deterministic (distance, dy, dx) tiebreak live
        // in runtime::findNearestTile, which the runtime teleport loader and the
        // planner's interact-tile / goal snapping share - so every consumer of
        // "nearest standable tile" agrees on the answer and the bake stays
        // reproducible across runs. Returns false if no walkable tile sits
        // within the radius.
        bool snapToWalkable(const CollisionLookup &collision, int &x, int &y, int plane,
                            int radius, bool &outMoved)
        {
            const auto walkable = [&](int32_t tx, int32_t ty)
            {
                return collision.isWalkable(tx, ty, plane);
            };
            int32_t snappedX = x;
            int32_t snappedY = y;
            if (!runtime::findNearestTile(x, y, radius, true, walkable, snappedX, snappedY))
            {
                return false;
            }
            outMoved = outMoved || snappedX != x || snappedY != y;
            x = snappedX;
            y = snappedY;
            return true;
        }

        // True if any tile within `radius` (Chebyshev, centre included) is standable.
        bool hasWalkableNeighbor(const CollisionLookup &collision, int x, int y, int plane,
                                 int radius)
        {
            const auto walkable = [&](int32_t tx, int32_t ty)
            {
                return collision.isWalkable(tx, ty, plane);
            };
            int32_t unusedX = 0;
            int32_t unusedY = 0;
            return runtime::findNearestTile(x, y, radius, true, walkable, unusedX, unusedY);
        }

        enum class Outcome
        {
            Kept,
            Dangling,
            SelfLoop,
        };

        Outcome snapAndCost(Transition &t, const CollisionLookup &collision,
                            TransitionReport &report)
        {
            t.cost = computeCost(t);

            // Local-origin kinds click an object at (originX, originY): keep that
            // tile, but drop the edge if its raw origin equals its dest (incomplete
            // fairy-ring data) or no walkable approach tile lies within the radius.
            if (!t.isGlobalOrigin)
            {
                if (t.originX == t.destX && t.originY == t.destY && t.originPlane == t.destPlane)
                {
                    return Outcome::SelfLoop;
                }
                if (!hasWalkableNeighbor(collision, t.originX, t.originY, t.originPlane, kSnapRadius))
                {
                    return Outcome::Dangling;
                }
            }

            bool destMoved = false;
            if (!snapToWalkable(collision, t.destX, t.destY, t.destPlane, kSnapRadius, destMoved))
            {
                return Outcome::Dangling;
            }
            if (destMoved)
            {
                ++report.snappedDest;
            }
            // Re-check after the snap: a dest snapped onto a walkable origin
            // tile is the same degenerate edge the pre-snap check drops.
            if (!t.isGlobalOrigin
                && t.originX == t.destX && t.originY == t.destY && t.originPlane == t.destPlane)
            {
                return Outcome::SelfLoop;
            }
            return Outcome::Kept;
        }

        // Identity key for dedup. Two transitions match when their endpoint
        // tuple AND kind agree — independent of objectId / shape / rotation /
        // chain padding — so a dataset entry and a derived freshness entry
        // pointing at the same ladder collapse to one. First-seen wins: the
        // dataset is processed before the derived candidates in main.cpp, so
        // dataset transitions take precedence on a tie.
        struct EndpointKey
        {
            TransitionKind kind;
            int32_t originX;
            int32_t originY;
            uint8_t originPlane;
            int32_t destX;
            int32_t destY;
            uint8_t destPlane;
            bool isGlobalOrigin;
        };

        bool operator==(const EndpointKey &a, const EndpointKey &b)
        {
            return a.kind            == b.kind
                && a.isGlobalOrigin  == b.isGlobalOrigin
                && a.originX         == b.originX
                && a.originY         == b.originY
                && a.originPlane     == b.originPlane
                && a.destX           == b.destX
                && a.destY           == b.destY
                && a.destPlane       == b.destPlane;
        }

        struct EndpointKeyHash
        {
            std::size_t operator()(const EndpointKey &k) const noexcept
            {
                // FNV-1a 64-bit over the fixed-layout key.
                uint64_t h = 1469598103934665603ull;
                auto mix = [&h](uint64_t v)
                {
                    for (int i = 0; i < 8; ++i)
                    {
                        h ^= static_cast<uint8_t>(v >> (i * 8));
                        h *= 1099511628211ull;
                    }
                };
                mix(static_cast<uint64_t>(k.kind));
                mix(static_cast<uint64_t>(k.isGlobalOrigin));
                mix(static_cast<uint64_t>(static_cast<uint32_t>(k.originX)));
                mix(static_cast<uint64_t>(static_cast<uint32_t>(k.originY)));
                mix(static_cast<uint64_t>(k.originPlane));
                mix(static_cast<uint64_t>(static_cast<uint32_t>(k.destX)));
                mix(static_cast<uint64_t>(static_cast<uint32_t>(k.destY)));
                mix(static_cast<uint64_t>(k.destPlane));
                return static_cast<std::size_t>(h);
            }
        };

        EndpointKey endpointKey(const Transition &t)
        {
            return {t.kind, t.originX, t.originY, t.originPlane,
                    t.destX, t.destY, t.destPlane, t.isGlobalOrigin};
        }
    }

    TransitionModel finalizeTransitions(const TransitionModel &raw,
                                        const ww::build::CollisionLookup &collision,
                                        TransitionReport *outReport)
    {
        TransitionModel result;
        TransitionReport report;
        report.input = raw.transitions.size();
        result.transitions.reserve(raw.transitions.size());

        std::unordered_set<EndpointKey, EndpointKeyHash> seen;
        seen.reserve(raw.transitions.size() * 2 + 1);

        for (const Transition &source : raw.transitions)
        {
            Transition t = source;
            const Outcome outcome = snapAndCost(t, collision, report);
            if (outcome == Outcome::Dangling)
            {
                ++report.droppedDangling;
                continue;
            }
            if (outcome == Outcome::SelfLoop)
            {
                ++report.droppedSelfLoop;
                continue;
            }
            // Dedup on (kind, origin, dest, isGlobalOrigin). Snapping above
            // may have moved dest, so we key after snap — two dataset entries
            // that snap to the same destination collapse, as do a dataset
            // entry and a derived freshness entry at the same effective tile.
            if (!seen.insert(endpointKey(t)).second)
            {
                ++report.droppedDuplicate;
                continue;
            }
            result.transitions.push_back(std::move(t));
        }

        report.kept = result.transitions.size();
        if (outReport != nullptr)
        {
            *outReport = report;
        }
        return result;
    }
}
