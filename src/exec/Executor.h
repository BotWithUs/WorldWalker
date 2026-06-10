#ifndef WORLDWALKER_EXEC_EXECUTOR_H
#define WORLDWALKER_EXEC_EXECUTOR_H

#include "exec/Callbacks.h"
#include "format/ArtifactReader.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/ContextPool.h"
#include "runtime/PathAssembler.h"

#include <cstdint>
#include <span>
#include <vector>

namespace ww::format
{
    struct ChainStepRecord;
    struct TransitionRecord;
}

namespace ww::runtime
{
    struct Step;
    struct SearchContext;
}

namespace ww::exec
{
    // Drives one walk to completion (ADR 0008, 0010). Constructed with the
    // borrowed artifact, the borrowed search-context pool, and the host's
    // callback vtable; run(goal) owns the calling thread until arrival,
    // failure, or cancellation.
    //
    // Phase 4b/4c/4d scope: the executor reads the player's position, short-
    // circuits arrival when already inside the acceptance set, borrows a
    // SearchContext from the pool, plans with a freshly-snapshotted capability
    // view, and iterates the resulting Walk and Transition steps. After each
    // step it re-evaluates the teleport-allowed predicate at the live position
    // and re-plans when the player has just crossed into a teleport-allowed
    // zone — the load-bearing "walk out of wilderness, then teleport" path
    // (ADR 0009). Walk failures (stuck / stall) consume one re-plan from a
    // bounded budget and retry from the live position; the budget runs out
    // before infinite-loop pathologies do.
    //
    // Walk step: walkTo(target), then poll readPosition with sleepTicks between
    // samples; arrival = within the caller-supplied radius of the step target
    // (kHandoffChebyshev when another Walk follows, so the next click fires
    // mid-stride; kArrivalChebyshev when the next action needs an exact tile). A
    // stalled-distance counter (no progress for N polls) and a wall-clock
    // deadline together detect "stuck" and surface as Failed; shouldCancel
    // polled before every sleep aborts with Cancelled. The final live
    // position is written out so run() can drive re-plan / teleport-allowed
    // checks without re-reading.
    //
    // Transition step: look up the TransitionRecord by step.transitionIndex.
    // For local-origin (the common case) fire interact(objectId, originTile,
    // optionIndex); for global-origin emit TeleportInitiated and skip the
    // interact. Then iterate the embedded chain — for each Click, poll
    // isInterfaceOpen(targetInterface) with sleepTicks between polls until it
    // opens (or the budget elapses → Failed) then runChainStep(interface,
    // component, option); for each Wait, sleepTicks(ticks). After a short
    // post-chain settle the live position is read out (the engine commits the
    // destination during the settle). Transition Failed is terminal — re-plan
    // does not retry it.
    //
    // The pool borrow is held across the loop so re-plans reuse the same
    // SearchContext without re-entering the blocking acquire path. The
    // ww_executor_run C ABI entry (Phase 4e) follows.
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

        // Pull a fresh capability snapshot from the host and ask the planner
        // for a Plan from `start` to `goal` on the borrowed context. Returns
        // false when the planner cannot reach the goal (outPlan left empty by
        // the assembler); an empty-on-true plan means "already at goal at the
        // area level". The snapshot is cleared then rebuilt on every call so
        // mid-walk skill / item / varbit changes take effect at the next
        // re-plan, but its (and outPlan's) backing storage is reused across
        // calls via the Executor's members.
        bool planFrom(const WwTile &start, const WwGoal &goal,
                      runtime::SearchContext &context, runtime::Plan &outPlan);

        // Copy the wire-shape entries from `src` into `dst`. Each id/value
        // pair becomes a setSkillLevel / setItemCount / setVarbit / setVarp
        // call; nullptr arrays with zero counts are no-ops. Static because
        // it touches no Executor state.
        static void copyCapabilities(const WwCapabilitySnapshot &src,
                                     runtime::CapabilitySnapshot &dst);

        // Drive one Walk step to its target. Issues walkTo, then alternates
        // shouldCancel / sleepTicks / readPosition until arrival, cancellation,
        // a stalled-distance trip, or the wall-clock stuck deadline. Emits
        // StepAdvanced once at entry; emits Stuck on either failure path.
        // Writes the final sampled position to outPosition so run() can drive
        // re-plan / teleport-allowed checks without a redundant readPosition.
        //
        // arrivalRadius is the Chebyshev distance at which the step counts as
        // done: run() passes the wider kHandoffChebyshev when another Walk
        // follows (so the next click fires while the avatar is still moving,
        // instead of stopping on each waypoint), the tight kArrivalChebyshev
        // when the next action needs the avatar on an exact tile, and 0 for
        // the final walk of a radius-0 goal (the ARRIVED contract demands the
        // exact tile, not its neighbour).
        WwStatus walkOneStep(const runtime::Step &step, int32_t stepIndex,
                             int32_t arrivalRadius, WwTile &outPosition);

        // Drive one Transition step's interact + embedded chain. Looks up
        // the TransitionRecord, validates its chain range, fires interact for
        // local-origin records, then runs the chain (interface-open gate per
        // Click, sleepTicks per Wait) and finishes with a short settle sleep
        // and a final readPosition into outPosition. Returns Arrived on
        // success, Cancelled if shouldCancel trips, Failed on a bad index or
        // an interface that never opens. Emits StepAdvanced at entry and
        // TeleportInitiated for global-origin records; the terminal Failed
        // event is emitted by run() so the (stepIndex, transitionIndex) pair
        // carries through.
        WwStatus executeTransitionStep(const runtime::Step &step, int32_t stepIndex,
                                       WwTile &outPosition);

        // Poll isInterfaceOpen(interfaceId) with sleepTicks between polls until
        // it opens. Returns Arrived when open, Cancelled if shouldCancel trips,
        // Failed if the poll budget elapses first.
        WwStatus waitForInterface(int32_t interfaceId) const;

        // Forward one chain step to the host's runChainStep, passing the kind
        // discriminant and all nine generic slots so the host can resolve
        // host-side kinds (DialogueSelect paging).
        void dispatchChainStep(const format::ChainStepRecord &cs) const;

        // Resolve a ClickItem step's worn-vs-backpack variant using `tx`'s item
        // requirements (the candidate item ids) + the isItemWorn callback, then
        // forward the chosen variant (iface, comp, option, sub, special) to the
        // host. The host maps `special` to the COMPONENT_SPECIAL action type.
        void dispatchClickItem(const format::TransitionRecord &tx,
                               const format::ChainStepRecord &cs) const;

        // Emit one progress event when the host wired onEvent; no-op otherwise.
        // stepIndex / transitionIndex default to -1 to signal "not applicable".
        void emit(WwEventKind kind,
                  int32_t stepIndex = -1,
                  int32_t transitionIndex = -1) const;

        const format::ArtifactReader *artifact;
        runtime::ContextPool *pool;
        const Callbacks *callbacks;

        // Distinct varbit / item ids referenced by any transition requirement.
        // Built once on the artifact (ArtifactReader::rebuildRequirementIdLists)
        // and borrowed here; planFrom() reads each via the readVarbit /
        // readItemCount callbacks on every (re-)plan and writes the live values
        // into the capability snapshot. Without this a varbit-gated teleport
        // (e.g. a lodestone-unlock varbit) would always read 0 from the
        // capability snapshot and be rejected — the planner then only ever
        // walks. Borrowed, not owned: the spans alias storage on the artifact
        // that outlives the Executor.
        std::span<const int32_t> requirementVarbitIds;
        std::span<const int32_t> requirementItemIds;

        // Reused per (re-)plan so consecutive plans share their backing
        // storage. CapabilitySnapshot has its own clear() that preserves
        // vector capacity; Plan's steps vector grows once and is then reused
        // across stuck-recovery re-plans within one ww_executor_run.
        runtime::CapabilitySnapshot snapshot;
        runtime::Plan plan;

        // Scratch output buffers for the batched readVarbits / readItemCounts
        // callbacks. Sized to the artifact's requirement id lists at construct
        // time; reused across (re-)plans so the host writes into the same
        // storage without per-plan allocation.
        std::vector<int32_t> varbitValues;
        std::vector<int32_t> itemValues;
    };
}

#endif  // WORLDWALKER_EXEC_EXECUTOR_H
