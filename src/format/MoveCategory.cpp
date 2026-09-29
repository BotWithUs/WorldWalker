#include "format/MoveCategory.h"

#include "data/Transitions.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iterator>

namespace ww::format
{
    namespace
    {
        struct LocMove
        {
            int32_t      locId;
            MoveCategory category;
        };

        // Every loc a curated transport link clicks, with the category its
        // name and option text give it. Sorted by loc id.
        constexpr LocMove kLocMoves[] = {
#include "format/MoveCategoryLocs.inc"
        };

        // The travel interface each network's chain waits on, and what it is.
        struct InterfaceMove
        {
            int32_t      interfaceId;
            MoveCategory category;
        };

        constexpr InterfaceMove kInterfaceMoves[] = {
            { 1145, MoveCategory::SpiritTrees },   // spirit tree destinations
            { 138,  MoveCategory::Gliders },       // gnome glider map
            { 95,   MoveCategory::Charters },      // charter ship map
            { 1928, MoveCategory::MagicCarpets },  // magic carpet destinations
        };

        // A walk down (or up) a ladder, trapdoor or cave into the dungeon under
        // the same ground: the underground copy of the map sits 6400 tiles
        // north, on plane 0, so it reads as the same plane.
        constexpr int32_t kUndergroundOffsetY = 6400;
        constexpr int32_t kUndergroundSlack   = 64;

        // How far a stair or ladder may carry the player sideways and still be
        // one: a staircase footprint and its landing, not a ride.
        constexpr int32_t kMaxPlaneHop = 8;

        // Same-floor crossings hop at most this far (runtime::kMaxSameFloorHop).
        constexpr int32_t kMaxCrossingHop = 4;

        // Host action id for an interface-component click (a chain Click
        // whose d carries (interface << 16) | component).
        constexpr int32_t kComponentActionId = 57;

        int32_t interfaceOf(const ChainStepRecord &step)
        {
            switch (static_cast<data::ChainStepKind>(step.kind))
            {
                case data::ChainStepKind::WaitInterface:
                    return step.a;
                case data::ChainStepKind::Click:
                    return step.a == kComponentActionId ? (step.d >> 16) : -1;
                default:
                    return -1;
            }
        }

        bool findChainMove(const TransitionRecord &tx, std::span<const ChainStepRecord> chain,
                           MoveCategory &outCategory)
        {
            const uint64_t end = static_cast<uint64_t>(tx.chainStart) + tx.chainCount;
            if (end > chain.size())
            {
                return false;
            }
            for (uint64_t i = tx.chainStart; i < end; ++i)
            {
                const int32_t iface = interfaceOf(chain[i]);
                for (const InterfaceMove &m : kInterfaceMoves)
                {
                    if (m.interfaceId == iface)
                    {
                        outCategory = m.category;
                        return true;
                    }
                }
            }
            return false;
        }

        bool findLocMove(int32_t locId, MoveCategory &outCategory)
        {
            const auto it = std::lower_bound(std::begin(kLocMoves), std::end(kLocMoves), locId,
                                             [](const LocMove &m, int32_t id)
                                             { return m.locId < id; });
            if (it == std::end(kLocMoves) || it->locId != locId)
            {
                return false;
            }
            outCategory = it->category;
            return true;
        }

        bool isStairLike(const TransitionRecord &tx)
        {
            const int32_t dx = std::abs(tx.destX - tx.originX);
            const int32_t dy = tx.destY - tx.originY;
            if (tx.originPlane != tx.destPlane)
            {
                return std::max(dx, std::abs(dy)) <= kMaxPlaneHop;
            }
            return dx <= kUndergroundSlack
                && std::abs(std::abs(dy) - kUndergroundOffsetY) <= kUndergroundSlack;
        }

        // A local Transport, in order of the signal's reliability: the travel
        // interface its chain opens; the stair-like shape of a move to another
        // floor (or underground); the curated loc's own name; and last the
        // shape of a short same-floor hop, which is what the doors the bake
        // derives from the cache all are. Anything else is a ride.
        MoveCategory classifyTransport(const TransitionRecord &tx,
                                       std::span<const ChainStepRecord> chain)
        {
            MoveCategory category = MoveCategory::Transports;
            if (findChainMove(tx, chain, category))
            {
                return category;
            }
            if (isStairLike(tx))
            {
                return MoveCategory::Plane;
            }
            if (tx.objectId > 0 && findLocMove(tx.objectId, category))
            {
                return category;
            }
            const int32_t hop = std::max(std::abs(tx.destX - tx.originX),
                                         std::abs(tx.destY - tx.originY));
            const bool isCrossing = (tx.flags & kTransitionFlagGlobalOrigin) == 0u
                                 && tx.objectId > 0
                                 && tx.originPlane == tx.destPlane
                                 && hop >= 1 && hop <= kMaxCrossingHop;
            return isCrossing ? MoveCategory::Doors : MoveCategory::Transports;
        }
    }

    const char *moveCategoryName(MoveCategory category)
    {
        switch (category)
        {
            case MoveCategory::Doors:        return "doors";
            case MoveCategory::Shortcuts:    return "shortcuts";
            case MoveCategory::Plane:        return "plane";
            case MoveCategory::ClimbOvers:   return "climbovers";
            case MoveCategory::Transports:   return "transports";
            case MoveCategory::Teleports:    return "teleports";
            case MoveCategory::Lodestones:   return "lodestones";
            case MoveCategory::FairyRings:   return "fairy_rings";
            case MoveCategory::SpiritTrees:  return "spirit_trees";
            case MoveCategory::Gliders:      return "gliders";
            case MoveCategory::Charters:     return "charters";
            case MoveCategory::MagicCarpets: return "magic_carpets";
            case MoveCategory::OtherChains:  return "other_chains";
        }
        return "unknown";
    }

    MoveCategory classifyMove(const TransitionRecord &tx, std::span<const ChainStepRecord> chain)
    {
        switch (static_cast<data::TransitionKind>(tx.kind))
        {
            case data::TransitionKind::FairyRing:
                return MoveCategory::FairyRings;
            case data::TransitionKind::TeleportChain:
            {
                // teleport_chains rows of a network with its own bit keep it.
                MoveCategory category = MoveCategory::OtherChains;
                findChainMove(tx, chain, category);
                return category;
            }
            case data::TransitionKind::Spell:
            case data::TransitionKind::ItemTeleport:
                return MoveCategory::Teleports;
            case data::TransitionKind::Lodestone:
                return MoveCategory::Lodestones;
            case data::TransitionKind::Transport:
                return classifyTransport(tx, chain);
        }
        return MoveCategory::Transports;
    }
}
