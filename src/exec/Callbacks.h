#ifndef WORLDWALKER_EXEC_CALLBACKS_H
#define WORLDWALKER_EXEC_CALLBACKS_H

#include <cstddef>
#include <cstdint>

// POD wire-shapes the WorldWalker executor exchanges with its host. These types
// are deliberately C-ABI-shaped (plain integers + pointers, fixed sizes, no
// virtual tables, no STL); Phase 5 publishes them through worldwalker_c.h with
// nothing more than a using-alias so the runtime layout matches byte-for-byte.
//
// Function-pointer typedefs are declared with `extern "C"` linkage so the same
// callback addresses passed from a C consumer through the C ABI can be invoked
// here without any thunk.

namespace ww::exec
{
    // World tile coordinate. Mirrors the runtime planner's (x, y, plane) tuple.
    struct WwTile
    {
        int32_t x;
        int32_t y;
        int32_t plane;
    };

    static_assert(sizeof(WwTile) == 12, "WwTile must be 12 bytes (wire layout)");

    // Acceptance set for ww_executor_run. The query succeeds when the player's
    // tile lies within a Chebyshev radius around (x, y, plane) on the same plane.
    // radius == 0 demands the exact tile. A negative radius is treated as 0.
    struct WwGoal
    {
        int32_t x;
        int32_t y;
        int32_t plane;
        int32_t radius;
    };

    static_assert(sizeof(WwGoal) == 16, "WwGoal must be 16 bytes (wire layout)");

    // Terminal status of one ww_executor_run call.
    enum class WwStatus : int32_t
    {
        Arrived   = 0,
        Failed    = 1,
        Cancelled = 2,
    };

    // Progress-event discriminator. Reserved values for future event kinds land
    // at the end; the host should treat an unknown kind as "ignore".
    enum class WwEventKind : int32_t
    {
        StepAdvanced       = 0,  // executor advanced to a new Step in the Plan
        WalkingToInteract  = 1,  // approaching a Transition's interact-tile
        TeleportInitiated  = 2,  // executor began running a global teleport
        Stuck              = 3,  // stuck deadline elapsed on the current step
        ReplanStarted      = 4,  // re-invoking the planner in-process
        Arrived            = 5,  // reached the acceptance set
        Failed             = 6,  // unrecoverable error (out: planner returned false, etc.)
    };

    // Single progress event. stepIndex and transitionIndex are -1 when not
    // applicable to the kind (e.g., StepAdvanced has both; Arrived has neither).
    struct WwEvent
    {
        WwEventKind kind;
        int32_t     pad;
        int32_t     stepIndex;
        int32_t     transitionIndex;
    };

    static_assert(sizeof(WwEvent) == 16, "WwEvent must be 16 bytes (wire layout)");

    // One (id, value) pair in a sparse Capability snapshot. Mirrors the
    // CapabilitySnapshot setters: skill level, item count, varbit value, varp
    // value — all int32 so one shape covers every kind.
    struct WwCapabilityEntry
    {
        int32_t id;
        int32_t value;
    };

    static_assert(sizeof(WwCapabilityEntry) == 8, "WwCapabilityEntry must be 8 bytes");

    // Per-re-plan capability snapshot, pulled live through readCapability. Each
    // run is a pointer + count borrowed from the host; the executor copies the
    // entries it needs into a runtime::CapabilitySnapshot, then returns from
    // the callback (after which the runs may be reused / freed by the host).
    struct WwCapabilitySnapshot
    {
        const WwCapabilityEntry *skills;
        std::size_t              skillCount;
        const WwCapabilityEntry *items;
        std::size_t              itemCount;
        const WwCapabilityEntry *varbits;
        std::size_t              varbitCount;
        const WwCapabilityEntry *varps;
        std::size_t              varpCount;
    };

    extern "C"
    {
        // Reads — pulled live by the executor; must be cheap and side-effect-free.
        using WwReadPositionFn    = void    (*)(void *user, WwTile *outTile);
        using WwReadCapabilityFn  = void    (*)(void *user, WwCapabilitySnapshot *outSnapshot);
        using WwReadVarbitFn      = int32_t (*)(void *user, int32_t id);
        using WwIsInterfaceOpenFn = int32_t (*)(void *user, int32_t interfaceId);

        // Actions — fire-and-forget; the executor sequences them with sleepTicks
        // and re-polls reads between calls to detect arrival / drift / stuck.
        using WwWalkToFn       = void (*)(void *user, WwTile target);
        using WwInteractFn     = void (*)(void *user, int32_t objectId, WwTile tile, int32_t optionIndex);
        using WwRunChainStepFn = void (*)(void *user, int32_t chainIndex, int32_t stepIndex);
        using WwSleepTicksFn   = void (*)(void *user, int32_t ticks);

        // Control — polled each loop turn. Returning non-zero aborts the run with
        // WwStatus::Cancelled at the next safe point.
        using WwShouldCancelFn = int32_t (*)(void *user);

        // Progress — optional. Null disables reporting. Called from the executor
        // thread; must not retain the WwEvent pointer past the callback return.
        using WwOnEventFn = void (*)(void *user, const WwEvent *event);
    }

    // Consumer-supplied callback vtable. Every non-null function pointer is
    // required; onEvent may be null. `user` is an opaque cookie threaded into
    // every call. The executor never copies these fields — the vtable must
    // outlive the ww_executor_run call.
    struct Callbacks
    {
        void *user;

        WwReadPositionFn    readPosition;
        WwReadCapabilityFn  readCapability;
        WwReadVarbitFn      readVarbit;
        WwIsInterfaceOpenFn isInterfaceOpen;

        WwWalkToFn       walkTo;
        WwInteractFn     interact;
        WwRunChainStepFn runChainStep;
        WwSleepTicksFn   sleepTicks;

        WwShouldCancelFn shouldCancel;
        WwOnEventFn      onEvent;
    };
}

#endif  // WORLDWALKER_EXEC_CALLBACKS_H
