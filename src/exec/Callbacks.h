#ifndef WORLDWALKER_EXEC_CALLBACKS_H
#define WORLDWALKER_EXEC_CALLBACKS_H

#include "c_api/worldwalker_c.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/InstanceMap.h"

#include <cstddef>
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
    using ::WwInstanceChunks;

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
    using ::WwReadInstanceFn;
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

    // Copy a host-supplied dynamic-region descriptor grid into `map`, or clear
    // the map when the scene is static. A null struct or a null descriptors
    // pointer is the ordinary case — an overworld scene — not an error.
    //
    // The copy is what makes the host's borrowing contract cheap to honour: the
    // grid only has to outlive this call, not the whole run.
    //
    // Shared by both entry points so the planner (ww_query_ex) and the executor
    // (per re-plan) install a grid the same way.
    inline void installInstance(runtime::InstanceMap &outMap, const WwInstanceChunks *chunks)
    {
        if (chunks == nullptr || chunks->descriptors == nullptr)
        {
            outMap.clear();
            return;
        }
        outMap.assign(chunks->originMapX, chunks->originMapY,
                      chunks->gridW, chunks->gridH,
                      chunks->descriptors, chunks->descriptorCount);
    }

    // Apply one wire-shape capability run onto `outSnapshot` through `apply`.
    // A null array with a non-zero count is the host's bug; it is treated as
    // an empty run so a malformed snapshot cannot dereference past null.
    template <typename ApplyFn>
    void applyCapabilityRun(const WwCapabilityEntry *entries, std::size_t count, ApplyFn apply)
    {
        if (entries == nullptr)
        {
            return;
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            apply(entries[i].id, entries[i].value);
        }
    }

    // Copy the wire-shape entries of `src` into `outSnapshot`: each id/value
    // pair becomes a setSkillLevel / setItemCount / setVarbit / setVarp call.
    // The one translation from the C snapshot to the runtime one — the query
    // entry (ww_query_ex) and the executor's per-plan pull both come through
    // here, so the null-run rule above holds at both.
    inline void copyCapabilities(const WwCapabilitySnapshot &src,
                                 runtime::CapabilitySnapshot &outSnapshot)
    {
        applyCapabilityRun(src.skills, src.skillCount,
                           [&](int32_t id, int32_t v) { outSnapshot.setSkillLevel(id, v); });
        applyCapabilityRun(src.items, src.itemCount,
                           [&](int32_t id, int32_t v) { outSnapshot.setItemCount(id, v); });
        applyCapabilityRun(src.varbits, src.varbitCount,
                           [&](int32_t id, int32_t v) { outSnapshot.setVarbit(id, v); });
        applyCapabilityRun(src.varps, src.varpCount,
                           [&](int32_t id, int32_t v) { outSnapshot.setVarp(id, v); });
    }
}

#endif  // WORLDWALKER_EXEC_CALLBACKS_H
