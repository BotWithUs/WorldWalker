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
 *   describe            (ww_transition_describe, WwTransitionInfo) — what a
 *                       WwStep / WwEvent transitionIndex refers to
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
#define WW_ARTIFACT_FORMAT_VERSION 3u

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

   A row whose requirements cannot be parsed (an unknown key, an incomplete
   `extra`, ...) does not fail the load: that row alone is left out, so it is
   never planned. On WW_OK, ww_last_error() is therefore a load report rather
   than an error: "" when every row loaded, otherwise one line beginning
   "ww_artifact_load_teleports: N teleport row(s) excluded: " and naming each
   row by file, JSON path and offending key (the first 16; the rest counted).
   Hosts should log a non-empty report: those teleports are unavailable until
   the file is fixed.

   MUTATES the artifact, and serialises itself: the call takes the artifact's
   lifecycle lock exclusively, so it waits for every in-flight ww_query /
   ww_executor_run on the same artifact to return (a run lasts the whole walk)
   and holds new ones off until the reload completes. Hosts need no lock of
   their own; one is harmless. */
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
   WW_EVENT_* sentinels above.

   interactionHint (the old zero `pad`, same offset and size) tells a host how
   near the next interaction is, so it can hold back movement abilities that
   would overshoot it. On a Walk step's STEP_ADVANCED it is 1 + the path tiles
   from the clicked target to the next interaction, capped at 1001: 1 means the
   clicked target is itself the approach tile of the next Transition, or the
   goal. The next interaction is the next Transition step, or the goal when no
   Transition follows before the plan ends. Path tiles are summed as Chebyshev
   distances between consecutive step targets. 0 on every other event, and
   from older DLLs, means unknown. A Walk step's STEP_ADVANCED is emitted
   before its walkTo; a re-click of the same step (stall, dialog resume) calls
   walkTo again with no new event, so the last hint still holds. */
typedef struct WwEvent
{
    int32_t kind;
    int32_t interactionHint;
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
   run is a pointer + count borrowed from the host. NOTE the ordering: the
   readCapability callback returns FIRST, and only then does the executor copy
   the entries it needs into a runtime::CapabilitySnapshot — so the arrays must
   stay valid past the callback's return, until the executor invokes the next
   callback on this run (the copy provably precedes any further call). A
   host arena freed when the upcall returns is NOT safe backing storage. */
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

/* The client's dynamic-region ("instance") chunk-descriptor grid — the table
   that says which static 8x8 chunk each chunk of the current scene was copied
   from, and how it was rotated. A player-owned house, a Dungeoneering floor and
   a boss instance are all assembled this way.

   WorldWalker's baked artifact describes the static map only, so inside an
   instance it reads collision through this table. Pass a zeroed struct (or a
   NULL descriptors pointer) for an ordinary static scene.

   UNITS TRAP, and it has caught every implementation of this so far:
   originMapX/originMapY are MAPSQUARES (64 tiles); gridW/gridH are CHUNKS
   (8 tiles). The origin must be promoted to chunks before it is subtracted.

   `descriptors` is plane-major — cell (gx, gy) on plane p lives at
   ((p * gridW) + gx) * gridH + gy — and must hold at least
   4 * gridW * gridH entries; a shorter run is refused outright rather than
   partially installed, because the agent leaves the tail of its published array
   stale rather than clearing it every tick, so reading past the count resolves
   to the PREVIOUS instance's tiles.

   BORROWING: `descriptors` only has to outlive THIS call. Note that is a weaker
   requirement than WwCapabilitySnapshot's above, and deliberately so: the
   library deep-copies the grid into its own storage synchronously — the
   executor copies on the statement after readInstance returns, and ww_query_ex
   copies before it plans — so nothing retains the host pointer. A host buffer
   reused or freed as soon as the call returns is safe backing storage, and a
   host that allocates a fresh grid per call to satisfy the stricter capability
   contract is paying for nothing. */
typedef struct WwInstanceChunks
{
    int32_t        originMapX;      /* min loaded MAPSQUARE X — the grid origin */
    int32_t        originMapY;
    int32_t        gridW;           /* descriptor grid width in CHUNKS */
    int32_t        gridH;           /* descriptor grid height in CHUNKS */
    const int32_t *descriptors;     /* plane-major; NULL for a static scene */
    size_t         descriptorCount;
} WwInstanceChunks;

/* ---- Executor callback vtable ------------------------------------------ */

/* Reads — pulled live by the executor; must be cheap and side-effect-free. */
typedef void    (*WwReadPositionFn)(void *user, WwTile *outTile);
typedef void    (*WwReadCapabilityFn)(void *user, WwCapabilitySnapshot *outSnapshot);
/* The current scene's dynamic-region descriptor grid, pulled at every (re-)plan
   alongside readCapability. Write a zeroed struct (or leave descriptors NULL)
   when the scene is not an instance — that is the common case and the executor
   treats it as "plan against the static map".

   This is a callback rather than a ww_executor_run parameter because a single
   run can cross into or out of an instance (walking through a house portal), so
   the executor must re-derive it per plan instead of trusting a value captured
   at entry.

   ABI NOTE: this slot was WwReadVarbitFn, a reserved never-called scalar varbit
   read left in place "for ABI stability" after the batched readVarbits replaced
   it. Claiming it is what a reserved slot is for, and it keeps sizeof(WwCallbacks)
   at 120 bytes so the Java/Panama layout does not have to move. It IS called
   now, so unlike its predecessor it may not be NULL. */
typedef void (*WwReadInstanceFn)(void *user, WwInstanceChunks *outChunks);
/* Live count of item `itemId` the player holds (worn + carried), used to gate
   item-requirement teleports. The executor pulls only the ids that some
   requirement references, since readCapability cannot know which items matter.
   Return 0 when absent. */
typedef int32_t (*WwReadItemCountFn)(void *user, int32_t itemId);
/* Batched variants used at (re-)plan entry, where the executor pulls every id
   referenced by any transition requirement. The scalar readItemCount above
   remains for one-shot callsites (dispatchClickItem resolves the carried
   teleport item with it). `ids` is a contiguous run of `count` ids; the host
   must write exactly `count` int32_t results into `outValues` in the same
   order (sentinel 0 for "not present"). Batching collapses 25-30 sequential
   pipe round-trips per plan into one or two host-side calls, which is the
   dominant cost in pre-walk latency. */
typedef void (*WwReadVarbitsFn)(void *user, const int32_t *ids, size_t count, int32_t *outValues);
typedef void (*WwReadItemCountsFn)(void *user, const int32_t *ids, size_t count, int32_t *outValues);
/* Whether item `itemId` is currently worn (equipped), as opposed to carried in
   the backpack. Used to pick the worn-vs-backpack variant of a ClickItem chain
   step. Return non-zero if worn. */
typedef int32_t (*WwIsItemWornFn)(void *user, int32_t itemId);
/* Whether interface `interfaceId` is currently mounted in the engine's
   open-subs hashmap — the canonical "this interface is open right now" signal.
   The chain executor polls this between a click-that-opens-a-dialog and the
   click-inside-that-dialog so the second click only fires once the dialog has
   actually appeared. Return non-zero when open. */
typedef int32_t (*WwIsInterfaceOpenFn)(void *user, int32_t interfaceId);

/* Actions — fire-and-forget; the executor sequences them with sleepTicks
   and re-polls reads between calls to detect arrival / drift / stuck. */
typedef void (*WwWalkToFn)(void *user, WwTile target);
/* interact returns non-zero if it actually issued a game action, zero if it was
   a no-op (e.g. the baked loc is absent because a door is already open). The
   executor uses this to skip the post-action settle wait when nothing was done,
   so an already-open door flows straight through instead of pausing. */
typedef int32_t (*WwInteractFn)(void *user, int32_t objectId, WwTile tile, int32_t optionIndex);
/* runChainStep dispatches one host-resolved step of a transition's execution
   chain. `kind` is the ww::data::ChainStepKind discriminant; (a..i) are the
   ChainStepRecord's nine generic slots. Only the kinds the host must resolve
   against live game state reach this callback (the executor handles Wait /
   WaitInterface itself):
     - Click  (0): generic queued action — a=actionId, b..d=param1..3
       (component click = (COMPONENT, option, sub, (iface<<16)|comp)).
     - DialogueSelect (3): a=interface, b=index, c=per_page, d=next_comp,
       e=wait_ticks — the host resolves the option component against the live
       (possibly paged) dialogue and clicks it.
     - ClickItem (4): the executor has already chosen the worn-vs-backpack
       variant, so the host receives the single resolved click:
         a=interface, b=component, c=option, d=sub_component (slot fallback),
         e=special (non-zero -> COMPONENT_SPECIAL action), f=carried item id.
       When f != 0 (backpack variant) the host resolves that item's LIVE slot
       and uses it for the sub-component instead of d (the baked d is only a
       fallback); for the worn variant f is 0 and d is used as-is.
     - ClickNpc (6): the origin of a transition with no loc. The host clicks
       the nearest NPC whose type id is in [f, g] on plane d within Chebyshev
       e of (b, c), with 0-based option a (NPC_OPTIONS[a + 1]), and does
       nothing when none is found. Always a chain's first step. A host that
       predates this kind must reject it loudly, not ignore it. */
typedef void (*WwRunChainStepFn)(void *user, int32_t kind,
                                 int32_t a, int32_t b, int32_t c, int32_t d,
                                 int32_t e, int32_t f, int32_t g, int32_t h, int32_t i);
typedef void (*WwSleepTicksFn)(void *user, int32_t ticks);

/* Control — polled each loop turn. Returning non-zero aborts the run with
   WW_STATUS_CANCELLED at the next safe point. */
typedef int32_t (*WwShouldCancelFn)(void *user);

/* Progress — optional. NULL disables reporting. Called from the executor
   thread; must not retain the WwEvent pointer past the callback return. */
typedef void (*WwOnEventFn)(void *user, const WwEvent *event);

/* Consumer-supplied callback vtable. Every function pointer is required
   except onEvent (optional); ww_executor_run validates the required set and
   fails clean on a NULL. `user` is an opaque cookie threaded into every call.
   The executor never copies these fields — the vtable must outlive the
   ww_executor_run call. */
typedef struct WwCallbacks
{
    void *user;

    WwReadPositionFn    readPosition;
    WwReadCapabilityFn  readCapability;
    WwReadInstanceFn    readInstance;
    WwReadItemCountFn   readItemCount;
    WwReadVarbitsFn     readVarbits;
    WwReadItemCountsFn  readItemCounts;
    WwIsItemWornFn      isItemWorn;
    WwIsInterfaceOpenFn isInterfaceOpen;

    WwWalkToFn       walkTo;
    WwInteractFn     interact;
    WwRunChainStepFn runChainStep;
    WwSleepTicksFn   sleepTicks;

    WwShouldCancelFn shouldCancel;
    WwOnEventFn      onEvent;
} WwCallbacks;

/* ---- Movement categories ------------------------------------------------ */

/* Every transition belongs to one movement category. A `disabledMoves` mask
   (ww_executor_run_ex, ww_query_moves) with bit (1u << WW_MOVE_x) set keeps
   the planner off every transition of that category, on the first plan and
   every re-plan. 0 disables nothing. With WW_MOVE_DOORS set a route that
   needs a closed door fails rather than open it.

   Categories are read from the artifact as it is (no format change): the
   transition kind, the travel interface its chain waits on, whether it
   changes floor, and for locs the curated transport links click, a loc-id
   table generated from the loc definitions (tools/move_categories). */
#define WW_MOVE_DOORS          0u  /* doors and gates */
#define WW_MOVE_SHORTCUTS      1u  /* agility shortcuts and obstacles */
#define WW_MOVE_PLANE          2u  /* stairs, ladders, trapdoors, into dungeons */
#define WW_MOVE_CLIMBOVERS     3u  /* stiles and walls climbed or stepped over */
#define WW_MOVE_TRANSPORTS     4u  /* boats, carts, portals, other loc / NPC links */
#define WW_MOVE_TELEPORTS      5u  /* spell and item teleports */
#define WW_MOVE_LODESTONES     6u
#define WW_MOVE_FAIRY_RINGS    7u
#define WW_MOVE_SPIRIT_TREES   8u
#define WW_MOVE_GLIDERS        9u
#define WW_MOVE_CHARTERS      10u  /* charter ships */
#define WW_MOVE_MAGIC_CARPETS 11u
#define WW_MOVE_OTHER_CHAINS  12u  /* any other teleport_chains network */
#define WW_MOVE_COUNT         13u

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

/* ww_executor_run with movement categories switched off: `disabledMoves` as
   described under WW_MOVE_*. ww_executor_run is exactly this call with 0. */
WW_API int32_t ww_executor_run_ex(ww_artifact       *artifact,
                                   ww_context_pool   *pool,
                                   WwGoal             goal,
                                   const WwCallbacks *callbacks,
                                   uint32_t           disabledMoves);

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

/* ww_query against a scene that may be a dynamic region ("instance"). Identical
   to ww_query in every respect except that `instance` supplies the chunk
   descriptor grid through which collision is resolved; ww_query is exactly this
   call with instance == NULL, which is the static-scene case.

   Inside an instance the planner routes by walking only: the baked area graph,
   its transitions and the global teleports all describe the static world, and
   none of them apply to terrain assembled at runtime. A query whose start and
   goal are not BOTH inside the descriptor grid returns WW_ERR_NOT_FOUND —
   crossing an instance boundary needs an exit transition that nothing bakes
   yet. `instance` is borrowed for the duration of the call only. */
WW_API ww_result ww_query_ex(ww_artifact                *artifact,
                              ww_context_pool            *pool,
                              WwTile                      start,
                              WwGoal                      goal,
                              const WwCapabilitySnapshot *capabilities,
                              const WwInstanceChunks     *instance,
                              WwPath                     *outPath);

/* ww_query_ex with movement categories switched off: ww_query_ex's
   parameters plus a trailing `disabledMoves`, as described under WW_MOVE_*. `capabilities` may still be NULL, which admits
   every requirement gate as before while the disabled categories stay
   refused. ww_query_ex is exactly this call with 0. */
WW_API ww_result ww_query_moves(ww_artifact                *artifact,
                                 ww_context_pool            *pool,
                                 WwTile                      start,
                                 WwGoal                      goal,
                                 const WwCapabilitySnapshot *capabilities,
                                 const WwInstanceChunks     *instance,
                                 WwPath                     *outPath,
                                 uint32_t                    disabledMoves);

/* Release a path produced by ww_query and zero its fields. Safe to call on
   a zero-initialised WwPath or with path == NULL. */
WW_API void ww_path_free(WwPath *path);

/* ---- Transition description --------------------------------------------- */

/* How the executor starts a transition, and the field a consumer branches on.
   Each value is the first thing ww_executor_run does with the record:
     LOC    -> interact(objectId, originTile, optionIndex), then the chain
     NPC    -> no loc; chain[0] is a ClickNpc step (npcType* and npcSearchRadius
               below repeat its slots; originTile is the search centre)
     GLOBAL -> a teleport usable from (almost) anywhere: TELEPORT_INITIATED,
               then the chain. originTile is unused and reads zero. */
#define WW_TRANSITION_ORIGIN_LOC    0
#define WW_TRANSITION_ORIGIN_NPC    1
#define WW_TRANSITION_ORIGIN_GLOBAL 2

/* Transition kind, value for value with ww::data::TransitionKind. Every baked
   record of a current artifact is TRANSPORT (charters, gliders, fairy rings
   and the like arrive as transport links and are told apart by moveCategory);
   SPELL, LODESTONE and ITEM_TELEPORT are the runtime-loaded globals. */
#define WW_TRANSITION_KIND_TRANSPORT      0
#define WW_TRANSITION_KIND_FAIRY_RING     1
#define WW_TRANSITION_KIND_TELEPORT_CHAIN 2
#define WW_TRANSITION_KIND_SPELL          3
#define WW_TRANSITION_KIND_LODESTONE      4
#define WW_TRANSITION_KIND_ITEM_TELEPORT  5

/* Requirement predicate kind, value for value with ww::data::RequirementKind. */
#define WW_REQUIREMENT_SKILL           0  /* id = skill, amount = level */
#define WW_REQUIREMENT_ITEM            1  /* id = item, amount = count (worn + carried) */
#define WW_REQUIREMENT_VARBIT          2  /* amount = value, matched exactly */
#define WW_REQUIREMENT_VARP            3  /* amount = value, matched exactly */
#define WW_REQUIREMENT_VARBIT_AT_LEAST 4  /* amount = minimum value */
#define WW_REQUIREMENT_VARP_AT_LEAST   5  /* amount = minimum value */
#define WW_REQUIREMENT_VARP_BIT        6  /* amount = bit index (0..31) that must be set */

/* Chain-step kind, value for value with ww::data::ChainStepKind — the same
   `kind` runChainStep receives. */
#define WW_CHAIN_STEP_CLICK           0
#define WW_CHAIN_STEP_WAIT            1
#define WW_CHAIN_STEP_WAIT_INTERFACE  2
#define WW_CHAIN_STEP_DIALOGUE_SELECT 3
#define WW_CHAIN_STEP_CLICK_ITEM      4
#define WW_CHAIN_STEP_DIALOGUE_ANSWER 5  /* never baked; the executor sends it itself */
#define WW_CHAIN_STEP_CLICK_NPC       6

/* Capacity of WwTransitionInfo's arrays. Generous: the largest record in the
   artifact shipped 2026-10-05 has 4 requirements and 5 chain steps, and the
   runtime teleport datasets 2 and 4. */
#define WW_TRANSITION_MAX_REQUIREMENTS 16
#define WW_TRANSITION_MAX_CHAIN_STEPS  16

/* One requirement predicate. Every listed predicate must hold for the planner
   to admit the transition. */
typedef struct WwRequirement
{
    int32_t kind;    /* WW_REQUIREMENT_* */
    int32_t id;
    int32_t amount;
} WwRequirement;

/* One chain step, exactly as baked: the record's nine slots verbatim, keyed on
   `kind` as documented at WwRunChainStepFn and in format/Artifact.h. This is
   NOT always the call a host's runChainStep sees: WAIT and WAIT_INTERFACE are
   handled inside the executor and never reach the host, and a CLICK_ITEM
   carries both variants here (a..d worn, e..h backpack, i backpack special)
   where the executor forwards only the one isItemWorn picks. */
typedef struct WwChainStep
{
    int32_t kind;    /* WW_CHAIN_STEP_* */
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t d;
    int32_t e;
    int32_t f;
    int32_t g;
    int32_t h;
    int32_t i;
} WwChainStep;

/* Everything the artifact holds about one transition. `origin` is the
   predicate; the rest is payload. Fields that do not apply to the origin read
   -1 (ids, option, radius) or zero (tiles, shape, rotation, code).

   Entries of requirements[] at and past min(requirementCount,
   WW_TRANSITION_MAX_REQUIREMENTS), and of chain[] at and past
   min(chainStepCount, WW_TRANSITION_MAX_CHAIN_STEPS), are zero — guaranteed,
   so a consumer may hash or compare the whole struct. The counts are the
   record's true totals: a count above the capacity means the array was
   truncated, never that entries were invented.

   Not held by the artifact, so not described: the dataset row's name, a
   charter's fare surcharge (folded into cost), and rows the dataset marks
   disabled (never baked). Name locs and NPCs through NXTCache instead. */
typedef struct WwTransitionInfo
{
    int32_t  origin;            /* WW_TRANSITION_ORIGIN_* — the predicate */
    int32_t  kind;              /* WW_TRANSITION_KIND_* */
    int32_t  moveCategory;      /* WW_MOVE_* bit number, as disabledMoves sees it */
    int32_t  isRuntimeLoaded;   /* 1: appended by ww_artifact_load_teleports; 0: baked */
    WwTile   originTile;        /* LOC: the loc's tile; NPC: search centre; GLOBAL: zero */
    WwTile   destTile;
    int32_t  objectId;          /* LOC: loc id; otherwise -1 */
    int32_t  npcTypeMin;        /* NPC: inclusive NPC type id range; otherwise -1 */
    int32_t  npcTypeMax;
    int32_t  npcSearchRadius;   /* NPC: Chebyshev radius around originTile; otherwise -1 */
    int32_t  optionIndex;       /* 0-based. LOC: loc option; NPC: NPC option; GLOBAL: -1 */
    uint8_t  shape;             /* LOC: loc shape and rotation as baked; otherwise 0 */
    uint8_t  rotation;
    uint8_t  pad[2];            /* zero */
    char     code[4];           /* fairy-ring code, NUL-padded; all zero otherwise */
    float    cost;              /* tick cost the planner charges for the transition */
    uint32_t requirementCount;  /* true total; > WW_TRANSITION_MAX_REQUIREMENTS = truncated */
    uint32_t chainStepCount;    /* true total; > WW_TRANSITION_MAX_CHAIN_STEPS = truncated */
    WwRequirement requirements[WW_TRANSITION_MAX_REQUIREMENTS];
    WwChainStep   chain[WW_TRANSITION_MAX_CHAIN_STEPS];
} WwTransitionInfo;

/* Describe the transition at `transitionIndex` — the index WwStep and WwEvent
   carry (WwEvent's int32_t -1 casts to UINT32_MAX, which is never valid).

   Returns WW_OK and fills *outInfo; WW_ERR_INVALID when artifact or outInfo is
   NULL; WW_ERR_NOT_FOUND when the index is out of range (UINT32_MAX, the Walk
   step sentinel, included); WW_ERR_INTERNAL on a corrupt record. On any error
   other than a NULL outInfo, *outInfo is zeroed.

   INDEX STABILITY: indices of baked records (isRuntimeLoaded == 0) are fixed
   for the artifact's life. Runtime-loaded indices are reassigned by every
   ww_artifact_load_teleports call, so describe one before the next reload.

   LOCKING, and safe from inside an executor callback. Like ww_query it holds
   the artifact's lifecycle lock shared — except when the calling thread is
   already inside ww_executor_run / ww_executor_run_ex on the SAME artifact,
   i.e. it is called from one of that run's callbacks. The run already holds the
   lock shared on this thread, and a reload cannot proceed while it does, so
   describe reads without acquiring it again. Acquiring it again would be a
   recursive shared lock, which with a ww_artifact_load_teleports writer
   queued deadlocks. So the call is safe from any thread, inside a callback
   included, and from a callback it never waits on a reload. From any other
   thread it waits only as ww_query does: behind a reload that holds or is
   queued for the lock. Verified by `wwcli describe`, which describes from
   inside a run's readPosition while a reload is queued. */
WW_API ww_result ww_transition_describe(const ww_artifact *artifact,
                                         uint32_t           transitionIndex,
                                         WwTransitionInfo  *outInfo);

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
static_assert(sizeof(WwInstanceChunks)    == 32, "WwInstanceChunks must be 32 bytes (wire) — 4 i32 + ptr + size_t");
static_assert(sizeof(WwCallbacks)         == 120, "WwCallbacks must be 120 bytes (wire) — 15 ptrs of 8 bytes each on x64");
static_assert(sizeof(WwStep)              == 16, "WwStep must be 16 bytes (wire)");
static_assert(sizeof(WwPath)              == 24, "WwPath must be 24 bytes (wire) — ptr+size_t+float+pad");
static_assert(sizeof(WwRequirement)       == 12, "WwRequirement must be 12 bytes (wire)");
static_assert(sizeof(WwChainStep)         == 40, "WwChainStep must be 40 bytes (wire)");
static_assert(sizeof(WwTransitionInfo)    == 912, "WwTransitionInfo must be 912 bytes (wire)");

/* Every WwTransitionInfo offset, pinned so a host mirror can assert the same
   numbers. No pointers or size_t inside, so they hold on any target. */
static_assert(offsetof(WwTransitionInfo, origin)           == 0);
static_assert(offsetof(WwTransitionInfo, kind)             == 4);
static_assert(offsetof(WwTransitionInfo, moveCategory)     == 8);
static_assert(offsetof(WwTransitionInfo, isRuntimeLoaded)  == 12);
static_assert(offsetof(WwTransitionInfo, originTile)       == 16);
static_assert(offsetof(WwTransitionInfo, destTile)         == 28);
static_assert(offsetof(WwTransitionInfo, objectId)         == 40);
static_assert(offsetof(WwTransitionInfo, npcTypeMin)       == 44);
static_assert(offsetof(WwTransitionInfo, npcTypeMax)       == 48);
static_assert(offsetof(WwTransitionInfo, npcSearchRadius)  == 52);
static_assert(offsetof(WwTransitionInfo, optionIndex)      == 56);
static_assert(offsetof(WwTransitionInfo, shape)            == 60);
static_assert(offsetof(WwTransitionInfo, rotation)         == 61);
static_assert(offsetof(WwTransitionInfo, pad)              == 62);
static_assert(offsetof(WwTransitionInfo, code)             == 64);
static_assert(offsetof(WwTransitionInfo, cost)             == 68);
static_assert(offsetof(WwTransitionInfo, requirementCount) == 72);
static_assert(offsetof(WwTransitionInfo, chainStepCount)   == 76);
static_assert(offsetof(WwTransitionInfo, requirements)     == 80);
static_assert(offsetof(WwTransitionInfo, chain)            == 272);
static_assert(offsetof(WwRequirement, kind) == 0 && offsetof(WwRequirement, id) == 4
              && offsetof(WwRequirement, amount) == 8);
static_assert(offsetof(WwChainStep, kind) == 0 && offsetof(WwChainStep, a) == 4
              && offsetof(WwChainStep, i) == 36);
#endif

#endif  /* WORLDWALKER_C_H */
