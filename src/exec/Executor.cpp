#include "exec/Executor.h"

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
                if (result != WwStatus::Arrived)
                {
                    failedStepIndex = stepIndex;
                    break;
                }
            }
            else
            {
                // Phase 4c will execute interact + chain. For 4b a Transition
                // step trips the terminal Failed below, carrying its index so
                // the host can tell which step was unhandled.
                result = WwStatus::Failed;
                failedStepIndex = stepIndex;
                failedTransitionIndex = static_cast<int32_t>(step.transitionIndex);
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
