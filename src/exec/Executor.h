#ifndef WORLDWALKER_EXEC_EXECUTOR_H
#define WORLDWALKER_EXEC_EXECUTOR_H

#include "exec/Callbacks.h"
#include "format/ArtifactReader.h"
#include "runtime/ContextPool.h"

namespace ww::exec
{
    // Drives one walk to completion (ADR 0008, 0010). Constructed with the
    // borrowed artifact, the borrowed search-context pool, and the host's
    // callback vtable; run(goal) owns the calling thread until arrival,
    // failure, or cancellation.
    //
    // Phase 4a scope: the executor reads the player's position, short-circuits
    // arrival when already inside the acceptance set, otherwise borrows a
    // SearchContext from the pool and asks the planner for a Plan. An empty
    // plan (planner agrees we're at the goal) reports Arrived; a non-empty
    // plan reports Failed because the walk loop is not implemented yet.
    // No action callbacks (walkTo / interact / runChainStep / sleepTicks) and
    // no shouldCancel polling are exercised until Phase 4b.
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
