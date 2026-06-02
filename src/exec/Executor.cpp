#include "exec/Executor.h"

#include "data/Transitions.h"
#include "format/Artifact.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/PathAssembler.h"
#include "runtime/SearchContext.h"
#include "runtime/TeleportPolicy.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace ww::exec
{
    namespace
    {
        // Tunables, all open per CONTEXT.md. Polling cadence is in game ticks;
        // the stuck deadline is wall-clock (executor links the CRT DLL).
        constexpr int32_t kPollTicks            = 2;     // ~1.2s game time between polls
        constexpr int32_t kArrivalChebyshev     = 1;     // accept being within 1 tile of step target
        // Human walkers don't wait to land on each waypoint before clicking the
        // next — they click ahead while still moving, so motion is continuous.
        // When another Walk step follows, hand off this far out from the current
        // chunk endpoint so the next walkTo fires mid-stride. The tight
        // kArrivalChebyshev is kept for the last walk before an interact /
        // Transition (and the final approach to goal), where landing on the
        // exact interact tile matters. Open tuning per CONTEXT.md.
        constexpr int32_t kHandoffChebyshev     = 3;     // re-click the next chunk this far out
        constexpr int32_t kStalledPollsTrip     = 3;     // N polls with no progress => stuck
        constexpr int32_t kStuckTimeoutMs       = 20000; // 20s wall-clock per Walk step

        // Transition-step tunables (Phase 4c). Interface-open polling lets the
        // executor wait for an interact-opened dialog before clicking inside
        // it; the budget is wall-clock-cheap because each poll is one
        // isInterfaceOpen call plus a short sleep. The settle wait absorbs the
        // engine tick between the chain's final action and the position
        // committing at the destination, so the next walkOneStep reads a
        // stable position.
        constexpr int32_t kInterfaceOpenPollTicks = 2;   // ~1.2s between isInterfaceOpen polls
        constexpr int32_t kInterfaceOpenMaxPolls  = 10;  // ~12s budget per Click step
        constexpr int32_t kPostChainSettleTicks   = 2;   // ~1.2s wait for the engine to commit dest

        // Re-plan budget (Phase 4d). Each walk-stuck recovery and each
        // teleport-allowed flip consumes one re-plan; the cap stops a
        // pathological loop (e.g., a planner that keeps proposing the same
        // unreachable step) from running forever. Three is enough for the
        // realistic worst cases (one stuck recovery + one wilderness-exit
        // teleport re-plan + a margin) without inviting tail-latency surprises.
        constexpr int32_t kMaxReplans = 3;

        // Chebyshev distance on the same plane; INT32_MAX on plane mismatch so
        // a teleport mid-walk reads as "infinitely far" and trips the stall
        // counter immediately rather than masquerading as progress.
        int32_t chebyshev(const WwTile &a, const WwTile &b)
        {
            if (a.plane != b.plane)
            {
                return std::numeric_limits<int32_t>::max();
            }
            const int32_t dx = std::abs(a.x - b.x);
            const int32_t dy = std::abs(a.y - b.y);
            return std::max(dx, dy);
        }
    }

    Executor::Executor(const format::ArtifactReader &reader,
                       runtime::ContextPool &pool,
                       const Callbacks &callbacks)
        : artifact(&reader),
          pool(&pool),
          callbacks(&callbacks)
    {
    }

    bool Executor::isInsideGoal(const WwTile &tile, const WwGoal &goal)
    {
        if (tile.plane != goal.plane)
        {
            return false;
        }
        const int32_t radius = std::max<int32_t>(goal.radius, 0);
        const int32_t dx = std::abs(tile.x - goal.x);
        const int32_t dy = std::abs(tile.y - goal.y);
        return dx <= radius && dy <= radius;
    }

    void Executor::emit(WwEventKind kind, int32_t stepIndex, int32_t transitionIndex) const
    {
        if (callbacks->onEvent == nullptr)
        {
            return;
        }
        const WwEvent event{ static_cast<int32_t>(kind), 0, stepIndex, transitionIndex };
        callbacks->onEvent(callbacks->user, &event);
    }

    WwStatus Executor::walkOneStep(const runtime::Step &step, int32_t stepIndex,
                                   int32_t arrivalRadius, WwTile &outPosition)
    {
        const WwTile target{ step.targetX, step.targetY, static_cast<int32_t>(step.plane) };
        callbacks->walkTo(callbacks->user, target);
        emit(WwEventKind::StepAdvanced, stepIndex);

        const auto stepStart = std::chrono::steady_clock::now();
        WwTile lastPos{};
        callbacks->readPosition(callbacks->user, &lastPos);
        outPosition = lastPos;
        int32_t stalledPolls = 0;

        while (true)
        {
            if (callbacks->shouldCancel(callbacks->user) != 0)
            {
                return WwStatus::Cancelled;
            }
            callbacks->sleepTicks(callbacks->user, kPollTicks);

            WwTile pos{};
            callbacks->readPosition(callbacks->user, &pos);
            outPosition = pos;
            if (chebyshev(pos, target) <= arrivalRadius)
            {
                return WwStatus::Arrived;
            }

            const int32_t prevDist = chebyshev(lastPos, target);
            const int32_t curDist  = chebyshev(pos, target);
            if (curDist >= prevDist)
            {
                ++stalledPolls;
            }
            else
            {
                stalledPolls = 0;
            }
            const auto elapsed = std::chrono::steady_clock::now() - stepStart;
            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            if (stalledPolls >= kStalledPollsTrip || elapsedMs >= kStuckTimeoutMs)
            {
                emit(WwEventKind::Stuck, stepIndex);
                return WwStatus::Failed;
            }
            lastPos = pos;
        }
    }

    WwStatus Executor::executeTransitionStep(const runtime::Step &step, int32_t stepIndex,
                                             WwTile &outPosition)
    {
        // The terminal Failed event is emitted by run() with both stepIndex
        // and transitionIndex; failure paths here just return WwStatus::Failed
        // so the dispatch site can carry the indices through.
        const auto txs = artifact->transitions();
        if (step.transitionIndex >= txs.size())
        {
            return WwStatus::Failed;
        }
        const format::TransitionRecord &tx = txs[step.transitionIndex];

        const auto chain = artifact->chainSteps();
        const std::size_t chainStart = tx.chainStart;
        const std::size_t chainEnd   = chainStart + tx.chainCount;
        if (chainEnd > chain.size())
        {
            return WwStatus::Failed;
        }

        const int32_t transitionIndex = static_cast<int32_t>(step.transitionIndex);
        const bool isGlobal = (tx.flags & format::kTransitionFlagGlobalOrigin) != 0;
        emit(WwEventKind::StepAdvanced, stepIndex, transitionIndex);

        bool issuedAction = true;
        if (!isGlobal)
        {
            // Click the world object from the interact-tile (the prior Walk
            // step put the player there). The object tile itself may be
            // blocked; the engine resolves the click from an adjacent tile.
            // interact returns zero when it was a no-op — the baked loc is
            // gone from the live scene, which for a door means it is already
            // open (open doors are a different loc id). Nothing was issued, so
            // there is no action to settle for; we skip the post-chain wait
            // below and the next Walk step flows straight through the doorway.
            const WwTile origin{ tx.originX, tx.originY, static_cast<int32_t>(tx.originPlane) };
            issuedAction =
                callbacks->interact(callbacks->user, tx.objectId, origin,
                                    static_cast<int32_t>(tx.optionIndex)) != 0;
        }
        else
        {
            emit(WwEventKind::TeleportInitiated, stepIndex, transitionIndex);
        }

        for (std::size_t i = 0; i < tx.chainCount; ++i)
        {
            if (callbacks->shouldCancel(callbacks->user) != 0)
            {
                return WwStatus::Cancelled;
            }
            const format::ChainStepRecord &cs = chain[chainStart + i];
            const int32_t stepIndexInChain = static_cast<int32_t>(i);
            if (cs.kind == static_cast<uint8_t>(data::ChainStepKind::Click))
            {
                // Wait for the target interface to appear, then fire the click.
                // The host knows how to dispatch the click from (transitionIndex,
                // stepIndexInChain) since it can index into the same artifact.
                int32_t polls = 0;
                while (callbacks->isInterfaceOpen(callbacks->user, cs.a) == 0)
                {
                    if (callbacks->shouldCancel(callbacks->user) != 0)
                    {
                        return WwStatus::Cancelled;
                    }
                    if (polls >= kInterfaceOpenMaxPolls)
                    {
                        return WwStatus::Failed;
                    }
                    callbacks->sleepTicks(callbacks->user, kInterfaceOpenPollTicks);
                    ++polls;
                }
                callbacks->runChainStep(callbacks->user, transitionIndex, stepIndexInChain);
            }
            else
            {
                // Wait: a=ticks to sleep.
                callbacks->sleepTicks(callbacks->user, cs.a);
            }
        }

        // Let the engine commit the destination position before sampling it.
        // run() uses this position to decide whether the goal is satisfied
        // and whether to re-plan on a teleport-allowed flip. Skip the wait when
        // nothing was actually done: a no-op interact on an already-open door
        // (issuedAction == false, no chain) leaves the avatar exactly where the
        // prior Walk left it, so there is no late-committing destination to
        // absorb — pausing here is the dead "walk up, stop, wait" the door
        // never needed. Teleports/stairs (global or chain-bearing) and any
        // issued click still settle as before.
        const bool didAct = isGlobal || issuedAction || tx.chainCount > 0;
        if (didAct)
        {
            callbacks->sleepTicks(callbacks->user, kPostChainSettleTicks);
        }
        callbacks->readPosition(callbacks->user, &outPosition);
        return WwStatus::Arrived;
    }

    void Executor::copyCapabilities(const WwCapabilitySnapshot &src,
                                    runtime::CapabilitySnapshot &dst)
    {
        for (std::size_t i = 0; i < src.skillCount; ++i)
        {
            dst.setSkillLevel(src.skills[i].id, src.skills[i].value);
        }
        for (std::size_t i = 0; i < src.itemCount; ++i)
        {
            dst.setItemCount(src.items[i].id, src.items[i].value);
        }
        for (std::size_t i = 0; i < src.varbitCount; ++i)
        {
            dst.setVarbit(src.varbits[i].id, src.varbits[i].value);
        }
        for (std::size_t i = 0; i < src.varpCount; ++i)
        {
            dst.setVarp(src.varps[i].id, src.varps[i].value);
        }
    }

    bool Executor::planFrom(const WwTile &start, const WwGoal &goal,
                            runtime::SearchContext &context, runtime::Plan &outPlan) const
    {
        // Snapshot host state into a runtime::CapabilitySnapshot. Re-plans
        // therefore reflect mid-walk state changes (an item picked up, a
        // teleport tab newly available) at the cost of one readCapability
        // call per (re-)plan. The snapshot is stack-allocated and consumed
        // entirely by assemble() — the assembler stores nothing from it.
        runtime::CapabilitySnapshot snapshot;
        WwCapabilitySnapshot raw{};
        callbacks->readCapability(callbacks->user, &raw);
        copyCapabilities(raw, snapshot);

        return context.assembler.assemble(
            start.x, start.y, start.plane,
            goal.x, goal.y, goal.plane,
            &snapshot, outPlan);
    }

    WwStatus Executor::run(WwGoal goal)
    {
        WwTile position{ 0, 0, 0 };
        callbacks->readPosition(callbacks->user, &position);
        if (isInsideGoal(position, goal))
        {
            emit(WwEventKind::Arrived);
            return WwStatus::Arrived;
        }

        // Borrow a SearchContext for the entire run so re-plans (4d) reuse
        // the same context without re-acquiring through the pool (ADR 0007:
        // contexts are heap-allocated and never relocated). The lease's
        // destructor returns it to the pool on every exit path — including
        // the implicit throw paths inside planFrom() / walkOneStep() — so
        // there is no "forgot to release on this branch" failure mode here.
        runtime::ContextLease lease = pool->acquire();
        runtime::SearchContext &context = *lease;

        runtime::Plan plan;
        if (!planFrom(position, goal, context, plan))
        {
            emit(WwEventKind::Failed);
            return WwStatus::Failed;
        }
        if (plan.steps.empty())
        {
            // Planner agrees we're at the goal even though the live position
            // fell outside the explicit radius (e.g., the goal tile is
            // unwalkable but the start tile lies on its acceptance set at
            // the area level).
            emit(WwEventKind::Arrived);
            return WwStatus::Arrived;
        }

        // Snapshot the teleport-allowed predicate at the planner's anchor
        // position so the post-step check can detect a false→true flip and
        // re-plan with global teleports newly considerable (ADR 0009).
        bool teleAllowedAtLastPlan = runtime::isTeleportAllowed(
            *artifact, position.x, position.y, position.plane);

        int32_t replansUsed         = 0;
        int32_t failedStepIndex     = -1;
        int32_t failedTransitionIndex = -1;
        WwStatus terminal           = WwStatus::Arrived;
        bool arrivedEmitted         = false;

        std::size_t i = 0;
        while (i < plan.steps.size())
        {
            const runtime::Step &step = plan.steps[i];
            const int32_t stepIndex   = static_cast<int32_t>(i);

            WwStatus stepResult;
            if (step.kind == runtime::StepKind::Walk)
            {
                // Hand off to the next chunk while still moving when another
                // Walk follows; arrive tight when the next step is an interact
                // (Transition) or this is the final approach to the goal, where
                // the exact tile matters.
                const bool nextIsWalk = (i + 1 < plan.steps.size())
                    && plan.steps[i + 1].kind == runtime::StepKind::Walk;
                const int32_t arrivalRadius =
                    nextIsWalk ? kHandoffChebyshev : kArrivalChebyshev;
                stepResult = walkOneStep(step, stepIndex, arrivalRadius, position);
            }
            else
            {
                stepResult = executeTransitionStep(step, stepIndex, position);
            }

            if (stepResult == WwStatus::Cancelled)
            {
                terminal = WwStatus::Cancelled;
                break;
            }

            if (stepResult == WwStatus::Failed)
            {
                // Transition failures and exhausted re-plan budgets are
                // terminal. Walk failures (the Stuck event was emitted
                // inside walkOneStep) consume one re-plan.
                const bool isTransition = step.kind == runtime::StepKind::Transition;
                if (isTransition || replansUsed >= kMaxReplans)
                {
                    failedStepIndex = stepIndex;
                    if (isTransition)
                    {
                        failedTransitionIndex = static_cast<int32_t>(step.transitionIndex);
                    }
                    terminal = WwStatus::Failed;
                    break;
                }

                // Walk-stuck recovery: re-read the position (walkOneStep
                // wrote the last sample to it already, but a host that
                // teleports us between samples might have moved further),
                // and re-plan.
                callbacks->readPosition(callbacks->user, &position);
                ++replansUsed;
                emit(WwEventKind::ReplanStarted, stepIndex);
                if (!planFrom(position, goal, context, plan))
                {
                    failedStepIndex = stepIndex;
                    terminal = WwStatus::Failed;
                    break;
                }
                if (plan.steps.empty())
                {
                    terminal = WwStatus::Arrived;
                    emit(WwEventKind::Arrived);
                    arrivedEmitted = true;
                    break;
                }
                teleAllowedAtLastPlan = runtime::isTeleportAllowed(
                    *artifact, position.x, position.y, position.plane);
                i = 0;
                continue;
            }

            // Step Arrived. walkOneStep / executeTransitionStep wrote the
            // live position into `position`; use it for the goal check and
            // the teleport-allowed flip without a redundant readPosition.
            if (isInsideGoal(position, goal))
            {
                terminal = WwStatus::Arrived;
                emit(WwEventKind::Arrived);
                arrivedEmitted = true;
                break;
            }
            const bool teleAllowedNow = runtime::isTeleportAllowed(
                *artifact, position.x, position.y, position.plane);
            if (teleAllowedNow && !teleAllowedAtLastPlan && replansUsed < kMaxReplans)
            {
                ++replansUsed;
                emit(WwEventKind::ReplanStarted, stepIndex);
                if (!planFrom(position, goal, context, plan))
                {
                    failedStepIndex = stepIndex;
                    terminal = WwStatus::Failed;
                    break;
                }
                if (plan.steps.empty())
                {
                    terminal = WwStatus::Arrived;
                    emit(WwEventKind::Arrived);
                    arrivedEmitted = true;
                    break;
                }
                teleAllowedAtLastPlan = true;
                i = 0;
                continue;
            }
            teleAllowedAtLastPlan = teleAllowedNow;
            ++i;
        }

        // Drained every step without an isInsideGoal short-circuit: the
        // assembler's final step lands at (or within radius of) the goal,
        // so a clean drain is success.
        if (i >= plan.steps.size() && terminal == WwStatus::Arrived && !arrivedEmitted)
        {
            emit(WwEventKind::Arrived);
            arrivedEmitted = true;
        }

        if (terminal == WwStatus::Failed)
        {
            emit(WwEventKind::Failed, failedStepIndex, failedTransitionIndex);
        }
        return terminal;
        // lease destructor returns the context to the pool here.
    }
}
