#include "data/TransitionBuilder.h"

#include "build/CollisionLookup.h"
#include "data/Transitions.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <unordered_set>
#include <utility>

namespace ww::data
{
    namespace
    {
        using ww::build::CollisionLookup;

        // Tunables (plan follow-up): per-kind default tick costs and the endpoint
        // snap radius. Dataset `cost` floats are a legacy heuristic unit, not ticks,
        // so cost is derived here from chain wait-steps plus these defaults.
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

        // Move (x, y) to the nearest standable tile within `radius` (Chebyshev),
        // searching outward ring by ring. Returns false if none is walkable.
        bool snapToWalkable(const CollisionLookup &collision, int &x, int &y, int plane,
                            int radius, bool &outMoved)
        {
            if (collision.isWalkable(x, y, plane))
            {
                return true;
            }
            for (int r = 1; r <= radius; ++r)
            {
                for (int dx = -r; dx <= r; ++dx)
                {
                    for (int dy = -r; dy <= r; ++dy)
                    {
                        if (std::max(std::abs(dx), std::abs(dy)) != r)
                        {
                            continue;
                        }
                        if (collision.isWalkable(x + dx, y + dy, plane))
                        {
                            x += dx;
                            y += dy;
                            outMoved = true;
                            return true;
                        }
                    }
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

        void hashBytes(uint64_t &h, const void *data, std::size_t n)
        {
            const auto *p = static_cast<const uint8_t *>(data);
            for (std::size_t i = 0; i < n; ++i)
            {
                h ^= p[i];
                h *= 1099511628211ull;
            }
        }

        template <typename T>
        void hashPod(uint64_t &h, const T &value)
        {
            hashBytes(h, &value, sizeof(T));
        }

        // Full-content fingerprint. Two transitions sharing it are exact duplicates;
        // this only ever collapses true duplicates, never merges distinct edges that
        // happen to share origin+dest+kind (e.g. two spells to the same tile).
        uint64_t fingerprint(const Transition &t)
        {
            uint64_t h = 1469598103934665603ull;
            hashPod(h, t.kind);
            hashPod(h, t.isGlobalOrigin);
            hashPod(h, t.originX);
            hashPod(h, t.originY);
            hashPod(h, t.originPlane);
            hashPod(h, t.destX);
            hashPod(h, t.destY);
            hashPod(h, t.destPlane);
            hashPod(h, t.objectId);
            hashPod(h, t.shape);
            hashPod(h, t.rotation);
            hashPod(h, t.optionIndex);
            hashBytes(h, t.code, sizeof(t.code));
            for (const Requirement &r : t.requirements)
            {
                hashPod(h, r.kind);
                hashPod(h, r.id);
                hashPod(h, r.amount);
            }
            for (const ChainStep &s : t.chain)
            {
                hashPod(h, s.kind);
                hashPod(h, s.a);
                hashPod(h, s.b);
                hashPod(h, s.c);
            }
            return h;
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

        std::unordered_set<uint64_t> seen;
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
            if (!seen.insert(fingerprint(t)).second)
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
