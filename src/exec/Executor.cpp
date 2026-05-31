#include "exec/Executor.h"

#include "runtime/PathAssembler.h"
#include "runtime/SearchContext.h"

#include <algorithm>
#include <cstdlib>

namespace ww::exec
{
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

    WwStatus Executor::run(WwGoal goal)
    {
        WwTile position{ 0, 0, 0 };
        callbacks->readPosition(callbacks->user, &position);
        if (isInsideGoal(position, goal))
        {
            emit(WwEventKind::Arrived);
            return WwStatus::Arrived;
        }

        // Borrow a search context for the duration of the plan. Phase 4b will
        // hold this for the walk loop as well; for now we release it as soon
        // as the planner returns, so the harness sees the pool refilled.
        runtime::SearchContext &context = pool->acquire();
        runtime::Plan plan;
        const bool assembled = context.assembler.assemble(
            position.x, position.y, position.plane,
            goal.x, goal.y, goal.plane,
            /*capabilities=*/nullptr, plan);
        pool->release(context);

        if (!assembled)
        {
            emit(WwEventKind::Failed);
            return WwStatus::Failed;
        }
        if (plan.steps.empty())
        {
            // Planner agrees we're at the goal even though position fell
            // outside the radius (e.g., the goal tile is unwalkable but the
            // start tile lies on its acceptance set at the area level).
            emit(WwEventKind::Arrived);
            return WwStatus::Arrived;
        }

        // Phase 4a: walk loop is pending 4b. Surface the failure rather than
        // silently dropping the planned steps; the host should see this as
        // "planner worked, executor not yet wired."
        emit(WwEventKind::Failed);
        return WwStatus::Failed;
    }
}
