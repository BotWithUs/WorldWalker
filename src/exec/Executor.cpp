#include "exec/Executor.h"

#include "data/Transitions.h"
#include "format/Artifact.h"
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

        // Host action id for an interface-component interaction (ActionTypes.
        // COMPONENT on the Java side). A chain action with this id carries its
        // target interface as param3>>16, which the chain loop gates on.
        constexpr int32_t kComponentActionId = 57;

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
          callbacks(&callbacks),
          requirementVarbitIds(reader.requirementVarbitIds()),
          requirementItemIds(reader.requirementItemIds())
    {
        // Distinct varbit / item id lists are built once on the artifact and
        // borrowed here, so the Executor pays no per-construction scan over
        // the requirement pool. ww_executor_run constructs a fresh Executor on
        // every run, so the savings matter even at one call per game tick.
        //
        // Size the batched-callback output buffers once to the (fixed) lengths
        // of the id lists. resize() fills with zero so a host that bails out
        // and writes nothing (e.g. callback threw on first id) still leaves
        // sentinel-zero values for the planner to read.
        varbitValues.resize(requirementVarbitIds.size());
        itemValues.resize(requirementItemIds.size());
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

    WwStatus Executor::waitForInterface(int32_t interfaceId) const
    {
        int32_t polls = 0;
        while (callbacks->isInterfaceOpen(callbacks->user, interfaceId) == 0)
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
        return WwStatus::Arrived;
    }

    void Executor::dispatchChainStep(const format::ChainStepRecord &cs) const
    {
        callbacks->runChainStep(callbacks->user, static_cast<int32_t>(cs.kind),
                                cs.a, cs.b, cs.c, cs.d, cs.e, cs.f, cs.g, cs.h, cs.i);
    }

    void Executor::dispatchClickItem(const format::TransitionRecord &tx,
                                     const format::ChainStepRecord &cs) const
    {
        // Decide worn-vs-backpack and identify the carried item, both from this
        // transition's required items (the candidate teleport-item variants —
        // e.g. the dungeoneering / max / completionist cape ids). Worn when any
        // is equipped; otherwise carriedItem is the first one in the backpack.
        bool worn = false;
        int32_t carriedItem = 0;
        const auto reqs = artifact->requirements();
        const uint64_t rstart = tx.requirementStart;
        const uint64_t rend = rstart + tx.requirementCount;
        if (rend <= reqs.size())
        {
            for (uint64_t r = rstart; r < rend; ++r)
            {
                if (static_cast<data::RequirementKind>(reqs[r].kind) != data::RequirementKind::Item)
                {
                    continue;
                }
                const int32_t id = reqs[r].id;
                if (callbacks->isItemWorn(callbacks->user, id) != 0)
                {
                    worn = true;
                    break;  // worn variant chosen — no backpack slot needed
                }
                if (carriedItem == 0 && callbacks->readItemCount(callbacks->user, id) > 0)
                {
                    carriedItem = id;
                }
            }
        }
        // a..d = worn variant, e..h = backpack variant, i = backpack_special.
        // The worn variant is a plain component click (never "special").
        const int32_t iface   = worn ? cs.a : cs.e;
        const int32_t comp    = worn ? cs.b : cs.f;
        const int32_t option  = worn ? cs.c : cs.g;
        const int32_t sub     = worn ? cs.d : cs.h;
        const int32_t special = worn ? 0 : cs.i;
        // For the backpack variant the baked sub-component (slot) is unreliable —
        // the item can sit in any slot — so pass the carried item id and let the
        // host resolve the live slot (the baked `sub` remains a fallback). The
        // worn variant addresses a fixed equipment slot, so it needs no lookup;
        // pass 0 to skip resolution there.
        const int32_t slotItem = worn ? 0 : carriedItem;
        callbacks->runChainStep(callbacks->user,
                                static_cast<int32_t>(data::ChainStepKind::ClickItem),
                                iface, comp, option, sub, special, slotItem, 0, 0, 0);
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
            const auto kind = static_cast<data::ChainStepKind>(cs.kind);
            switch (kind)
            {
                case data::ChainStepKind::Wait:
                    // a=ticks to sleep.
                    callbacks->sleepTicks(callbacks->user, cs.a);
                    break;

                case data::ChainStepKind::WaitInterface:
                {
                    // Block until interface `a` is open (e.g. a teleport dialog
                    // the prior click opened). Times out to Failed so a chain
                    // that never opens its dialog re-plans rather than hangs.
                    const WwStatus st = waitForInterface(cs.a);
                    if (st != WwStatus::Arrived)
                    {
                        return st;
                    }
                    break;
                }

                case data::ChainStepKind::Click:
                {
                    // Generic queued action: a=actionId, b/c/d=param1..3. For a
                    // COMPONENT click the target interface is packed as
                    // param3>>16 ((iface<<16)|comp); wait for it to appear before
                    // clicking. Non-component actions dispatch immediately.
                    if (cs.a == kComponentActionId)
                    {
                        const WwStatus st = waitForInterface(cs.d >> 16);
                        if (st != WwStatus::Arrived)
                        {
                            return st;
                        }
                    }
                    dispatchChainStep(cs);
                    break;
                }

                case data::ChainStepKind::ClickItem:
                {
                    // Pick the worn or carried variant of the item click. The
                    // worn-vs-backpack decision needs the transition's item
                    // requirements (the candidate item ids) — which the host
                    // does not have — so resolve it here via the isItemWorn
                    // callback and forward only the chosen variant. The host
                    // maps the special flag to the right action type.
                    dispatchClickItem(tx, cs);
                    break;
                }

                default:
                    // DialogueSelect: the host resolves the option component
                    // against the live (possibly paged) dialogue. Any interface
                    // gating is expressed as explicit WaitInterface steps, so
                    // just forward the descriptors.
                    dispatchChainStep(cs);
                    break;
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
                            runtime::SearchContext &context, runtime::Plan &outPlan)
    {
        // Snapshot host state into the reused runtime::CapabilitySnapshot
        // member. Re-plans therefore reflect mid-walk state changes (an item
        // picked up, a teleport tab newly available) at the cost of one
        // readCapability call per (re-)plan. The snapshot's backing storage
        // survives across calls (clear() drops contents but keeps capacity),
        // so a stuck-recovery re-plan does not re-grow the four sorted tables.
        snapshot.clear();
        WwCapabilitySnapshot raw{};
        callbacks->readCapability(callbacks->user, &raw);
        copyCapabilities(raw, snapshot);

        // Refresh the live value of every varbit a requirement references (e.g.
        // lodestone-unlock varbits). readCapability does not surface these — the
        // host can't know which ids matter — so we pull them through readVarbits
        // here in one batched call. A requirement-gated teleport is then admitted
        // by the planner only when its unlock varbit actually reads as set; an
        // empty snapshot would reject all of them and force a pure walk.
        //
        // The batched call collapses what was N sequential pipe round-trips
        // (~25-30 for the lodestone-unlock varbits) into one host-side call,
        // which the Java bridge in turn services with at most two batched RPCs
        // (one get_varps, one get_varcs_int) instead of N synchronous get_varp
        // round-trips. This was the dominant cost in pre-walk latency.
        if (!requirementVarbitIds.empty())
        {
            callbacks->readVarbits(callbacks->user,
                                   requirementVarbitIds.data(),
                                   requirementVarbitIds.size(),
                                   varbitValues.data());
            for (std::size_t i = 0; i < requirementVarbitIds.size(); ++i)
            {
                snapshot.setVarbit(requirementVarbitIds[i], varbitValues[i]);
            }
        }

        // Likewise refresh the live count of every item a requirement references
        // (e.g. the dungeoneering cape). Without this an item-gated teleport is
        // rejected against count 0 and the planner falls back to a walk/lodestone.
        // Batched for the same reason: the host can build one inventory map and
        // look every id up instead of repeating two inventory traversals per id.
        if (!requirementItemIds.empty())
        {
            callbacks->readItemCounts(callbacks->user,
                                      requirementItemIds.data(),
                                      requirementItemIds.size(),
                                      itemValues.data());
            for (std::size_t i = 0; i < requirementItemIds.size(); ++i)
            {
                snapshot.setItemCount(requirementItemIds[i], itemValues[i]);
            }
        }

        return context.assembler.assemble(
            start.x, start.y, start.plane,
            goal.x, goal.y, goal.plane,
            &snapshot, outPlan);
    }

    bool Executor::isTeleportAllowedCached(int32_t x, int32_t y, int32_t plane)
    {
        // Tile-level wilderness / no-tele lookups don't change at fractional
        // movement, so a one-slot sticky cache keyed on (squareX, squareY,
        // plane) skips the linear scan on every step inside a single
        // walk-segment chunk. Cache miss falls through to the artifact-side
        // scan.
        constexpr int32_t kSquareShift = 6;
        const int32_t sqX = x >> kSquareShift;
        const int32_t sqY = y >> kSquareShift;
        if (sqX == lastTeleSquareX && sqY == lastTeleSquareY && plane == lastTelePlane)
        {
            return lastTeleResult;
        }
        const bool result = runtime::isTeleportAllowed(*artifact, x, y, plane);
        lastTeleSquareX = sqX;
        lastTeleSquareY = sqY;
        lastTelePlane = plane;
        lastTeleResult = result;
        return result;
    }

    WwStatus Executor::run(WwGoal goal)
    {
        // Sticky teleport-allowed cache starts cold for each run.
        lastTeleSquareX = INT32_MIN;
        lastTeleSquareY = INT32_MIN;
        lastTelePlane = INT32_MIN;
        lastTeleResult = false;

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

        // The plan member's vector grows once and is reused across re-plans
        // — outPlan.steps.clear() inside the assembler keeps the capacity.
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
        bool teleAllowedAtLastPlan = isTeleportAllowedCached(
            position.x, position.y, position.plane);

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
                teleAllowedAtLastPlan = isTeleportAllowedCached(
                    position.x, position.y, position.plane);
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
            const bool teleAllowedNow = isTeleportAllowedCached(
                position.x, position.y, position.plane);
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
