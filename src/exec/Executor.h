#ifndef WORLDWALKER_EXEC_EXECUTOR_H
#define WORLDWALKER_EXEC_EXECUTOR_H

#include "exec/Callbacks.h"
#include "format/ArtifactReader.h"
#include "runtime/ContextPool.h"

namespace ww::runtime
{
    struct Step;
}

namespace ww::exec
{
    // Drives one walk to completion (ADR 0008, 0010). Constructed with the
    // borrowed artifact, the borrowed search-context pool, and the host's
    // callback vtable; run(goal) owns the calling thread until arrival,
    // failure, or cancellation.
    //
    // Phase 4b scope: the executor reads the player's position, short-circuits
    // arrival when already inside the acceptance set, borrows a SearchContext
    // from the pool, asks the planner for a Plan, and iterates the resulting
    // Walk steps. Each Walk step: walkTo(target), then poll readPosition with
    // sleepTicks between samples; arrival = within kArrivalChebyshev tiles of
    // the step target. A stalled-distance counter (no progress for N polls)
    // and a wall-clock deadline together detect "stuck" and surface as Failed;
    // shouldCancel polled before every sleep aborts with Cancelled. The pool
    // borrow is held across the loop so Phase 4d re-plans (drift / stuck /
    // teleport-allowed) can reuse the same SearchContext.
    //
    // Transition steps (kind == Transition) are still surfaced as Failed —
    // Phase 4c implements interact + chain execution. Re-plan triggers
    // (Phase 4d) and the ww_executor_run C ABI entry (Phase 4e) follow.
    //
    // Non-copyable, non-movable (it holds references to the borrowed artifact,
    // pool, and callbacks — relocation would dangle them).
    class Executor
    {
    public:
        Executor(const format::ArtifactReader &reader,
                 runtime::ContextPool &pool,
                 const Callbacks &callbacks);

        Executor(const Executor &) = delete;
        Executor &operator=(const Executor &) = delete;
        Executor(Executor &&) = delete;
        Executor &operator=(Executor &&) = delete;

        // Block the caller until the run terminates.
        WwStatus run(WwGoal goal);

    private:
        // Chebyshev arrival test against goal on the same plane.
        static bool isInsideGoal(const WwTile &tile, const WwGoal &goal);

        // Drive one Walk step to its target. Issues walkTo, then alternates
        // shouldCancel / sleepTicks / readPosition until arrival, cancellation,
        // a stalled-distance trip, or the wall-clock stuck deadline. Emits
        // StepAdvanced once at entry; emits Stuck on either failure path.
        WwStatus walkOneStep(const runtime::Step &step, int32_t stepIndex);

        // Emit one progress event when the host wired onEvent; no-op otherwise.
        // stepIndex / transitionIndex default to -1 to signal "not applicable".
        void emit(WwEventKind kind,
                  int32_t stepIndex = -1,
                  int32_t transitionIndex = -1) const;

        const format::ArtifactReader *artifact;
        runtime::ContextPool *pool;
        const Callbacks *callbacks;
    };
}

#endif  // WORLDWALKER_EXEC_EXECUTOR_H
