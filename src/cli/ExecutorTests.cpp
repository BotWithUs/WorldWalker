#include "cli/ExecutorTests.h"

#include "c_api/worldwalker_c.h"
#include "cli/HarnessPicks.h"
#include "data/Transitions.h"
#include "exec/Callbacks.h"
#include "exec/Executor.h"
#include "format/Artifact.h"
#include "runtime/AreaSearch.h"
#include "runtime/ContextPool.h"
#include "runtime/PathAssembler.h"
#include "runtime/TeleportPolicy.h"
#include "runtime/TileSearch.h"
#include "runtime/TransitionShape.h"
#include "runtime/WorldView.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace ww::cli
{
    namespace
    {
        // Counters + simulated position for the Executor harness. Routed via the
        // Callbacks.user cookie so each function pointer stays a plain extern "C"
        // entry. Four modes share the same struct:
        //   AbortOnAction        — short-circuit / start==goal tests; any action
        //                          callback is a failure (the executor must not
        //                          try to walk when it is already there).
        //   SimulateInstantWalk  — walk-loop test; walkTo updates `position` so
        //                          the next readPosition reports the player as
        //                          arrived at the clicked tile. sleepTicks and
        //                          shouldCancel are silent no-ops. interact /
        //                          runChainStep are still forbidden (a walk-only
        //                          plan must not touch them).
        //   SimulateTransition   — walk + transition test; walkTo / sleepTicks /
        //                          shouldCancel as above; interact / runChainStep
        //                          count silently and land the simulated player
        //                          on transitionDest (the engine commits the
        //                          destination after the click); isInterfaceOpen
        //                          reports the dialog open immediately so the
        //                          chain doesn't stall on the open-poll budget.
        //   SimulateReplanRecovery — the re-plan path. walkTo only updates the
        //                          simulated position when walkToUpdatesPosition
        //                          is true — initially false so the first walk
        //                          stalls and trips the stuck event. On the
        //                          ReplanStarted event the harness flips the flag
        //                          so the re-planned walk arrives normally.
        enum class ExecHarnessMode : std::uint8_t
        {
            AbortOnAction          = 0,
            SimulateInstantWalk    = 1,
            SimulateTransition     = 2,
            SimulateReplanRecovery = 3,
        };

        struct ExecHarness
        {
            exec::WwTile      position;
            ExecHarnessMode   mode;
            int               readPositionCalls;
            int               onEventCalls;
            int               walkToCalls;
            int               sleepTicksCalls;
            int               shouldCancelCalls;
            int               interactCalls;
            int               runChainStepCalls;
            int               isInterfaceOpenCalls;
            // Two classes of surprise with two different policies, so two
            // counters. An action the mode forbids is a real failure: the
            // executor did something the plan did not call for. A read the
            // harness does not model is reported but does not fail — the
            // executor is entitled to ask, and the zeroed answer it gets back is
            // a legitimate one. Sharing one counter let a read spend the
            // action budget and made "abort=0" mean less than it looked.
            int               unexpectedActions;
            int               unmodelledReads;
            int               stuckEvents;
            int               replanStartedEvents;
            bool              walkToUpdatesPosition;
            exec::WwEventKind lastEventKind;
            exec::WwTile      transitionDest;   // SimulateTransition: where the click lands
            // The combat varbit is modelled, not a surprise: every plan reads
            // it. It answers 1 (in combat) for this many reads, then 0.
            int               inCombatReadsLeft;
            int               combatReads;
            // SimulateTransition: interact answers 0 ("no such loc here") and
            // does not move the player, as the host does for a loc it cannot
            // find near the origin.
            bool              isLocMissing;
            // transitionIndex of the last Failed event: the failing transition,
            // or -1 when the run failed somewhere other than a transition.
            int               failedTransitionIndex;
            // Walk clicks the game drops before one lands, as it does for a
            // click made while the player is still in a forced move.
            int               droppedWalkClicks;
            // SimulateTransition: this many interacts land the player on
            // `failedLanding` instead of transitionDest, like a failed agility
            // jump dropping them into a pit.
            int               failedLandingsLeft;
            exec::WwTile      failedLanding;
            // SimulateTransition: a door that walks the player through itself.
            // The landing commits `landingDelayTicks` ticks after the click; a
            // walk clicked before then cancels it, as the game does, and is
            // counted in cancelledLandings. droppedInteracts clicks do nothing.
            int               landingDelayTicks;
            int               pendingLandingTicks;
            int               cancelledLandings;
            int               droppedInteracts;
            // SimulateTransition: the door raises a plain message box (1186)
            // on its first click and lets the player through only once that
            // page is continued. continueClicks counts the continues.
            bool              hasEntryChat;
            bool              isChatOpen;
            int               continueClicks;
        };

        // Every callback that can surprise the harness. The class each one
        // belongs to is decided in recordUnexpected below and nowhere else, so a
        // new callback cannot quietly join the wrong accumulator.
        enum class HarnessCallback : std::uint8_t
        {
            WalkTo,
            Interact,
            RunChainStep,
            SleepTicks,
            ShouldCancel,
            ReadItemCount,
            ReadVarbits,
            ReadItemCounts,
            IsItemWorn,
            IsInterfaceOpen,
        };

        // The single place a harness callback records "the executor did
        // something this mode did not model", and the single place the class
        // rule lives.
        void recordUnexpected(ExecHarness *harness, HarnessCallback which)
        {
            switch (which)
            {
                case HarnessCallback::WalkTo:
                case HarnessCallback::Interact:
                case HarnessCallback::RunChainStep:
                case HarnessCallback::SleepTicks:
                case HarnessCallback::ShouldCancel:
                    ++harness->unexpectedActions;
                    return;
                case HarnessCallback::ReadItemCount:
                case HarnessCallback::ReadVarbits:
                case HarnessCallback::ReadItemCounts:
                case HarnessCallback::IsItemWorn:
                case HarnessCallback::IsInterfaceOpen:
                    ++harness->unmodelledReads;
                    return;
            }
        }

        extern "C" void harnessReadPosition(void *user, exec::WwTile *outTile)
        {
            ExecHarness *h = static_cast<ExecHarness *>(user);
            ++h->readPositionCalls;
            *outTile = h->position;
        }

        extern "C" void harnessReadCapability(void *user, exec::WwCapabilitySnapshot *outSnapshot)
        {
            // planFrom() always pulls a snapshot, so this fires on every plan /
            // re-plan. An empty snapshot exercises the capability-aware overload
            // without admitting any req-bearing transition (the transition test
            // picks ungated transitions for that reason).
            static_cast<void>(user);
            *outSnapshot = exec::WwCapabilitySnapshot{};
        }

        extern "C" void harnessReadInstance(void *user, exec::WwInstanceChunks *outChunks)
        {
            // planFrom() pulls this on every plan / re-plan, so unlike the
            // reserved readVarbit slot it replaced, being called is NOT a
            // surprise. The harness walks the static overworld and answers with
            // a zeroed struct — "not an instance" — exactly as a host does for
            // an ordinary scene.
            static_cast<void>(user);
            *outChunks = exec::WwInstanceChunks{};
        }

        extern "C" std::int32_t harnessReadItemCount(void *user, std::int32_t)
        {
            recordUnexpected(static_cast<ExecHarness *>(user), HarnessCallback::ReadItemCount);
            return 0;
        }

        extern "C" void harnessReadVarbits(void *user, const std::int32_t *ids, size_t count,
                                           std::int32_t *out)
        {
            // The batched variant is invoked at every (re-)plan and after every
            // step, always carrying the combat varbit. That one is modelled;
            // any other id is an unmodelled read answered 0.
            ExecHarness *h = static_cast<ExecHarness *>(user);
            bool hasOtherId = false;
            for (size_t i = 0; i < count; ++i)
            {
                out[i] = 0;
                if (ids[i] != runtime::kInCombatVarbitId)
                {
                    hasOtherId = true;
                    continue;
                }
                ++h->combatReads;
                if (h->inCombatReadsLeft > 0)
                {
                    --h->inCombatReadsLeft;
                    out[i] = 1;
                }
            }
            if (hasOtherId)
            {
                recordUnexpected(h, HarnessCallback::ReadVarbits);
            }
        }

        extern "C" void harnessReadItemCounts(void *user, const std::int32_t *, size_t count,
                                              std::int32_t *out)
        {
            if (count > 0)
            {
                recordUnexpected(static_cast<ExecHarness *>(user), HarnessCallback::ReadItemCounts);
                for (size_t i = 0; i < count; ++i)
                {
                    out[i] = 0;
                }
            }
        }

        // The plain chat pages the executor continues, and the one the entry
        // chat test opens (1186 MESBOX_V2, continue 1186:8).
        constexpr std::int32_t kMessageBox = 1186;
        constexpr std::int32_t kMessageBoxContinue = (kMessageBox << 16) | 8;
        constexpr std::int32_t kDialogueAction = 30;

        bool isChatInterface(std::int32_t interfaceId)
        {
            return interfaceId == 1184 || interfaceId == 1191 || interfaceId == 1187
                || interfaceId == 1186 || interfaceId == 1189;
        }

        extern "C" std::int32_t harnessIsItemWorn(void *user, std::int32_t)
        {
            recordUnexpected(static_cast<ExecHarness *>(user), HarnessCallback::IsItemWorn);
            return 0;
        }

        extern "C" std::int32_t harnessIsInterfaceOpen(void *user, std::int32_t interfaceId)
        {
            ExecHarness *h = static_cast<ExecHarness *>(user);
            ++h->isInterfaceOpenCalls;
            if (isChatInterface(interfaceId))
            {
                return (h->isChatOpen && interfaceId == kMessageBox) ? 1 : 0;
            }
            if (h->mode == ExecHarnessMode::SimulateTransition)
            {
                return 1;
            }
            recordUnexpected(h, HarnessCallback::IsInterfaceOpen);
            return 0;
        }

        extern "C" void harnessWalkTo(void *user, exec::WwTile target)
        {
            ExecHarness *h = static_cast<ExecHarness *>(user);
            ++h->walkToCalls;
            if (h->pendingLandingTicks > 0)
            {
                h->pendingLandingTicks = 0;
                ++h->cancelledLandings;
                return;
            }
            if (h->droppedWalkClicks > 0)
            {
                --h->droppedWalkClicks;
                return;
            }
            if (h->mode == ExecHarnessMode::SimulateInstantWalk
                || h->mode == ExecHarnessMode::SimulateTransition)
            {
                h->position = target;
            }
            else if (h->mode == ExecHarnessMode::SimulateReplanRecovery)
            {
                // Stall until the harness flips walkToUpdatesPosition on the
                // executor's first ReplanStarted event. The first walk never
                // moves the simulated player, so the stalled-distance counter
                // trips at kStalledPollsTrip polls -> Stuck -> run() re-plans.
                if (h->walkToUpdatesPosition)
                {
                    h->position = target;
                }
            }
            else
            {
                recordUnexpected(h, HarnessCallback::WalkTo);
            }
        }

        extern "C" std::int32_t harnessInteract(void *user, std::int32_t, exec::WwTile,
                                                std::int32_t)
        {
            ExecHarness *h = static_cast<ExecHarness *>(user);
            ++h->interactCalls;
            if (h->mode != ExecHarnessMode::SimulateTransition)
            {
                recordUnexpected(h, HarnessCallback::Interact);
            }
            else if (h->isLocMissing)
            {
                return 0;
            }
            else if (h->failedLandingsLeft > 0)
            {
                --h->failedLandingsLeft;
                h->position = h->failedLanding;
            }
            else if (h->droppedInteracts > 0)
            {
                --h->droppedInteracts;
            }
            else if (h->hasEntryChat && h->continueClicks == 0)
            {
                h->isChatOpen = true;
            }
            else if (h->landingDelayTicks > 0)
            {
                h->pendingLandingTicks = h->landingDelayTicks;
            }
            else
            {
                h->position = h->transitionDest;
            }
            // Otherwise the harness "issues" the action, so the executor settles
            // as before — the SimulateTransition step counts depend on that wait.
            return 1;
        }

        extern "C" void harnessRunChainStep(void *user, std::int32_t, std::int32_t a,
                                            std::int32_t, std::int32_t, std::int32_t d,
                                            std::int32_t, std::int32_t, std::int32_t,
                                            std::int32_t, std::int32_t)
        {
            ExecHarness *h = static_cast<ExecHarness *>(user);
            if (a == kDialogueAction && d == kMessageBoxContinue && h->isChatOpen)
            {
                // The page closes and the door starts carrying the player in.
                ++h->continueClicks;
                h->isChatOpen = false;
                h->pendingLandingTicks = h->landingDelayTicks;
                return;
            }
            ++h->runChainStepCalls;
            if (h->mode != ExecHarnessMode::SimulateTransition)
            {
                recordUnexpected(h, HarnessCallback::RunChainStep);
            }
            else
            {
                h->position = h->transitionDest;
            }
        }

        extern "C" void harnessSleepTicks(void *user, std::int32_t ticks)
        {
            ExecHarness *h = static_cast<ExecHarness *>(user);
            ++h->sleepTicksCalls;
            if (h->pendingLandingTicks > 0)
            {
                h->pendingLandingTicks -= ticks;
                if (h->pendingLandingTicks <= 0)
                {
                    h->pendingLandingTicks = 0;
                    h->position = h->transitionDest;
                }
            }
            if (h->mode == ExecHarnessMode::AbortOnAction)
            {
                recordUnexpected(h, HarnessCallback::SleepTicks);
            }
        }

        extern "C" std::int32_t harnessShouldCancel(void *user)
        {
            ExecHarness *h = static_cast<ExecHarness *>(user);
            ++h->shouldCancelCalls;
            if (h->mode == ExecHarnessMode::AbortOnAction)
            {
                recordUnexpected(h, HarnessCallback::ShouldCancel);
            }
            return 0;
        }

        extern "C" void harnessOnEvent(void *user, const exec::WwEvent *event)
        {
            ExecHarness *h = static_cast<ExecHarness *>(user);
            ++h->onEventCalls;
            const auto kind = static_cast<exec::WwEventKind>(event->kind);
            h->lastEventKind = kind;
            if (kind == exec::WwEventKind::Stuck)
            {
                ++h->stuckEvents;
            }
            else if (kind == exec::WwEventKind::Failed)
            {
                h->failedTransitionIndex = event->transitionIndex;
            }
            else if (kind == exec::WwEventKind::ReplanStarted)
            {
                ++h->replanStartedEvents;
                // SimulateReplanRecovery: the executor has just consumed one
                // re-plan from its budget. Flipping the flag lets the next walk
                // arrive normally so the run terminates Arrived.
                if (h->mode == ExecHarnessMode::SimulateReplanRecovery)
                {
                    h->walkToUpdatesPosition = true;
                }
            }
        }

        const exec::Callbacks kCallbackPrototype{
            nullptr,
            harnessReadPosition,
            harnessReadCapability,
            harnessReadInstance,
            harnessReadItemCount,
            harnessReadVarbits,
            harnessReadItemCounts,
            harnessIsItemWorn,
            harnessIsInterfaceOpen,
            harnessWalkTo,
            harnessInteract,
            harnessRunChainStep,
            harnessSleepTicks,
            harnessShouldCancel,
            harnessOnEvent,
        };

        // What every test in this file needs: the artifact, a context pool, a
        // view over it, and the walk-only start/goal pair the walk, re-plan and
        // FFI tests all reuse.
        struct ExecContext
        {
            const format::ArtifactReader &reader;
            runtime::ContextPool         &pool;
            runtime::WorldView           &view;
            const format::AreaNodeRecord &node;
            std::int32_t                  plane;
            runtime::TilePoint            farthest;
        };

        ExecHarness makeHarness(ExecHarnessMode mode, std::int32_t x, std::int32_t y,
                                std::int32_t plane)
        {
            ExecHarness harness{};
            harness.mode          = mode;
            harness.position      = exec::WwTile{ x, y, plane };
            harness.lastEventKind = exec::WwEventKind::Failed;
            harness.failedTransitionIndex = -1;
            return harness;
        }

        // The two counters, printed together on every test's second line. Only
        // unexpectedActions gates; unmodelledReads is context for the reader.
        void printCallPattern(const char *label, const ExecHarness &h)
        {
            std::printf("  exec:   %swalks=%d sleeps=%d cancels=%d reads=%d events=%d"
                        " actions=%d (expect 0) unmodelled-reads=%d\n",
                        label, h.walkToCalls, h.sleepTicksCalls, h.shouldCancelCalls,
                        h.readPositionCalls, h.onEventCalls, h.unexpectedActions,
                        h.unmodelledReads);
        }

        void printLanding(const char *label, const ExecHarness &h, runtime::TilePoint goal,
                          std::int32_t plane)
        {
            std::printf("  exec:   %s landed at (%d,%d,p%d), goal (%d,%d,p%d)\n", label,
                        h.position.x, h.position.y, h.position.plane, goal.x, goal.y, plane);
        }

        // Test 1: start == goal -> short-circuit Arrived. No action callback
        // should fire.
        std::size_t testStartEqualsGoal(ExecContext &ctx)
        {
            ExecHarness harness = makeHarness(ExecHarnessMode::AbortOnAction, ctx.node.centroidX,
                                              ctx.node.centroidY, ctx.plane);
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwGoal goal{ ctx.node.centroidX, ctx.node.centroidY, ctx.plane, 0 };
            const exec::WwStatus status = executor.run(goal);

            std::printf("  exec:   start==goal status=%d (expect 0=Arrived) free=%zu\n",
                        static_cast<int>(status), ctx.pool.freeCount());
            std::printf("  exec:   readPosition=%d onEvent=%d lastEvent=%d (expect 1,1,%d)"
                        " actions=%d (expect 0) unmodelled-reads=%d\n",
                        harness.readPositionCalls, harness.onEventCalls,
                        static_cast<int>(harness.lastEventKind),
                        static_cast<int>(exec::WwEventKind::Arrived),
                        harness.unexpectedActions, harness.unmodelledReads);
            std::size_t failures = status == exec::WwStatus::Arrived ? 0u : 1u;
            failures += (harness.readPositionCalls == 1 && harness.onEventCalls == 1
                         && harness.lastEventKind == exec::WwEventKind::Arrived) ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Test 2: walk-only plan from the centroid to the farthest same-area
        // tile within 24 hops. With SimulateInstantWalk every walkTo flips the
        // simulated position to the target, so each Walk step arrives on its
        // first poll.
        std::size_t testWalkLoop(ExecContext &ctx)
        {
            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                              ctx.node.centroidX, ctx.node.centroidY, ctx.plane);
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwGoal goal{ ctx.farthest.x, ctx.farthest.y, ctx.plane, 0 };
            const exec::WwStatus status = executor.run(goal);

            std::printf("  exec:   walk-loop status=%d (expect 0=Arrived) free=%zu"
                        " lastEvent=%d (expect %d)\n",
                        static_cast<int>(status), ctx.pool.freeCount(),
                        static_cast<int>(harness.lastEventKind),
                        static_cast<int>(exec::WwEventKind::Arrived));
            printCallPattern("", harness);
            printLanding("walk-loop", harness, ctx.farthest, ctx.plane);
            std::size_t failures = (status == exec::WwStatus::Arrived
                                    && harness.lastEventKind == exec::WwEventKind::Arrived)
                                       ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Count the Click / Wait steps of a transition's embedded chain, which
        // is what the transition test expects the executor to dispatch.
        void countChain(const format::ArtifactReader &reader, const format::TransitionRecord &tx,
                        std::int32_t &outClicks, std::int32_t &outWaits)
        {
            outClicks = 0;
            outWaits  = 0;
            const auto chain = reader.chainSteps();
            for (std::uint32_t i = 0; i < tx.chainCount; ++i)
            {
                if (chain[tx.chainStart + i].kind
                    == static_cast<std::uint8_t>(data::ChainStepKind::Click))
                {
                    ++outClicks;
                }
                else
                {
                    ++outWaits;
                }
            }
        }

        // Test 3: cross-area plan that crosses one Transition. Prefer a chained
        // transition so the Click/Wait dispatch in the chain loop is exercised;
        // if the artifact has none, fall back to any traversable edge — that
        // still verifies the interact + loop machinery + post-chain settle.
        std::size_t testTransition(ExecContext &ctx)
        {
            CrossAreaPick pick{};
            if (!pickCrossAreaPair(ctx.reader, ctx.view, acceptChainedUngated, pick)
                && !pickCrossAreaPair(ctx.reader, ctx.view, acceptAnyTransition, pick))
            {
                std::printf("  exec:   transition test skipped (no traversable cross-area edge)\n");
                return 0;
            }
            const auto &edge = ctx.reader.areaEdges()[pick.edgeIndex];
            const auto &tx   = ctx.reader.transitions()[edge.transitionIndex];
            // The any-transition fallback admits req-bearing transitions; with an
            // empty capability snapshot the planner would filter them out and the
            // test would lose its picked edge. Skip in that case (rare on
            // realistic artifacts; chained ungated edges dominate).
            if (tx.requirementCount != 0u)
            {
                std::printf("  exec:   transition test skipped (picked edge tx%u has %u reqs)\n",
                            edge.transitionIndex, tx.requirementCount);
                return 0;
            }
            const bool isGlobal = (tx.flags & format::kTransitionFlagGlobalOrigin) != 0;
            std::int32_t clickCount = 0;
            std::int32_t waitCount  = 0;
            countChain(ctx.reader, tx, clickCount, waitCount);
            std::printf("  exec:   tx%u kind=%u global=%d chain: %d clicks + %d waits\n",
                        edge.transitionIndex, tx.kind, isGlobal ? 1 : 0, clickCount, waitCount);

            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateTransition, pick.start.x,
                                              pick.start.y, pick.startPlane);
            harness.transitionDest =
                exec::WwTile{ tx.destX, tx.destY, static_cast<std::int32_t>(tx.destPlane) };
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwGoal goal{ pick.goal.x, pick.goal.y, pick.goalPlane, 0 };
            const exec::WwStatus status = executor.run(goal);

            std::printf("  exec:   transition status=%d (expect 0=Arrived) free=%zu"
                        " lastEvent=%d (expect %d)\n",
                        static_cast<int>(status), ctx.pool.freeCount(),
                        static_cast<int>(harness.lastEventKind),
                        static_cast<int>(exec::WwEventKind::Arrived));
            std::printf("  exec:   interacts=%d (expect %d) chainSteps=%d (expect %d)"
                        " ifaceOpen=%d (>= %d)\n",
                        harness.interactCalls, isGlobal ? 0 : 1, harness.runChainStepCalls,
                        clickCount, harness.isInterfaceOpenCalls, clickCount);
            printCallPattern("", harness);
            std::size_t failures = (status == exec::WwStatus::Arrived
                                    && harness.lastEventKind == exec::WwEventKind::Arrived)
                                       ? 0u : 1u;
            failures += (harness.interactCalls == (isGlobal ? 0 : 1)
                         && harness.runChainStepCalls == clickCount) ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Test 4: stuck -> re-plan -> recover. The first walk's walkTo does NOT
        // advance the simulated position, so the stalled-distance counter trips
        // and walkOneStep emits Stuck and returns Failed. run() then issues
        // ReplanStarted; on that event the harness flips walkToUpdatesPosition,
        // so the re-planned walk arrives on its first poll. Final terminal:
        // Arrived. This exercises the whole re-plan path — capability snapshot
        // pull, planFrom, walk-failure recovery — without touching transitions.
        std::size_t testReplanRecovery(ExecContext &ctx)
        {
            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateReplanRecovery,
                                              ctx.node.centroidX, ctx.node.centroidY, ctx.plane);
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwGoal goal{ ctx.farthest.x, ctx.farthest.y, ctx.plane, 0 };
            const exec::WwStatus status = executor.run(goal);

            std::printf("  exec:   replan status=%d (expect 0=Arrived) free=%zu"
                        " lastEvent=%d (expect %d)\n",
                        static_cast<int>(status), ctx.pool.freeCount(),
                        static_cast<int>(harness.lastEventKind),
                        static_cast<int>(exec::WwEventKind::Arrived));
            std::printf("  exec:   stucks=%d replans=%d (expect stucks>=1, replans>=1)\n",
                        harness.stuckEvents, harness.replanStartedEvents);
            printCallPattern("", harness);
            std::size_t failures = (status == exec::WwStatus::Arrived
                                    && harness.lastEventKind == exec::WwEventKind::Arrived)
                                       ? 0u : 1u;
            failures += (harness.stuckEvents >= 1 && harness.replanStartedEvents >= 1) ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            printLanding("replan", harness, ctx.farthest, ctx.plane);
            return failures;
        }

        // Ungated local transitions a missing loc may be skipped on (doors),
        // and ones it may not (ladders, cave mouths, long rides).
        bool acceptUngatedDoor(const format::TransitionRecord &tx)
        {
            return tx.requirementCount == 0u && runtime::isSameFloorCrossing(tx);
        }

        bool acceptUngatedNonDoor(const format::TransitionRecord &tx)
        {
            const bool isGlobal = (tx.flags & format::kTransitionFlagGlobalOrigin) != 0;
            return tx.requirementCount == 0u && !isGlobal && !runtime::isSameFloorCrossing(tx);
        }

        // Test 4b: the host cannot find the loc a transition names. A
        // same-floor crossing (a door already open) is skipped after one try and
        // the run carries on past it; anything else is retried and then fails
        // on that transition, rather than walking on into a stall and a re-plan
        // loop. Run once per kind, with `filter` choosing which.
        //
        // Whether the door run then ARRIVES is not this test's business: the
        // harness player never crosses, so a plan that ends on the door drains
        // short of the goal. What is checked is that the transition itself was
        // not what failed it.
        std::size_t testMissingLoc(ExecContext &ctx, TransitionFilter filter, const char *label)
        {
            CrossAreaPick pick{};
            if (!pickCrossAreaPair(ctx.reader, ctx.view, filter, pick))
            {
                std::printf("  exec:   missing-loc %s skipped (no traversable cross-area edge)\n",
                            label);
                return 0;
            }
            const auto &edge = ctx.reader.areaEdges()[pick.edgeIndex];
            const auto &tx   = ctx.reader.transitions()[edge.transitionIndex];
            const bool isSkippable = runtime::isSameFloorCrossing(tx);

            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateTransition, pick.start.x,
                                              pick.start.y, pick.startPlane);
            harness.transitionDest =
                exec::WwTile{ tx.destX, tx.destY, static_cast<std::int32_t>(tx.destPlane) };
            harness.isLocMissing = true;
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwGoal goal{ pick.goal.x, pick.goal.y, pick.goalPlane, 0 };
            const exec::WwStatus status = executor.run(goal);

            // A missing ladder or cave mouth must fail the run ON that
            // transition; a skipped door must not.
            const int txIndex = static_cast<int>(edge.transitionIndex);
            const bool isFailedOnTx = status == exec::WwStatus::Failed
                                   && harness.failedTransitionIndex == txIndex;
            const int wantInteracts = isSkippable ? 1 : 3;
            std::printf("  exec:   missing-loc %s tx%d status=%d failedOnTx=%d (expect %d)"
                        " interacts=%d (expect %d) replans=%d (expect 0)\n",
                        label, txIndex, static_cast<int>(status), isFailedOnTx ? 1 : 0,
                        isSkippable ? 0 : 1, harness.interactCalls, wantInteracts,
                        harness.replanStartedEvents);
            printCallPattern("missing-loc ", harness);
            std::size_t failures = isFailedOnTx == !isSkippable ? 0u : 1u;
            failures += harness.interactCalls == wantInteracts ? 0u : 1u;
            failures += harness.replanStartedEvents == 0 ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // An ungated one-tile door: the click is made from a tile already
        // within a tile of the far side.
        bool acceptUngatedOneTileDoor(const format::TransitionRecord &tx)
        {
            const int hop = std::max(std::abs(tx.originX - tx.destX),
                                     std::abs(tx.originY - tx.destY));
            return acceptUngatedDoor(tx) && hop == 1;
        }

        // Test 4f: a door that walks the player through itself, a few ticks
        // after the click (Draynor Manor's front door). The next walk must not
        // be clicked before the player is through, or the game cancels the
        // walk-through; with `droppedInteracts` the first click does nothing
        // and the door is clicked again; with `hasEntryChat` the first click
        // raises a message box the executor must continue before anything moves.
        std::size_t testWalkThroughDoor(ExecContext &ctx, int droppedInteracts, bool hasEntryChat)
        {
            CrossAreaPick pick{};
            if (!pickCrossAreaPair(ctx.reader, ctx.view, acceptUngatedOneTileDoor, pick))
            {
                std::printf("  exec:   walk-through door test skipped (no one-tile door)\n");
                return 0;
            }
            const auto &edge = ctx.reader.areaEdges()[pick.edgeIndex];
            const auto &tx   = ctx.reader.transitions()[edge.transitionIndex];

            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateTransition, pick.start.x,
                                              pick.start.y, pick.startPlane);
            harness.transitionDest =
                exec::WwTile{ tx.destX, tx.destY, static_cast<std::int32_t>(tx.destPlane) };
            harness.landingDelayTicks = 4;
            harness.droppedInteracts  = droppedInteracts;
            harness.hasEntryChat      = hasEntryChat;
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwGoal goal{ pick.goal.x, pick.goal.y, pick.goalPlane, 0 };
            const exec::WwStatus status = executor.run(goal);

            const int wantInteracts = 1 + droppedInteracts;
            const int wantContinues = hasEntryChat ? 1 : 0;
            std::printf("  exec:   walk-through door tx%u dropped=%d chat=%d status=%d (expect 0)"
                        " interacts=%d (expect %d) continues=%d (expect %d) cancelled=%d"
                        " stucks=%d (expect 0, 0)\n",
                        edge.transitionIndex, droppedInteracts, hasEntryChat ? 1 : 0,
                        static_cast<int>(status), harness.interactCalls, wantInteracts,
                        harness.continueClicks, wantContinues, harness.cancelledLandings,
                        harness.stuckEvents);
            printCallPattern("walk-through ", harness);
            std::size_t failures = status == exec::WwStatus::Arrived ? 0u : 1u;
            failures += harness.interactCalls == wantInteracts ? 0u : 1u;
            failures += (harness.cancelledLandings == 0 && harness.stuckEvents == 0) ? 0u : 1u;
            failures += harness.continueClicks == wantContinues ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Test 4d: the game drops the first walk click (the player was still in
        // an agility obstacle's forced move). The stall re-clicks once before
        // calling it stuck, so the walk arrives without a Stuck or a re-plan.
        std::size_t testDroppedWalkClick(ExecContext &ctx)
        {
            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                              ctx.node.centroidX, ctx.node.centroidY, ctx.plane);
            harness.droppedWalkClicks = 1;
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwGoal goal{ ctx.farthest.x, ctx.farthest.y, ctx.plane, 0 };
            const exec::WwStatus status = executor.run(goal);

            std::printf("  exec:   dropped-click status=%d (expect 0=Arrived) stucks=%d replans=%d"
                        " (expect 0, 0) walks=%d (expect >= 2)\n",
                        static_cast<int>(status), harness.stuckEvents,
                        harness.replanStartedEvents, harness.walkToCalls);
            printCallPattern("dropped-click ", harness);
            std::size_t failures = status == exec::WwStatus::Arrived ? 0u : 1u;
            failures += (harness.stuckEvents == 0 && harness.replanStartedEvents == 0) ? 0u : 1u;
            failures += harness.walkToCalls >= 2 ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Test 4e: a non-door transition lands the player back where they
        // started (a failed jump into a pit) `failedLandings` times. Each is a
        // re-plan from the live position that spends no stuck budget; one
        // failure is recovered, endless failures end on that transition once
        // the reroute budget is gone rather than looping.
        std::size_t testOffCourseLanding(ExecContext &ctx, int failedLandings, bool isRecoverable)
        {
            CrossAreaPick pick{};
            if (!pickCrossAreaPair(ctx.reader, ctx.view, acceptUngatedNonDoor, pick))
            {
                std::printf("  exec:   off-course test skipped (no traversable non-door edge)\n");
                return 0;
            }
            const auto &edge = ctx.reader.areaEdges()[pick.edgeIndex];
            const auto &tx   = ctx.reader.transitions()[edge.transitionIndex];
            const exec::WwTile start{ pick.start.x, pick.start.y, pick.startPlane };
            const exec::WwTile dest{ tx.destX, tx.destY, static_cast<std::int32_t>(tx.destPlane) };
            const std::int32_t spread = std::max(std::abs(start.x - dest.x),
                                                 std::abs(start.y - dest.y));
            if (start.plane == dest.plane && spread <= runtime::kMaxSameFloorHop + 1)
            {
                std::printf("  exec:   off-course test skipped (tx%u lands too near its start)\n",
                            edge.transitionIndex);
                return 0;
            }

            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateTransition, start.x,
                                              start.y, start.plane);
            harness.transitionDest     = dest;
            harness.failedLandingsLeft = failedLandings;
            harness.failedLanding      = start;
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwGoal goal{ pick.goal.x, pick.goal.y, pick.goalPlane, 0 };
            const exec::WwStatus status = executor.run(goal);

            // One interact per attempt: the first plus one per reroute, and the
            // reroute budget is 3.
            const int wantInteracts = isRecoverable ? failedLandings + 1 : 4;
            const int txIndex = static_cast<int>(edge.transitionIndex);
            const bool isStatusRight = isRecoverable
                ? status == exec::WwStatus::Arrived
                : (status == exec::WwStatus::Failed && harness.failedTransitionIndex == txIndex);
            std::printf("  exec:   off-course tx%d fails=%d status=%d (expect %d) interacts=%d"
                        " (expect %d) stucks=%d (expect 0)\n",
                        txIndex, failedLandings, static_cast<int>(status),
                        isRecoverable ? 0 : 1, harness.interactCalls, wantInteracts,
                        harness.stuckEvents);
            printCallPattern("off-course ", harness);
            std::size_t failures = isStatusRight ? 0u : 1u;
            failures += harness.interactCalls == wantInteracts ? 0u : 1u;
            failures += harness.stuckEvents == 0 ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Test 4c: in combat at the first plan, out of it after the first step.
        // The walk-only goal needs no teleport, but the flip back out of combat
        // must still re-plan once, since that is the moment teleports come
        // back into consideration (V1 interrupted its walk the same way).
        std::size_t testCombatEndReplan(ExecContext &ctx)
        {
            if (!runtime::isTeleportAllowed(ctx.reader, ctx.node.centroidX, ctx.node.centroidY,
                                            ctx.plane))
            {
                std::printf("  exec:   combat test skipped (start tile is not teleport-allowed)\n");
                return 0;
            }
            runtime::AreaSearch    refAreaSearch(ctx.reader);
            runtime::TileSearch    refTileSearch(ctx.view);
            runtime::PathAssembler refAssembler(ctx.reader, ctx.view, refAreaSearch, refTileSearch);
            runtime::Plan refPlan;
            if (!refAssembler.assemble(ctx.node.centroidX, ctx.node.centroidY, ctx.plane,
                                       ctx.farthest.x, ctx.farthest.y, ctx.plane, refPlan)
                || refPlan.steps.size() < 2)
            {
                std::printf("  exec:   combat test skipped (walk has fewer than two steps)\n");
                return 0;
            }

            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                              ctx.node.centroidX, ctx.node.centroidY, ctx.plane);
            harness.inCombatReadsLeft = 1;
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwGoal goal{ ctx.farthest.x, ctx.farthest.y, ctx.plane, 0 };
            const exec::WwStatus status = executor.run(goal);

            std::printf("  exec:   combat-end status=%d (expect 0=Arrived) replans=%d (expect 1)"
                        " combat-reads=%d (expect >= 2)\n",
                        static_cast<int>(status), harness.replanStartedEvents,
                        harness.combatReads);
            printCallPattern("combat ", harness);
            std::size_t failures = status == exec::WwStatus::Arrived ? 0u : 1u;
            failures += harness.replanStartedEvents == 1 ? 0u : 1u;
            failures += harness.combatReads >= 2 ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Test 5: drive the same walk-only goal as Test 2 through the public C
        // ABI (ww_executor_run) instead of constructing the C++ Executor
        // directly. Validates that the artifact / pool handles, the WwCallbacks
        // wire-shape, and the FFI entry all round-trip cleanly.
        std::size_t testFfiExecutorRun(ExecContext &ctx, ww_artifact *cArt, ww_context_pool *cPool)
        {
            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                              ctx.node.centroidX, ctx.node.centroidY, ctx.plane);
            WwCallbacks cb = kCallbackPrototype;
            cb.user = &harness;

            const WwGoal goal{ ctx.farthest.x, ctx.farthest.y, ctx.plane, 0 };
            const std::int32_t status = ww_executor_run(cArt, cPool, goal, &cb);

            std::printf("  exec:   ffi status=%d (expect 0=Arrived) lastEvent=%d (expect %d)\n",
                        static_cast<int>(status), static_cast<int>(harness.lastEventKind),
                        static_cast<int>(exec::WwEventKind::Arrived));
            printCallPattern("ffi ", harness);
            std::size_t failures = (status == WW_STATUS_ARRIVED
                                    && harness.lastEventKind == exec::WwEventKind::Arrived)
                                       ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            printLanding("ffi", harness, ctx.farthest, ctx.plane);
            return failures;
        }

        // Test 6: drive ww_query (the C ABI query entry) for the same walk-only
        // plan as Test 2 and compare the returned WwPath byte-for-byte to a
        // direct in-process assembler.assemble call. Validates the FFI surface
        // (handles, capability-snapshot wire shape, malloc'd steps buffer,
        // ww_path_free lifecycle) and that the memcpy across the boundary
        // preserves the runtime::Step layout.
        std::size_t testFfiQuery(ExecContext &ctx, ww_artifact *cArt, ww_context_pool *cPool)
        {
            runtime::AreaSearch    refAreaSearch(ctx.reader);
            runtime::TileSearch    refTileSearch(ctx.view);
            runtime::PathAssembler refAssembler(ctx.reader, ctx.view, refAreaSearch, refTileSearch);
            runtime::Plan refPlan;
            const bool refOk = refAssembler.assemble(ctx.node.centroidX, ctx.node.centroidY,
                                                     ctx.plane, ctx.farthest.x, ctx.farthest.y,
                                                     ctx.plane, refPlan);

            const WwTile queryStart{ ctx.node.centroidX, ctx.node.centroidY, ctx.plane };
            const WwGoal queryGoal{ ctx.farthest.x, ctx.farthest.y, ctx.plane, 0 };
            WwPath ffiPath{};
            const ww_result queryStatus = ww_query(cArt, cPool, queryStart, queryGoal,
                                                   nullptr, &ffiPath);

            const bool stepsMatch = (refOk && queryStatus == WW_OK
                                  && ffiPath.stepCount == refPlan.steps.size()
                                  && (ffiPath.stepCount == 0
                                      || std::memcmp(ffiPath.steps, refPlan.steps.data(),
                                                     ffiPath.stepCount * sizeof(WwStep)) == 0));
            const bool costMatch = (refOk && queryStatus == WW_OK
                                  && ffiPath.cost == refPlan.cost);
            std::printf("  exec:   ffi query status=%d (expect 0=OK) steps=%zu (ref=%zu match=%d)"
                        " cost=%.1f (ref=%.1f match=%d)\n",
                        static_cast<int>(queryStatus), ffiPath.stepCount, refPlan.steps.size(),
                        stepsMatch ? 1 : 0, static_cast<double>(ffiPath.cost),
                        static_cast<double>(refPlan.cost), costMatch ? 1 : 0);

            ww_path_free(&ffiPath);
            std::printf("  exec:   ffi query post-free steps=%p stepCount=%zu cost=%.1f\n",
                        static_cast<void *>(ffiPath.steps), ffiPath.stepCount,
                        static_cast<double>(ffiPath.cost));
            return (queryStatus == WW_OK && stepsMatch && costMatch) ? 0u : 1u;
        }

        // Tests 5 and 6 share the C ABI handles, so they are opened, used and
        // closed here rather than by either test.
        std::size_t testFfi(ExecContext &ctx, const char *artifactPath)
        {
            if (artifactPath == nullptr)
            {
                std::printf("  exec:   ffi test skipped (no artifact path)\n");
                return 0;
            }
            ww_artifact *cArt = ww_artifact_open(artifactPath);
            if (cArt == nullptr)
            {
                std::printf("  exec:   ffi ww_artifact_open failed: %s\n", ww_last_error());
                return 1;
            }
            ww_context_pool *cPool = ww_context_pool_create(cArt, 1);
            if (cPool == nullptr)
            {
                std::printf("  exec:   ffi ww_context_pool_create failed: %s\n", ww_last_error());
                ww_artifact_close(cArt);
                return 1;
            }
            std::size_t failures = testFfiExecutorRun(ctx, cArt, cPool);
            failures += testFfiQuery(ctx, cArt, cPool);
            ww_context_pool_destroy(cPool);
            ww_artifact_close(cArt);
            return failures;
        }
    }

    std::size_t runExecutorTests(const format::ArtifactReader &reader, const char *artifactPath)
    {
        const auto nodes = reader.areaNodes();
        if (nodes.empty())
        {
            std::printf("  exec:   skipped (no area nodes)\n");
            return 0;
        }
        runtime::ContextPool pool(reader, 1);
        runtime::WorldView   view(reader);
        const format::AreaNodeRecord &n0 = nodes[0];
        const std::int32_t plane = static_cast<std::int32_t>(n0.plane);
        ExecContext ctx{ reader, pool, view, n0, plane, { n0.centroidX, n0.centroidY } };

        std::size_t failures = testStartEqualsGoal(ctx);

        const std::int32_t area0 = view.areaAt(n0.centroidX, n0.centroidY, plane);
        ctx.farthest = farthestInArea(view, n0.centroidX, n0.centroidY, plane, area0, 24);
        if (ctx.farthest.x == n0.centroidX && ctx.farthest.y == n0.centroidY)
        {
            std::printf("  exec:   walk-loop skipped (no in-area target distinct from start)\n");
            return failures;
        }
        failures += testWalkLoop(ctx);
        failures += testTransition(ctx);
        failures += testReplanRecovery(ctx);
        failures += testMissingLoc(ctx, acceptUngatedDoor, "door");
        failures += testMissingLoc(ctx, acceptUngatedNonDoor, "non-door");
        failures += testCombatEndReplan(ctx);
        failures += testDroppedWalkClick(ctx);
        failures += testOffCourseLanding(ctx, 1, true);
        failures += testOffCourseLanding(ctx, 99, false);
        failures += testWalkThroughDoor(ctx, 0, false);
        failures += testWalkThroughDoor(ctx, 1, false);
        failures += testWalkThroughDoor(ctx, 0, true);
        failures += testFfi(ctx, artifactPath);
        return failures;
    }
}
