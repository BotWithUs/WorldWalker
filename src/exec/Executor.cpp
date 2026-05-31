#include "exec/Executor.h"

#include "data/Transitions.h"
#include "format/Artifact.h"
#include "runtime/PathAssembler.h"
#include "runtime/SearchContext.h"

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
        const WwEvent event{ kind, 0, stepIndex, transitionIndex };
        callbacks->onEvent(callbacks->user, &event);
    }

    WwStatus Executor::walkOneStep(const runtime::Step &step, int32_t stepIndex)
    {
        const WwTile target{ step.targetX, step.targetY, static_cast<int32_t>(step.plane) };
        callbacks->walkTo(callbacks->user, target);
        emit(WwEventKind::StepAdvanced, stepIndex);

        const auto stepStart = std::chrono::steady_clock::now();
        WwTile lastPos{};
        callbacks->readPosition(callbacks->user, &lastPos);
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
            if (chebyshev(pos, target) <= kArrivalChebyshev)
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

    WwStatus Executor::executeTransitionStep(const runtime::Step &step, int32_t stepIndex)
    {
        const auto txs = artifact->transitions();
        if (step.transitionIndex >= txs.size())
        {
            emit(WwEventKind::Failed, stepIndex, static_cast<int32_t>(step.transitionIndex));
            return WwStatus::Failed;
        }
        const format::TransitionRecord &tx = txs[step.transitionIndex];

        const auto chain = artifact->chainSteps();
        const std::size_t chainStart = tx.chainStart;
        const std::size_t chainEnd   = chainStart + tx.chainCount;
        if (chainEnd > chain.size())
        {
            emit(WwEventKind::Failed, stepIndex, static_cast<int32_t>(step.transitionIndex));
            return WwStatus::Failed;
        }

        const int32_t transitionIndex = static_cast<int32_t>(step.transitionIndex);
        const bool isGlobal = (tx.flags & format::kTransitionFlagGlobalOrigin) != 0;
        emit(WwEventKind::StepAdvanced, stepIndex, transitionIndex);

        if (!isGlobal)
        {
            // Click the world object from the interact-tile (the prior Walk
            // step put the player there). The object tile itself may be
            // blocked; the engine resolves the click from an adjacent tile.
            const WwTile origin{ tx.originX, tx.originY, static_cast<int32_t>(tx.originPlane) };
            callbacks->interact(callbacks->user, tx.objectId, origin, static_cast<int32_t>(tx.optionIndex));
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
                        emit(WwEventKind::Failed, stepIndex, transitionIndex);
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

        // Let the engine commit the destination position. Phase 4d will
        // explicitly resync via readPosition + drift re-plan; for 4c the next
        // Walk step's initial readPosition picks up the new position naturally.
        callbacks->sleepTicks(callbacks->user, kPostChainSettleTicks);
        return WwStatus::Arrived;
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

        // Borrow a SearchContext for the entire run so Phase 4d can re-invoke
        // the planner on the same context without re-acquiring through the
        // pool (ADR 0007: contexts are heap-allocated and never relocated).
        runtime::SearchContext &context = pool->acquire();
        runtime::Plan plan;
        const bool assembled = context.assembler.assemble(
            position.x, position.y, position.plane,
            goal.x, goal.y, goal.plane,
            /*capabilities=*/nullptr, plan);
        if (!assembled)
        {
            pool->release(context);
            emit(WwEventKind::Failed);
            return WwStatus::Failed;
        }
        if (plan.steps.empty())
        {
            // Planner agrees we're at the goal even though position fell
            // outside the radius (e.g., the goal tile is unwalkable but the
            // start tile lies on its acceptance set at the area level).
            pool->release(context);
            emit(WwEventKind::Arrived);
            return WwStatus::Arrived;
        }

        WwStatus result = WwStatus::Arrived;
        int32_t failedStepIndex = -1;
        int32_t failedTransitionIndex = -1;
        for (std::size_t i = 0; i < plan.steps.size(); ++i)
        {
            const runtime::Step &step = plan.steps[i];
            const int32_t stepIndex = static_cast<int32_t>(i);
            if (step.kind == runtime::StepKind::Walk)
            {
                result = walkOneStep(step, stepIndex);
            }
            else
            {
                result = executeTransitionStep(step, stepIndex);
            }
            if (result != WwStatus::Arrived)
            {
                failedStepIndex = stepIndex;
                if (step.kind == runtime::StepKind::Transition)
                {
                    failedTransitionIndex = static_cast<int32_t>(step.transitionIndex);
                }
                break;
            }
        }
        pool->release(context);

        if (result == WwStatus::Arrived)
        {
            emit(WwEventKind::Arrived);
        }
        else if (result == WwStatus::Failed)
        {
            emit(WwEventKind::Failed, failedStepIndex, failedTransitionIndex);
        }
        return result;
    }
}
