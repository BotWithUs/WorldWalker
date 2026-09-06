#ifndef WORLDWALKER_DATA_TRANSITIONCOST_H
#define WORLDWALKER_DATA_TRANSITIONCOST_H

#include "data/Transitions.h"

// Per-kind default tick costs and the cost formula. Shared by wwbuild
// (finalizeTransitions) and the runtime teleport loader so both compute
// identical costs — a single source of truth avoids the two paths drifting.
//
// Cost is the per-kind base plus the sum of the chain's Wait ticks. The
// datasets DO carry `cost` / `cost_quick` / `default_cost` fields, but those
// are the legacy nav stack's unit (hundreds per teleport, not ticks) and the
// loader deliberately ignores them rather than mix units. If a tick-denominated
// field is ever added, route it through Transition::cost at parse time AND
// short-circuit here on a negative sentinel (cost==0 is not a safe sentinel:
// a legitimate zero-cost transition would clash).
namespace ww::data
{
    inline constexpr float kTransportTicks = 3.0f;
    inline constexpr float kFairyRingTicks = 5.0f;
    inline constexpr float kTeleportChainTicks = 5.0f;
    inline constexpr float kSpellBaseTicks = 1.0f;
    inline constexpr float kLodestoneBaseTicks = 1.0f;
    inline constexpr float kItemTeleportBaseTicks = 1.0f;

    inline float baseTicks(TransitionKind kind)
    {
        switch (kind)
        {
            case TransitionKind::Transport:     return kTransportTicks;
            case TransitionKind::FairyRing:     return kFairyRingTicks;
            case TransitionKind::TeleportChain: return kTeleportChainTicks;
            case TransitionKind::Spell:         return kSpellBaseTicks;
            case TransitionKind::Lodestone:     return kLodestoneBaseTicks;
            case TransitionKind::ItemTeleport:  return kItemTeleportBaseTicks;
        }
        return kTransportTicks;
    }

    inline float computeCost(const Transition &t)
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
}

#endif  // WORLDWALKER_DATA_TRANSITIONCOST_H
