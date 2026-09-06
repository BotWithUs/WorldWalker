#ifndef WORLDWALKER_DATA_CROSSINGSTAMP_H
#define WORLDWALKER_DATA_CROSSINGSTAMP_H

#include "build/CacheClient.h"
#include "data/Transitions.h"

namespace ww::data
{
    // Stamp the interactable loc of a cache Crossing (id, shape, rotation,
    // option slot) onto a derived transition so the executor can click it.
    //
    // Returns false, leaving `outTransition` untouched, when the crossing
    // carries no option slot (kCrossingNoOption: a varbit-only door, a loc with
    // no clickable action). A hop through such a loc cannot be performed, so
    // the deriver must drop the candidate rather than bake an edge the planner
    // will route through and the executor will then stall at until its re-plan
    // budget runs out. Both derivers (doors and vertical ladders/stairs) come
    // through here so the rule cannot drift between them.
    inline bool stampCrossingLoc(const ww::build::Crossing &crossing, Transition &outTransition)
    {
        if (crossing.optionIndex == ww::build::kCrossingNoOption)
        {
            return false;
        }
        outTransition.objectId = crossing.objectId;
        outTransition.shape = crossing.shape;
        outTransition.rotation = crossing.rotation;
        outTransition.optionIndex = crossing.optionIndex;
        return true;
    }
}

#endif  // WORLDWALKER_DATA_CROSSINGSTAMP_H
