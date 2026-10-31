#ifndef WORLDWALKER_RUNTIME_TRANSITIONSHAPE_H
#define WORLDWALKER_RUNTIME_TRANSITIONSHAPE_H

#include "data/Transitions.h"
#include "format/Artifact.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace ww::runtime
{
    // The farthest (Chebyshev, origin->dest) a same-floor crossing hops. Doors
    // and gates step you onto the tile just past the wall; the dest may have
    // snapped a tile or two during the bake, so the bound is loose enough to
    // admit those without letting long Transport links (mine-cart rides, cave
    // mouths, levers that fling you across the map) masquerade as doors.
    inline constexpr int32_t kMaxSameFloorHop = 4;

    // How far (Chebyshev, origin->dest) tx carries the player, ignoring plane.
    inline int32_t transitionHop(const format::TransitionRecord &tx)
    {
        return std::max(std::abs(tx.originX - tx.destX), std::abs(tx.originY - tx.destY));
    }

    // True when tx is a door in the graph sense: a local-origin Transport that
    // stays on one plane and hops a short distance, so you interact with a loc
    // to cross a same-floor barrier. In the curated dataset this one category
    // (all shape 10) covers literal doors and gates, wilderness-wall crossings
    // and stepping-stone / interactive-scenery shortcuts; shape does NOT tell
    // them apart, so it is not looked at. Ladders and stairs (plane change),
    // teleports (global origin) and long Transport rides are excluded.
    //
    // The distinction matters at run time: a missing door loc usually means
    // the door is already open and the walk can flow through, while a missing
    // ladder or cave mouth means there is no way across at all.
    inline bool isSameFloorCrossing(const format::TransitionRecord &tx)
    {
        if (static_cast<data::TransitionKind>(tx.kind) != data::TransitionKind::Transport)
        {
            return false;
        }
        if ((tx.flags & format::kTransitionFlagGlobalOrigin) != 0u)
        {
            return false;
        }
        if (tx.originPlane != tx.destPlane)
        {
            return false;
        }
        const int32_t hop = transitionHop(tx);
        return hop >= 1 && hop <= kMaxSameFloorHop;
    }

    // True when tx is a local loc that carries the player along one plane
    // farther than any same-floor crossing: an agility obstacle such as
    // Everlight's 1x5 bridge (6 tiles). The game holds the player still on
    // its near end, or on the obstacle's own solid tiles, for a few ticks
    // before the move, so standing still there is not a landing.
    inline bool isLongLocalHop(const format::TransitionRecord &tx)
    {
        if ((tx.flags & format::kTransitionFlagGlobalOrigin) != 0u)
        {
            return false;
        }
        return tx.originPlane == tx.destPlane && transitionHop(tx) > kMaxSameFloorHop;
    }
}

#endif  // WORLDWALKER_RUNTIME_TRANSITIONSHAPE_H
