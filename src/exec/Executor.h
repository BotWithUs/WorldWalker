#ifndef WORLDWALKER_EXEC_EXECUTOR_H
#define WORLDWALKER_EXEC_EXECUTOR_H

#include "exec/Callbacks.h"
#include "format/ArtifactReader.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/ContextPool.h"
#include "runtime/PathAssembler.h"

#include <cstddef>
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
    // The executor reads the player's position, short-circuits arrival when
    // already inside the acceptance set, borrows a SearchContext from the pool,
    // plans with a freshly-snapshotted capability view, and iterates the
    // resulting Walk and Transition steps. After each step it re-evaluates the
    // teleport-allowed predicate at the live position and re-plans when the
    // player has just crossed into a teleport-allowed zone — the load-bearing
    // "walk out of wilderness, then teleport" path (ADR 0009). Walk failures
    // (stuck / stall) consume one re-plan from a bounded budget and retry from
    // the live position; the budget runs out before infinite-loop pathologies
    // do.
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
    // SearchContext without re-entering the blocking acquire path.
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
        // What one run knows between steps: the last sampled position, how
        // much of the re-plan budget is spent, and whether the tile the
        // current plan was anchored at allowed global teleports (so a
        // false→true flip after a step can be detected).
        struct RunState
        {
            WwTile  position{};
            int32_t replansUsed{0};
            bool    isTeleAllowedAtLastPlan{false};
        };

        // Outcome of one in-loop re-plan: the plan was rebuilt and the step
        // cursor must restart at 0, the planner declared the goal already
        // reached, or the planner found no route.
        enum class ReplanOutcome
        {
            Restarted,
            Arrived,
            Failed,
        };

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

        // Read the live value of every varbit and item id any transition
        // requirement references, through the batched host callbacks, into
        // `snapshot`. readCapability cannot surface these — the host does not
        // know which ids matter — and without them every requirement-gated
        // teleport reads as locked and the planner only ever walks.
        void refreshRequirementValues();

        // Consume one re-plan from the budget: emit ReplanStarted, re-invoke
        // the planner from io.position, and refresh the teleport-allowed
        // anchor. The caller restarts its step cursor on Restarted; Arrived
        // has already emitted the Arrived event.
        ReplanOutcome replan(const WwGoal &goal, runtime::SearchContext &context,
                             int32_t stepIndex, RunState &io);

        // Emit the terminal Failed event carrying the step (and transition, or
        // -1) it failed on, and return Failed.
        WwStatus failRun(int32_t stepIndex, int32_t transitionIndex) const;

        // Map a re-plan outcome onto the step loop: true when the plan was
        // rebuilt and the cursor restarts at 0; false when the run is over,
        // with its terminal status (Arrived, or Failed with the event emitted)
        // in outStatus.
        bool isRestart(ReplanOutcome outcome, int32_t stepIndex, WwStatus &outStatus) const;

        // Drive plan.steps[i]: a Walk with the arrival radius its successor
        // demands, or a Transition. Writes the final live position.
        WwStatus executeStep(std::size_t i, const WwGoal &goal, WwTile &outPosition);

        // Chebyshev distance at which the Walk step at index i counts as done:
        // kHandoffChebyshev when another Walk follows (the next click fires
        // while the avatar is still moving instead of stopping on each
        // waypoint), 0 for the final walk of a radius-0 goal (the ARRIVED
        // contract demands the exact tile, not its neighbour), else
        // kArrivalChebyshev.
        int32_t arrivalRadiusFor(std::size_t i, const WwGoal &goal) const;

        // The plan drained without an in-loop arrival: the assembler's final
        // step targets the acceptance set, but the walk hands back at its
        // arrival radius, which can be a tile short of the goal test. Judge
        // the live position (after one settling tick) rather than assuming
        // the drain implies arrival.
        WwStatus judgeDrainedRun(const WwGoal &goal, WwTile &ioPosition);

        // Drive one Walk step to its target. Issues walkTo, then alternates
        // shouldCancel / sleepTicks / readPosition until arrival, cancellation,
        // a stalled-distance trip, or the wall-clock stuck deadline. Emits
        // StepAdvanced once at entry; emits Stuck on either failure path.
        // Writes the final sampled position to outPosition so run() can drive
        // re-plan / teleport-allowed checks without a redundant readPosition.
        WwStatus walkOneStep(const runtime::Step &step, int32_t stepIndex,
                             int32_t arrivalRadius, WwTile &outPosition);

        // Drive one Transition step's interact + embedded chain. Looks up
        // the TransitionRecord, validates its chain range, fires interact for
        // local-origin records, then runs the chain and finishes with a short
        // settle sleep and a final readPosition into outPosition. Returns
        // Arrived on success, Cancelled if shouldCancel trips, Failed on a bad
        // index or an interface that never opens. Emits StepAdvanced at entry
        // and TeleportInitiated for global-origin records; the terminal Failed
        // event is emitted by run() so the (stepIndex, transitionIndex) pair
        // carries through.
        WwStatus executeTransitionStep(const runtime::Step &step, int32_t stepIndex,
                                       WwTile &outPosition);

        // Run every chain step of `tx` in order with a cancel poll between
        // steps. Arrived when the whole chain ran; the first non-Arrived
        // step result otherwise.
        WwStatus runChain(const format::TransitionRecord &tx) const;

        // Perform one chain step: the executor handles Wait / WaitInterface
        // itself and gates a COMPONENT Click on its interface being open; the
        // host-resolved kinds are forwarded. Arrived means "continue".
        WwStatus runChainStep(const format::TransitionRecord &tx,
                              const format::ChainStepRecord &cs) const;

        // Poll isInterfaceOpen(interfaceId) with sleepTicks between polls until
        // it opens. Returns Arrived when open, Cancelled if shouldCancel trips,
        // Failed if the poll budget elapses first.
        WwStatus waitForInterface(int32_t interfaceId) const;

        // Sleep `ticks` game ticks one at a time with a cancel poll between.
        // The count comes straight from the scripter-editable teleport JSON,
        // so a typo'd wait must not pin the run past cancellation, and a
        // negative one must never reach the host's sleep.
        WwStatus sleepCancellable(int32_t ticks) const;

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
        // and borrowed here; refreshRequirementValues reads each on every
        // (re-)plan. Borrowed, not owned: the spans alias storage on the
        // artifact that outlives the Executor.
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
