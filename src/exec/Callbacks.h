#ifndef WORLDWALKER_EXEC_CALLBACKS_H
#define WORLDWALKER_EXEC_CALLBACKS_H

#include "c_api/worldwalker_c.h"

#include <cstdint>

// C++ view of the executor wire-shapes that are canonically defined in the
// flat C header (c_api/worldwalker_c.h). The C surface is the single source of
// truth so a Java/Panama consumer and the C++ executor read the same bytes;
// here we re-publish the structs and callback typedefs into the ww::exec
// namespace via using-aliases and keep the C++-friendly strongly-typed enums
// for WwStatus / WwEventKind, with static_asserts pinning them to the C
// WW_STATUS_* / WW_EVENT_* sentinels.

namespace ww::exec
{
    using ::WwTile;
    using ::WwGoal;
    using ::WwEvent;
    using ::WwCapabilityEntry;
    using ::WwCapabilitySnapshot;

    static_assert(sizeof(WwTile)               == 12, "WwTile must be 12 bytes (wire layout)");
    static_assert(sizeof(WwGoal)               == 16, "WwGoal must be 16 bytes (wire layout)");
    static_assert(sizeof(WwEvent)              == 16, "WwEvent must be 16 bytes (wire layout)");
    static_assert(sizeof(WwCapabilityEntry)    == 8,  "WwCapabilityEntry must be 8 bytes");

    // Strongly-typed enum mirror of the WW_STATUS_* sentinels. int32_t
    // underlying type matches what ww_executor_run returns, so the C ABI sees
    // identical bits regardless of which view callers use.
    enum class WwStatus : int32_t
    {
        Arrived   = WW_STATUS_ARRIVED,
        Failed    = WW_STATUS_FAILED,
        Cancelled = WW_STATUS_CANCELLED,
    };

    // Strongly-typed enum mirror of the WW_EVENT_* sentinels. Stored in
    // WwEvent::kind as int32_t on the wire.
    enum class WwEventKind : int32_t
    {
        StepAdvanced       = WW_EVENT_STEP_ADVANCED,
        WalkingToInteract  = WW_EVENT_WALKING_TO_INTERACT,
        TeleportInitiated  = WW_EVENT_TELEPORT_INITIATED,
        Stuck              = WW_EVENT_STUCK,
        ReplanStarted      = WW_EVENT_REPLAN_STARTED,
        Arrived            = WW_EVENT_ARRIVED,
        Failed             = WW_EVENT_FAILED,
    };

    using ::WwReadPositionFn;
    using ::WwReadCapabilityFn;
    using ::WwReadVarbitFn;
    using ::WwReadVarbitsFn;
    using ::WwReadItemCountsFn;
    using ::WwIsInterfaceOpenFn;
    using ::WwWalkToFn;
    using ::WwInteractFn;
    using ::WwRunChainStepFn;
    using ::WwSleepTicksFn;
    using ::WwShouldCancelFn;
    using ::WwOnEventFn;

    // Alias for the C vtable so existing C++ code reads ww::exec::Callbacks
    // while sharing the byte layout with WwCallbacks at the FFI boundary.
    using Callbacks = ::WwCallbacks;
}

#endif  // WORLDWALKER_EXEC_CALLBACKS_H
