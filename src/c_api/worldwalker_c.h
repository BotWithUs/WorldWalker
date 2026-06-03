/*
 * WorldWalker C ABI — stable extern "C" surface intended for FFI consumers
 * (Java + Project Panama, .NET P/Invoke, Python ctypes, etc.).
 *
 * Conventions (mirroring NXTCacheLibrary's nxtcache_c.h)
 * -----------------------------------------------------
 *  - Lifetimes: opaque handles created by ww_artifact_open / ww_context_pool_create
 *    must be released with their matching _close / _destroy. Output buffers from
 *    getters are allocated by the library and released with ww_free.
 *  - Strings: char* outputs are NUL-terminated UTF-8.
 *  - Errors: functions returning ww_result return WW_OK (0) on success, non-zero
 *    on failure. Call ww_last_error() (thread-local) for a human-readable message.
 *    Functions returning a handle return NULL on failure (also sets last error).
 *  - Threading: the artifact is immutable and safe to share across threads. A
 *    single search context (borrowed from a pool) is NOT safe for concurrent use;
 *    the pool hands out one per query. ww_last_error()'s buffer is thread-local.
 *  - Naming: opaque handles are snake_case (ww_artifact, ww_context_pool). POD
 *    wire shapes used by the executor and the query surface are PascalCase
 *    (WwTile, WwGoal, WwEvent, WwCallbacks, WwStep, WwPath…) so the C++
 *    runtime layer can typedef-alias them and share storage byte-for-byte —
 *    see exec/Callbacks.h.
 *
 * Surfaces published here:
 *   Phase 4e — executor (ww_executor_run, WwCallbacks, WwEvent, WwGoal, …)
 *   Phase 5a — query    (ww_query, ww_path_free, WwStep, WwPath)
 */

#ifndef WORLDWALKER_C_H
#define WORLDWALKER_C_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
  #if defined(WORLDWALKER_BUILDING)
    #define WW_API __declspec(dllexport)
  #else
    #define WW_API __declspec(dllimport)
  #endif
#else
  #define WW_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Baked-artifact format version. ww_artifact_open refuses a mapping whose
   header version does not match this — a schema-breaking change bumps it so
   the host fails loud rather than misreading bytes. */
#define WW_ARTIFACT_FORMAT_VERSION 2u

typedef struct ww_artifact      ww_artifact;
typedef struct ww_context_pool  ww_context_pool;
typedef int                     ww_result;

#define WW_OK            0
#define WW_ERR_INVALID   1   /* invalid argument or handle */
#define WW_ERR_NOT_FOUND 2   /* requested entry doesn't exist */
#define WW_ERR_VERSION   3   /* artifact format version mismatch */
#define WW_ERR_IO        4   /* file / mapping I/O failure */
#define WW_ERR_INTERNAL  5   /* unexpected internal failure */

/* ---- Error + memory ----------------------------------------------------- */

/* Last error message produced on this thread, or "" if none. Owned by the
   library — do not free.
   IMPORTANT: the buffer is thread-local on the OS calling thread. Read it
   ONLY on the same thread that just made the failing call (and before that
   thread makes another library call), otherwise you may read another
   thread's stale message. For Java / virtual-thread callers, the carrier is
   pinned across the downcall so consulting ww_last_error() in the same
   try/catch as the call is safe; reading it out-of-band is not. */
WW_API const char *ww_last_error(void);

/* Free a buffer returned by any getter. Safe to pass NULL. */
WW_API void ww_free(void *ptr);

/* ---- Artifact lifecycle ------------------------------------------------- */

/* Open (memory-map) a baked artifact from disk. Returns NULL on failure
   (call ww_last_error). The handle is immutable and may be shared across
   threads; release it with ww_artifact_close once no query is using it. */
WW_API ww_artifact *ww_artifact_open(const char *path);

/* Release an artifact handle. Safe to pass NULL. */
WW_API void ww_artifact_close(ww_artifact *artifact);

/* Load (or reload) the scripter-editable global teleports — spell_teleports.json
   and item_teleports.json in `dir` — and append them onto the artifact's
   transitions so the planner considers them and the executor can fire them.
   Re-loadable: each call replaces the previously loaded set. Missing files are
   skipped. Returns WW_OK on success (including zero teleports), an error code
   on malformed JSON (call ww_last_error).

   MUTATES the artifact: it is NOT safe to call concurrently with ww_query or
   ww_executor_run on the same artifact. The caller must serialise it against
   all in-flight queries / runs (the Java host holds its lifecycle write-lock). */
WW_API ww_result ww_artifact_load_teleports(ww_artifact *artifact, const char *dir);

/* ---- Search-context pool ------------------------------------------------ */

/* Create a bounded pool of reusable search contexts over an artifact
   (typically sized ~= hardware concurrency). Each query/executor run borrows
   one context for its duration. Returns NULL on failure. */
WW_API ww_context_pool *ww_context_pool_create(ww_artifact *artifact, size_t count);

/* Destroy a context pool. Safe to pass NULL. Must outlive every in-flight
   query that borrowed from it. */
WW_API void ww_context_pool_destroy(ww_context_pool *pool);

/* ---- Executor wire shapes ---------------------------------------------- */

/* World tile coordinate. Mirrors the runtime planner's (x, y, plane) tuple. */
typedef struct WwTile
{
    int32_t x;
    int32_t y;
    int32_t plane;
} WwTile;

/* Acceptance set for ww_executor_run. The query succeeds when the player's
   tile lies within a Chebyshev radius around (x, y, plane) on the same plane.
   radius == 0 demands the exact tile. A negative radius is treated as 0. */
typedef struct WwGoal
{
    int32_t x;
    int32_t y;
    int32_t plane;
    int32_t radius;
} WwGoal;

/* Terminal status of one ww_executor_run call. Returned as int32_t so the ABI
   is enum-agnostic; values stay in lock-step with ww::exec::WwStatus. */
#define WW_STATUS_ARRIVED   0
#define WW_STATUS_FAILED    1
#define WW_STATUS_CANCELLED 2

/* Progress-event discriminator. Reserved values for future event kinds land
   at the end; the host should treat an unknown kind as "ignore". Values stay
   in lock-step with ww::exec::WwEventKind. */
#define WW_EVENT_STEP_ADVANCED        0  /* executor advanced to a new Step in the Plan */
#define WW_EVENT_WALKING_TO_INTERACT  1  /* approaching a Transition's interact-tile */
#define WW_EVENT_TELEPORT_INITIATED   2  /* executor began running a global teleport */
#define WW_EVENT_STUCK                3  /* stuck deadline elapsed on the current step */
#define WW_EVENT_REPLAN_STARTED       4  /* re-invoking the planner in-process */
#define WW_EVENT_ARRIVED              5  /* reached the acceptance set */
#define WW_EVENT_FAILED               6  /* unrecoverable error */

/* Single progress event. stepIndex and transitionIndex are -1 when not
   applicable to the kind (e.g., Arrived has neither). `kind` is one of the
   WW_EVENT_* sentinels above. */
typedef struct WwEvent
{
    int32_t kind;
    int32_t pad;
    int32_t stepIndex;
    int32_t transitionIndex;
} WwEvent;

/* One (id, value) pair in a sparse Capability snapshot. Mirrors the
   CapabilitySnapshot setters: skill level, item count, varbit value, varp
   value — all int32 so one shape covers every kind. */
typedef struct WwCapabilityEntry
{
    int32_t id;
    int32_t value;
} WwCapabilityEntry;

/* Per-re-plan capability snapshot, pulled live through readCapability. Each
   run is a pointer + count borrowed from the host; the executor copies the
   entries it needs into a runtime::CapabilitySnapshot, then returns from
   the callback (after which the runs may be reused / freed by the host). */
typedef struct WwCapabilitySnapshot
{
    const WwCapabilityEntry *skills;
    size_t                   skillCount;
    const WwCapabilityEntry *items;
    size_t                   itemCount;
    const WwCapabilityEntry *varbits;
    size_t                   varbitCount;
    const WwCapabilityEntry *varps;
    size_t                   varpCount;
} WwCapabilitySnapshot;

/* ---- Executor callback vtable ------------------------------------------ */

/* Reads — pulled live by the executor; must be cheap and side-effect-free. */
typedef void    (*WwReadPositionFn)(void *user, WwTile *outTile);
typedef void    (*WwReadCapabilityFn)(void *user, WwCapabilitySnapshot *outSnapshot);
typedef int32_t (*WwReadVarbitFn)(void *user, int32_t id);
typedef int32_t (*WwIsInterfaceOpenFn)(void *user, int32_t interfaceId);

/* Actions — fire-and-forget; the executor sequences them with sleepTicks
   and re-polls reads between calls to detect arrival / drift / stuck. */
typedef void (*WwWalkToFn)(void *user, WwTile target);
/* interact returns non-zero if it actually issued a game action, zero if it was
   a no-op (e.g. the baked loc is absent because a door is already open). The
   executor uses this to skip the post-action settle wait when nothing was done,
   so an already-open door flows straight through instead of pausing. */
typedef int32_t (*WwInteractFn)(void *user, int32_t objectId, WwTile tile, int32_t optionIndex);
/* runChainStep dispatches one Click step of a transition's execution chain
   (e.g. a lodestone-network or spell teleport) as a generic queued game action.
   (actionId, param1, param2, param3) are the ChainStepRecord's a/b/c/d — a
   ready-to-queue action — so the host just forwards them to queue_action with
   no knowledge of components or hashes. For a component click the values are
   (COMPONENT, option, sub_component, (iface<<16)|comp). The executor derives
   the interface-open gate from param3>>16 when actionId==COMPONENT, so the host
   needs no access to the artifact's chain data. */
typedef void (*WwRunChainStepFn)(void *user, int32_t actionId, int32_t param1,
                                 int32_t param2, int32_t param3);
typedef void (*WwSleepTicksFn)(void *user, int32_t ticks);

/* Control — polled each loop turn. Returning non-zero aborts the run with
   WW_STATUS_CANCELLED at the next safe point. */
typedef int32_t (*WwShouldCancelFn)(void *user);

/* Progress — optional. NULL disables reporting. Called from the executor
   thread; must not retain the WwEvent pointer past the callback return. */
typedef void (*WwOnEventFn)(void *user, const WwEvent *event);

/* Consumer-supplied callback vtable. Every non-NULL function pointer is
   required; onEvent may be NULL. `user` is an opaque cookie threaded into
   every call. The executor never copies these fields — the vtable must
   outlive the ww_executor_run call. */
typedef struct WwCallbacks
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
} WwCallbacks;

/* ---- Executor entry ----------------------------------------------------- */

/* Block the calling thread, plan a route from the player's live position to
   `goal`, walk it (re-planning in-process as needed), and report progress
   through `callbacks`. Returns one of WW_STATUS_ARRIVED / WW_STATUS_FAILED /
   WW_STATUS_CANCELLED. On invalid arguments or an internal exception, sets
   the thread-local last error and returns WW_STATUS_FAILED. */
WW_API int32_t ww_executor_run(ww_artifact      *artifact,
                                ww_context_pool *pool,
                                WwGoal           goal,
                                const WwCallbacks *callbacks);

/* ---- Query result shapes ----------------------------------------------- */

/* StepKind discriminator. Values stay in lock-step with
   ww::runtime::StepKind. */
#define WW_STEP_KIND_WALK       0  /* move toward (targetX, targetY, plane); arrival ends the step */
#define WW_STEP_KIND_TRANSITION 1  /* at (targetX, targetY, plane), invoke the transitionIndex'th
                                      TransitionRecord and run its embedded chain */

/* One emitted action of an assembled Plan. Mirrors ww::runtime::Step
   byte-for-byte (asserted at the C++ side). transitionIndex is UINT32_MAX
   for Walk steps. */
typedef struct WwStep
{
    uint8_t  kind;             /* WW_STEP_KIND_* */
    uint8_t  plane;
    uint16_t pad;              /* zero-filled */
    int32_t  targetX;
    int32_t  targetY;
    uint32_t transitionIndex;
} WwStep;

/* An assembled path returned by ww_query. The steps buffer is owned by the
   library and must be released with ww_path_free; stepCount may be zero
   (start == goal at the area level) on a WW_OK result. */
typedef struct WwPath
{
    WwStep *steps;
    size_t  stepCount;
    float   cost;
    int32_t pad;               /* keeps the struct naturally 8-byte aligned */
} WwPath;

/* ---- Query entry -------------------------------------------------------- */

/* Plan a route from `start` to `goal` against the immutable artifact, using
   one borrowed search context from `pool`. The goal's radius field is
   currently ignored — the planner plans to (goal.x, goal.y, goal.plane)
   exactly — and is reserved for a future "plan to acceptance set" pass.
   `capabilities` may be NULL, in which case all requirement-gated transitions
   are admitted. On WW_OK, *outPath holds a malloc'd steps array (release
   with ww_path_free); on any error, outPath is left zero-initialised.
   Returns WW_OK on success, WW_ERR_INVALID for null/invalid arguments,
   WW_ERR_NOT_FOUND when no route exists, WW_ERR_INTERNAL on an unexpected
   exception. */
WW_API ww_result ww_query(ww_artifact                *artifact,
                           ww_context_pool            *pool,
                           WwTile                      start,
                           WwGoal                      goal,
                           const WwCapabilitySnapshot *capabilities,
                           WwPath                     *outPath);

/* Release a path produced by ww_query and zero its fields. Safe to call on
   a zero-initialised WwPath or with path == NULL. */
WW_API void ww_path_free(WwPath *path);

#ifdef __cplusplus
}  /* extern "C" */

/* Pin the wire layout from inside the C ABI header — the static_asserts fire
   in every translation unit that includes the header (in C++ mode), so a
   future edit that grows / reorders a field can't ship without also updating
   the Java MemoryLayout side. The values match WorldWalkerLayouts.assertSize
   on the Java side.

   sizeof(bool) is implementation-defined in pure C; we don't use it in any of
   these structs. */
static_assert(sizeof(WwTile)              == 12, "WwTile must be 12 bytes (wire)");
static_assert(sizeof(WwGoal)              == 16, "WwGoal must be 16 bytes (wire)");
static_assert(sizeof(WwEvent)             == 16, "WwEvent must be 16 bytes (wire)");
static_assert(sizeof(WwCapabilityEntry)   == 8,  "WwCapabilityEntry must be 8 bytes (wire)");
static_assert(sizeof(WwCapabilitySnapshot) == 64, "WwCapabilitySnapshot must be 64 bytes (wire)");
static_assert(sizeof(WwCallbacks)         == 88, "WwCallbacks must be 88 bytes (wire) — 11 ptrs of 8 bytes each on x64");
static_assert(sizeof(WwStep)              == 16, "WwStep must be 16 bytes (wire)");
static_assert(sizeof(WwPath)              == 24, "WwPath must be 24 bytes (wire) — ptr+size_t+float+pad");
#endif

#endif  /* WORLDWALKER_C_H */
