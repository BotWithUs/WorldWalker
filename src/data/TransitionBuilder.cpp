#include "data/TransitionBuilder.h"

#include "build/CollisionLookup.h"
#include "data/Transitions.h"

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

        // Tunables (plan follow-up): per-kind default tick costs and the endpoint
        // snap radius. The dataset format does NOT carry a per-transition cost
        // field today, and wwbuild owns cost computation: chain Wait steps
        // accumulated against a per-kind base. If the dataset ever grows a
        // `cost_ticks` field, route that through Transition::cost in the loader
        // AND short-circuit computeCost here — relying on cost==0 as the
        // sentinel would clash with a legitimate zero-cost transition.
        constexpr float kTransportTicks = 3.0f;
        constexpr float kFairyRingTicks = 5.0f;
        constexpr float kTeleportChainTicks = 5.0f;
        constexpr float kSpellBaseTicks = 1.0f;
        constexpr float kLodestoneBaseTicks = 1.0f;
        constexpr int kSnapRadius = 5;

        float baseTicks(TransitionKind kind)
        {
            switch (kind)
            {
                case TransitionKind::Transport:     return kTransportTicks;
                case TransitionKind::FairyRing:     return kFairyRingTicks;
                case TransitionKind::TeleportChain: return kTeleportChainTicks;
                case TransitionKind::Spell:         return kSpellBaseTicks;
                case TransitionKind::Lodestone:     return kLodestoneBaseTicks;
            }
            return kTransportTicks;
        }

        float computeCost(const Transition &t)
        {
            float waits = 0.0f;
            for (const ChainStep &s : t.chain)
            {
                if (s.kind == ChainStepKind::Wait)
                {
                    waits += static_cast<float>(s.a);
                }
            }
            return waits + baseTicks(t.kind);
        }

        // Move (x, y) to the *closest* standable tile within `radius` (Chebyshev),
        // ring by ring. Inside each ring the candidate with the smallest squared-
        // Euclidean distance wins, with deterministic (dy, dx) tiebreak — so a
        // ring-1 cardinal neighbour is preferred over the dx-major-first corner,
        // and the bake is reproducible across runs. Returns false if no walkable
        // tile sits within the radius.
        bool snapToWalkable(const CollisionLookup &collision, int &x, int &y, int plane,
                            int radius, bool &outMoved)
        {
            if (collision.isWalkable(x, y, plane))
            {
                return true;
            }
            for (int r = 1; r <= radius; ++r)
            {
                int bestDx = 0;
                int bestDy = 0;
                int64_t bestSq = std::numeric_limits<int64_t>::max();
                bool found = false;
                for (int dy = -r; dy <= r; ++dy)
                {
                    for (int dx = -r; dx <= r; ++dx)
                    {
                        if (std::max(std::abs(dx), std::abs(dy)) != r)
                        {
                            continue;
                        }
                        if (!collision.isWalkable(x + dx, y + dy, plane))
                        {
                            continue;
                        }
                        const int64_t sq =
                            static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
                        // Lexicographic tiebreak on (sq, dy, dx) makes the choice
                        // deterministic without depending on iteration order.
                        if (!found || sq < bestSq
                            || (sq == bestSq && (dy < bestDy
                                                 || (dy == bestDy && dx < bestDx))))
                        {
                            bestSq = sq;
                            bestDx = dx;
                            bestDy = dy;
                            found = true;
                        }
                    }
                }
                if (found)
                {
                    x += bestDx;
                    y += bestDy;
                    outMoved = true;
                    return true;
                }
            }
            return false;
        }

        // True if any tile within `radius` (Chebyshev, centre included) is standable.
        bool hasWalkableNeighbor(const CollisionLookup &collision, int x, int y, int plane,
                                 int radius)
        {
            for (int dx = -radius; dx <= radius; ++dx)
            {
                for (int dy = -radius; dy <= radius; ++dy)
                {
                    if (collision.isWalkable(x + dx, y + dy, plane))
                    {
                        return true;
                    }
                }
            }
            return false;
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
