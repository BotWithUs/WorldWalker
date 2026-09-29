#include "cli/ExecutorTests.h"

#include "c_api/worldwalker_c.h"
#include "cli/HarnessPicks.h"
#include "data/Transitions.h"
#include "exec/Callbacks.h"
#include "exec/Executor.h"
#include "format/Artifact.h"
#include "format/MoveCategory.h"
#include "runtime/AreaSearch.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/ContextPool.h"
#include "runtime/PathAssembler.h"
#include "runtime/TeleportPolicy.h"
#include "runtime/TileScan.h"
#include "runtime/TileSearch.h"
#include "runtime/TransitionShape.h"
#include "runtime/WorldView.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <thread>
#include <vector>

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

        // One walk hop as the executor announced it: the plan step it named,
        // the plan it belongs to, its interaction hint, where the player stood,
        // and the tile the walkTo straight after the announcement clicked
        // (isClicked stays false until one does). Under a paced walk
        // (tilesPerTick), whether the player was still walking the previous
        // click when this hop was announced.
        struct HopRecord
        {
            std::int32_t stepIndex;
            int          plan;
            std::int32_t hint;
            exec::WwTile from;
            exec::WwTile target;
            bool         isClicked;
            bool         isMoving;
        };

        // One StepAdvanced of either kind, in order: the step, the plan,
        // whether it was a Transition (and which), its interaction hint, and
        // where the player stood when it was announced.
        struct StepRecord
        {
            std::int32_t stepIndex;
            int          plan;
            bool         isTransition;
            std::int32_t hint;
            std::int32_t transitionIndex;
            exec::WwTile at;
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
            // Interacts aimed at one transition (its loc id from its origin
            // tile), so a test can tell a retry of that transition from the
            // interacts of whatever route replaced it.
            std::int32_t      watchedObjectId;
            exec::WwTile      watchedOrigin;
            int               watchedInteracts;
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
            // SimulateTransition, when non-empty: every interact lands on the
            // destination of the transition it names (looked up here by loc,
            // origin and option), except the first loc the executor clicks,
            // whose first brokenLandingsLeft clicks do not move the player: a
            // transport that does not land. brokenInteracts counts every click
            // on that loc.
            std::span<const format::TransitionRecord> landingRecords;
            int               brokenLandingsLeft;
            // With landingRecords: the broken loc is not there at all. Every
            // interact on it answers 0, as the host does for a loc it cannot
            // find near the origin (Shantay Pass's gate, 2 tiles from the row).
            bool              isBrokenLocAbsent;
            bool              hasBrokenLoc;
            std::int32_t      brokenObjectId;
            exec::WwTile      brokenOrigin;
            int               brokenInteracts;
            // When set, a walk click moves the player only within the area of
            // the baked grid they stand in, as walls do: a walk into the next
            // area stalls. Without it the harness walk passes through any wall,
            // so a refused crossing followed by a walk would still arrive.
            runtime::WorldView *areaWalls;
            // SimulateTransition: ClickNpc steps received, and whether the NPC is
            // absent (the click does nothing and no interface ever opens).
            // npcOriginClicks counts only those searching from npcOrigin, the
            // transition under test, since a detour may start at another NPC.
            int               npcClicks;
            bool              isNpcAbsent;
            exec::WwTile      npcOrigin;
            int               npcOriginClicks;
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
            // The next walk click opens a message box (chatOnNextWalk) or the
            // option list (optionListOnNextWalk) instead of moving the player;
            // while either is open no walk moves. chatNeverCloses keeps the
            // message box open through every continue.
            bool              chatOnNextWalk;
            bool              chatNeverCloses;
            bool              optionListOnNextWalk;
            bool              isOptionListOpen;
            // DialogueAnswer steps the executor sent, and the last answer's text.
            int               answerCalls;
            char              lastAnswer[40];
            // What the stride tests read back: every walk hop and crossing in
            // the order the executor announced them (StepAdvanced), numbered
            // by plan (planGeneration counts ReplanStarted events), with the
            // tile the player stood on and the tile clicked for each hop. A
            // hop's StepAdvanced comes first; the next walkTo is its click
            // (isHopClickPending), any later one a re-click.
            int                     planGeneration;
            bool                    isHopClickPending;
            std::vector<HopRecord>  hops;
            std::vector<StepRecord> steps;
            // Walk clicks that leave the player walking but never on the
            // target: wanderPollsLeft polls of heading away from it (a winding
            // long hop), then the target; or, with isWanderingForever, a
            // player pacing between two tiles until the executor calls it
            // Stuck, with the time from that first click to the Stuck event.
            int                     wanderPollsLeft;
            bool                    isWanderingForever;
            bool                    hasWanderTarget;
            exec::WwTile            wanderTarget;
            std::chrono::steady_clock::time_point wanderStart;
            std::int64_t            wanderStuckMs;
            // SimulateInstantWalk / SimulateTransition, when above 0: a walk
            // click does not land at once but sets walkGoal, and every tick
            // slept carries the player up to this many tiles towards it (a
            // diagonal step is one tile), as the game walks (1) or runs (2).
            // A loc click ends the walk. ticksSlept counts every tick slept.
            int                     tilesPerTick;
            bool                    hasWalkGoal;
            exec::WwTile            walkGoal;
            int                     ticksSlept;
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
        constexpr std::int32_t kOptionList = 1188;
        constexpr std::int32_t kDialogueAnswerKind =
            static_cast<std::int32_t>(data::ChainStepKind::DialogueAnswer);
        constexpr std::int32_t kClickNpcKind =
            static_cast<std::int32_t>(data::ChainStepKind::ClickNpc);
        constexpr int kMaxDialogActionsPerRun = 20;  // the executor's per-run budget

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
            if (interfaceId == kOptionList)
            {
                return h->isOptionListOpen ? 1 : 0;
            }
            if (h->mode == ExecHarnessMode::SimulateTransition)
            {
                return h->isNpcAbsent ? 0 : 1;
            }
            recordUnexpected(h, HarnessCallback::IsInterfaceOpen);
            return 0;
        }

        // Whether a walk from `from` may reach `to` under areaWalls: both in
        // the same area of the baked grid.
        bool isSameArea(runtime::WorldView &view, const exec::WwTile &from, const exec::WwTile &to)
        {
            const std::int32_t fromArea = view.areaAt(from.x, from.y, from.plane);
            return fromArea >= 0 && fromArea == view.areaAt(to.x, to.y, to.plane);
        }

        bool isSameTile(const exec::WwTile &a, const exec::WwTile &b);

        extern "C" void harnessWalkTo(void *user, exec::WwTile target)
        {
            ExecHarness *h = static_cast<ExecHarness *>(user);
            ++h->walkToCalls;
            if (h->isHopClickPending)
            {
                h->isHopClickPending = false;
                h->hops.back().target = target;
                h->hops.back().isClicked = true;
            }
            if (h->isChatOpen || h->isOptionListOpen)
            {
                return;
            }
            if (h->chatOnNextWalk)
            {
                h->chatOnNextWalk = false;
                h->isChatOpen = true;
                return;
            }
            if (h->optionListOnNextWalk)
            {
                h->optionListOnNextWalk = false;
                h->isOptionListOpen = true;
                return;
            }
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
            if (h->areaWalls != nullptr && !isSameArea(*h->areaWalls, h->position, target))
            {
                return;
            }
            if (h->wanderPollsLeft > 0 || h->isWanderingForever)
            {
                // A re-click keeps the wander (and its clock) going.
                if (!h->hasWanderTarget)
                {
                    h->wanderStart = std::chrono::steady_clock::now();
                }
                h->wanderTarget = target;
                h->hasWanderTarget = true;
                return;
            }
            const bool isWalkingMode = h->mode == ExecHarnessMode::SimulateInstantWalk
                                    || h->mode == ExecHarnessMode::SimulateTransition;
            if (isWalkingMode && h->tilesPerTick > 0)
            {
                h->walkGoal = target;
                h->hasWalkGoal = !isSameTile(h->position, target);
            }
            else if (isWalkingMode)
            {
                h->position = target;
            }
            else if (h->mode == ExecHarnessMode::SimulateReplanRecovery)
            {
                // Stall until the harness flips walkToUpdatesPosition on the
                // executor's first ReplanStarted event. The first walk never
                // moves the simulated player, so the unmoved-player stall counter
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

        bool isSameTile(const exec::WwTile &a, const exec::WwTile &b)
        {
            return a.x == b.x && a.y == b.y && a.plane == b.plane;
        }

        // landingRecords mode: the first loc clicked is latched as broken and
        // does not move the player while brokenLandingsLeft lasts; every other
        // click lands on its own row's destination. False when the click found
        // no loc (the broken loc under isBrokenLocAbsent).
        bool landOnRecord(ExecHarness *h, std::int32_t objectId, const exec::WwTile &origin,
                          std::int32_t option)
        {
            if (!h->hasBrokenLoc)
            {
                h->hasBrokenLoc   = true;
                h->brokenObjectId = objectId;
                h->brokenOrigin   = origin;
            }
            const bool isBroken =
                objectId == h->brokenObjectId && isSameTile(origin, h->brokenOrigin);
            if (isBroken)
            {
                ++h->brokenInteracts;
            }
            if (isBroken && h->isBrokenLocAbsent)
            {
                return false;
            }
            if (isBroken && h->brokenLandingsLeft > 0)
            {
                --h->brokenLandingsLeft;
                return true;
            }
            for (const format::TransitionRecord &tx : h->landingRecords)
            {
                const exec::WwTile txOrigin{ tx.originX, tx.originY,
                                             static_cast<std::int32_t>(tx.originPlane) };
                if (tx.objectId == objectId && isSameTile(txOrigin, origin)
                    && static_cast<std::int32_t>(tx.optionIndex) == option)
                {
                    h->position = exec::WwTile{ tx.destX, tx.destY,
                                                static_cast<std::int32_t>(tx.destPlane) };
                    return true;
                }
            }
            return true;
        }

        extern "C" std::int32_t harnessInteract(void *user, std::int32_t objectId,
                                                exec::WwTile origin, std::int32_t option)
        {
            ExecHarness *h = static_cast<ExecHarness *>(user);
            ++h->interactCalls;
            h->hasWalkGoal = false;  // a loc click replaces the walk
            if (objectId == h->watchedObjectId && origin.x == h->watchedOrigin.x
                && origin.y == h->watchedOrigin.y && origin.plane == h->watchedOrigin.plane)
            {
                ++h->watchedInteracts;
            }
            if (h->mode != ExecHarnessMode::SimulateTransition)
            {
                recordUnexpected(h, HarnessCallback::Interact);
            }
            else if (h->isLocMissing)
            {
                return 0;
            }
            else if (!h->landingRecords.empty())
            {
                if (!landOnRecord(h, objectId, origin, option))
                {
                    return 0;
                }
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

        // A DialogueAnswer step: unpack the answer, and close the option list
        // when it is the reply the harness's conversation wants ("Yes.").
        void harnessAnswer(ExecHarness *h, const std::int32_t (&slots)[9])
        {
            ++h->answerCalls;
            static_assert(sizeof(slots) < sizeof(h->lastAnswer));
            std::memcpy(h->lastAnswer, slots, sizeof(slots));
            h->lastAnswer[sizeof(slots)] = '\0';
            if (h->isOptionListOpen && std::strcmp(h->lastAnswer, "Yes.") == 0)
            {
                h->isOptionListOpen = false;
            }
        }

        extern "C" void harnessRunChainStep(void *user, std::int32_t kind, std::int32_t a,
                                            std::int32_t b, std::int32_t c, std::int32_t d,
                                            std::int32_t e, std::int32_t f, std::int32_t g,
                                            std::int32_t hh, std::int32_t i)
        {
            ExecHarness *h = static_cast<ExecHarness *>(user);
            if (kind == kDialogueAnswerKind)
            {
                harnessAnswer(h, { a, b, c, d, e, f, g, hh, i });
                return;
            }
            if (a == kDialogueAction && d == kMessageBoxContinue && h->isChatOpen)
            {
                // The page closes (and a walk-through door starts carrying the
                // player in), unless the test wants a page that never does.
                ++h->continueClicks;
                if (!h->chatNeverCloses)
                {
                    h->isChatOpen = false;
                    h->pendingLandingTicks = h->landingDelayTicks;
                }
                return;
            }
            ++h->runChainStepCalls;
            const bool isNpcClick = kind == kClickNpcKind;
            if (isNpcClick)
            {
                ++h->npcClicks;
                // ClickNpc carries its search centre in b..d.
                const bool isAtOrigin =
                    b == h->npcOrigin.x && c == h->npcOrigin.y && d == h->npcOrigin.plane;
                h->npcOriginClicks += isAtOrigin ? 1 : 0;
            }
            if (h->mode != ExecHarnessMode::SimulateTransition)
            {
                recordUnexpected(h, HarnessCallback::RunChainStep);
            }
            else if (!(isNpcClick && h->isNpcAbsent))
            {
                h->position = h->transitionDest;
                h->hasWalkGoal = false;
            }
        }

        // One poll's worth of a wandering walk. Pacing forever: two tiles well
        // outside any arrival radius, alternately, so the player moves on every
        // poll and never lands; a real millisecond passes too, so the wait for
        // the wall-clock deadline is not a hot loop. Otherwise one tile further
        // from the target per poll, and on the last poll the target itself.
        void wanderOnePoll(ExecHarness *h)
        {
            if (h->isWanderingForever)
            {
                const std::int32_t paceX = h->position.x == h->wanderTarget.x + 10 ? 11 : 10;
                h->position = exec::WwTile{ h->wanderTarget.x + paceX, h->wanderTarget.y,
                                            h->wanderTarget.plane };
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return;
            }
            --h->wanderPollsLeft;
            if (h->wanderPollsLeft <= 0)
            {
                h->position = h->wanderTarget;
                h->hasWanderTarget = false;
                return;
            }
            h->position.x += h->position.x >= h->wanderTarget.x ? 1 : -1;
        }

        // One tick of a paced walk: up to tilesPerTick single-tile steps (a
        // diagonal step moves both axes) towards walkGoal, stopping on it.
        void walkOneTick(ExecHarness *h)
        {
            for (int t = 0; t < h->tilesPerTick && h->hasWalkGoal; ++t)
            {
                const std::int32_t dx = h->walkGoal.x - h->position.x;
                const std::int32_t dy = h->walkGoal.y - h->position.y;
                h->position.x += (dx > 0) - (dx < 0);
                h->position.y += (dy > 0) - (dy < 0);
                h->position.plane = h->walkGoal.plane;
                h->hasWalkGoal = !isSameTile(h->position, h->walkGoal);
            }
        }

        extern "C" void harnessSleepTicks(void *user, std::int32_t ticks)
        {
            ExecHarness *h = static_cast<ExecHarness *>(user);
            ++h->sleepTicksCalls;
            h->ticksSlept += ticks;
            for (std::int32_t t = 0; t < ticks; ++t)
            {
                walkOneTick(h);
            }
            if (h->hasWanderTarget)
            {
                wanderOnePoll(h);
            }
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
            if (kind == exec::WwEventKind::StepAdvanced)
            {
                const bool isTransition = event->transitionIndex >= 0;
                h->steps.push_back({ event->stepIndex, h->planGeneration, isTransition,
                                     event->interactionHint, event->transitionIndex,
                                     h->position });
                if (!isTransition)
                {
                    h->hops.push_back({ event->stepIndex, h->planGeneration,
                                        event->interactionHint, h->position, exec::WwTile{},
                                        false, h->hasWalkGoal });
                    h->isHopClickPending = true;
                }
            }
            else if (kind == exec::WwEventKind::Stuck)
            {
                ++h->stuckEvents;
                if (h->isWanderingForever)
                {
                    // Deadline reached: stop pacing and let the re-plan land.
                    const auto elapsed = std::chrono::steady_clock::now() - h->wanderStart;
                    h->wanderStuckMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                    h->isWanderingForever = false;
                    h->hasWanderTarget = false;
                    h->position = h->wanderTarget;
                }
            }
            else if (kind == exec::WwEventKind::Failed)
            {
                h->failedTransitionIndex = event->transitionIndex;
            }
            else if (kind == exec::WwEventKind::ReplanStarted)
            {
                ++h->replanStartedEvents;
                ++h->planGeneration;
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
                // Both reach the host: a Click, and the ClickNpc an NPC origin
                // opens its chain with.
                const std::uint8_t kind = chain[tx.chainStart + i].kind;
                if (kind == static_cast<std::uint8_t>(data::ChainStepKind::Click)
                    || kind == static_cast<std::uint8_t>(data::ChainStepKind::ClickNpc))
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
            // A global teleport and an NPC origin click no loc.
            const std::uint32_t chainAt = tx.chainStart;
            const bool isNpcOrigin = tx.chainCount != 0u
                && chainAt < ctx.reader.chainSteps().size()
                && ctx.reader.chainSteps()[chainAt].kind == static_cast<std::uint8_t>(kClickNpcKind);
            const int wantInteracts = (isGlobal || isNpcOrigin) ? 0 : 1;
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
                        harness.interactCalls, wantInteracts, harness.runChainStepCalls,
                        clickCount, harness.isInterfaceOpenCalls, clickCount);
            printCallPattern("", harness);
            std::size_t failures = (status == exec::WwStatus::Arrived
                                    && harness.lastEventKind == exec::WwEventKind::Arrived)
                                       ? 0u : 1u;
            failures += (harness.interactCalls == wantInteracts
                         && harness.runChainStepCalls == clickCount) ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Test 4: stuck -> re-plan -> recover. The first walk's walkTo does NOT
        // advance the simulated position, so the unmoved-player stall counter trips
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
        // the run carries on past it. Anything else is retried, then excluded
        // for the rest of the run and re-planned around: it must never be tried
        // again (the live bug was a map loc chosen, missed and chosen again on
        // every walk). Run once per kind, with `filter` choosing which.
        //
        // Whether either run then ARRIVES is not this test's business: the
        // harness player never crosses, and every other non-door loc is missing
        // too, so a detour may fail as well. What is checked is that a door was
        // not what failed its run, and that a missing non-door loc was tried
        // exactly kMissingLocAttempts times and then re-planned around.
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
            harness.watchedObjectId = tx.objectId;
            harness.watchedOrigin =
                exec::WwTile{ tx.originX, tx.originY, static_cast<std::int32_t>(tx.originPlane) };
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwGoal goal{ pick.goal.x, pick.goal.y, pick.goalPlane, 0 };
            const exec::WwStatus status = executor.run(goal);

            const int txIndex = static_cast<int>(edge.transitionIndex);
            const bool isFailedOnTx = status == exec::WwStatus::Failed
                                   && harness.failedTransitionIndex == txIndex;
            const int wantInteracts = isSkippable ? 1 : 3;
            std::printf("  exec:   missing-loc %s tx%d status=%d failedOnTx=%d"
                        " interacts-on-tx=%d (expect %d) replans=%d (expect %s)\n",
                        label, txIndex, static_cast<int>(status), isFailedOnTx ? 1 : 0,
                        harness.watchedInteracts, wantInteracts, harness.replanStartedEvents,
                        isSkippable ? "0" : ">= 1");
            printCallPattern("missing-loc ", harness);
            std::size_t failures = 0u;
            if (isSkippable)
            {
                // A skipped door must not be what failed the run.
                failures += isFailedOnTx ? 1u : 0u;
                failures += harness.replanStartedEvents == 0 ? 0u : 1u;
            }
            else
            {
                failures += status != exec::WwStatus::Cancelled ? 0u : 1u;
                failures += harness.replanStartedEvents >= 1 ? 0u : 1u;
            }
            failures += harness.watchedInteracts == wantInteracts ? 0u : 1u;
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

        // Test 4g: a walk click raises a plain chat page. The walk continues
        // it and clicks on, rather than reading the held player as a stall.
        std::size_t testChatDuringWalk(ExecContext &ctx)
        {
            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                              ctx.node.centroidX, ctx.node.centroidY, ctx.plane);
            harness.chatOnNextWalk = true;
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwGoal goal{ ctx.farthest.x, ctx.farthest.y, ctx.plane, 0 };
            const exec::WwStatus status = executor.run(goal);

            std::printf("  exec:   walk-chat status=%d (expect 0) continues=%d (expect 1)"
                        " stucks=%d replans=%d (expect 0, 0)\n",
                        static_cast<int>(status), harness.continueClicks, harness.stuckEvents,
                        harness.replanStartedEvents);
            printCallPattern("walk-chat ", harness);
            std::size_t failures = status == exec::WwStatus::Arrived ? 0u : 1u;
            failures += harness.continueClicks == 1 ? 0u : 1u;
            failures += (harness.stuckEvents == 0 && harness.replanStartedEvents == 0) ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Test 4h: a chat page that never closes. The run spends its dialog
        // budget and no more, then fails like any other stuck walk.
        std::size_t testChatNeverCloses(ExecContext &ctx)
        {
            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                              ctx.node.centroidX, ctx.node.centroidY, ctx.plane);
            harness.chatOnNextWalk = true;
            harness.chatNeverCloses = true;
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwGoal goal{ ctx.farthest.x, ctx.farthest.y, ctx.plane, 0 };
            const exec::WwStatus status = executor.run(goal);

            std::printf("  exec:   stuck-chat status=%d (expect 1) continues=%d (expect %d)\n",
                        static_cast<int>(status), harness.continueClicks,
                        kMaxDialogActionsPerRun);
            std::size_t failures = status == exec::WwStatus::Failed ? 0u : 1u;
            failures += harness.continueClicks == kMaxDialogActionsPerRun ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // A standable tile inside the first dialog zone, and a walk goal a few
        // tiles on in the same area; false when the artifact has no zone.
        bool pickZoneWalk(ExecContext &ctx, runtime::TilePoint &outStart,
                          runtime::TilePoint &outGoal, std::int32_t &outPlane,
                          const char *&outAnswer)
        {
            const auto zones = ctx.reader.dialogZones();
            if (zones.empty())
            {
                return false;
            }
            const format::DialogZoneRecord &zone = zones[0];
            outPlane = static_cast<std::int32_t>(zone.planeMin);
            outAnswer = ctx.reader.dialogAnswers()[zone.answerStart].text;
            for (std::int32_t y = zone.minY; y <= zone.maxY; ++y)
            {
                for (std::int32_t x = zone.minX; x <= zone.maxX; ++x)
                {
                    const std::int32_t area = ctx.view.areaAt(x, y, outPlane);
                    if (area < 0)
                    {
                        continue;
                    }
                    outStart = { x, y };
                    outGoal = farthestInArea(ctx.view, x, y, outPlane, area, 6);
                    if (outGoal.x != x || outGoal.y != y)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        // Test 4i: inside a dialog zone, a walk click raises the option list.
        // The executor sends the zone's answer and walks on once it closes.
        std::size_t testZoneAnswer(ExecContext &ctx)
        {
            runtime::TilePoint start{};
            runtime::TilePoint goal{};
            std::int32_t plane = 0;
            const char *answer = nullptr;
            if (!pickZoneWalk(ctx, start, goal, plane, answer))
            {
                std::printf("  exec:   zone-answer test skipped (artifact has no dialog zone)\n");
                return 0;
            }
            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk, start.x,
                                              start.y, plane);
            harness.optionListOnNextWalk = true;
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwStatus status = executor.run(exec::WwGoal{ goal.x, goal.y, plane, 0 });

            const bool isAnswerRight = std::strncmp(harness.lastAnswer, answer, 36) == 0;
            std::printf("  exec:   zone-answer (%d,%d,p%d) status=%d (expect 0) answers=%d"
                        " (expect 1) sent='%s' (expect '%.36s')\n",
                        start.x, start.y, plane, static_cast<int>(status), harness.answerCalls,
                        harness.lastAnswer, answer);
            std::size_t failures = status == exec::WwStatus::Arrived ? 0u : 1u;
            failures += (harness.answerCalls == 1 && isAnswerRight) ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Test 4j: the same option list outside every dialog zone. The
        // executor must never pick an option there.
        std::size_t testNoZoneNeverAnswers(ExecContext &ctx)
        {
            for (const format::DialogZoneRecord &z : ctx.reader.dialogZones())
            {
                if (ctx.node.centroidX >= z.minX && ctx.node.centroidX <= z.maxX
                    && ctx.node.centroidY >= z.minY && ctx.node.centroidY <= z.maxY)
                {
                    std::printf("  exec:   no-zone test skipped (start lies in a zone)\n");
                    return 0;
                }
            }
            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                              ctx.node.centroidX, ctx.node.centroidY, ctx.plane);
            harness.optionListOnNextWalk = true;
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwGoal goal{ ctx.farthest.x, ctx.farthest.y, ctx.plane, 0 };
            const exec::WwStatus status = executor.run(goal);

            std::printf("  exec:   no-zone status=%d (expect 1) answers=%d (expect 0)\n",
                        static_cast<int>(status), harness.answerCalls);
            std::size_t failures = status == exec::WwStatus::Failed ? 0u : 1u;
            failures += harness.answerCalls == 0 ? 0u : 1u;
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

        // Test 4e: every transition lands the player back where they started
        // (a failed jump into a pit) `failedLandings` times. Each is a re-plan
        // from the live position that spends no stuck budget; one failure is
        // recovered, endless failures end on a transition once the reroute
        // budget is gone rather than looping. Which transitions the reroutes
        // try (a detour, or the same one again when it is the only way) is
        // tests 4f and 4g's business; this one counts attempts.
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
                : (status == exec::WwStatus::Failed && harness.failedTransitionIndex >= 0);
            std::printf("  exec:   off-course tx%d fails=%d status=%d (expect %d) interacts=%d"
                        " (expect %d) failedOn=tx%d stucks=%d (expect 0)\n",
                        txIndex, failedLandings, static_cast<int>(status),
                        isRecoverable ? 0 : 1, harness.interactCalls, wantInteracts,
                        harness.failedTransitionIndex, harness.stuckEvents);
            printCallPattern("off-course ", harness);
            std::size_t failures = isStatusRight ? 0u : 1u;
            failures += harness.interactCalls == wantInteracts ? 0u : 1u;
            failures += harness.stuckEvents == 0 ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // A transition the off-course test can watch miss: local, ungated, not
        // a door, no chain (the landing is decided at the interact), and a
        // destination far enough from its origin that staying put reads as
        // off course.
        bool isFarLandingNonDoor(const format::TransitionRecord &tx)
        {
            const std::int32_t spread = std::max(std::abs(tx.originX - tx.destX),
                                                 std::abs(tx.originY - tx.destY));
            const bool isFar = tx.originPlane != tx.destPlane
                            || spread > runtime::kMaxSameFloorHop + 3;
            return acceptUngatedNonDoor(tx) && tx.objectId > 0 && tx.chainCount == 0u && isFar;
        }

        bool isSameLocRow(const format::TransitionRecord &a, const format::TransitionRecord &b)
        {
            return a.objectId == b.objectId && a.originX == b.originX
                && a.originY == b.originY && a.originPlane == b.originPlane;
        }

        // Two edges between the same pair of areas through different locs, both
        // accepted by `filter`: a route with a spare. The pick starts on the
        // first edge's interact tile and ends on its destination.
        bool pickParallelPair(const format::ArtifactReader &reader, runtime::WorldView &view,
                              TransitionFilter filter, CrossAreaPick &outPick)
        {
            const auto edges = reader.areaEdges();
            const auto txs   = reader.transitions();
            for (std::size_t i = 0; i < edges.size(); ++i)
            {
                const format::TransitionRecord &a = txs[edges[i].transitionIndex];
                if (!filter(a))
                {
                    continue;
                }
                for (std::size_t j = i + 1; j < edges.size(); ++j)
                {
                    const format::TransitionRecord &b = txs[edges[j].transitionIndex];
                    const bool isSpare = edges[j].fromArea == edges[i].fromArea
                                      && edges[j].toArea == edges[i].toArea
                                      && filter(b) && !isSameLocRow(a, b);
                    if (!isSpare)
                    {
                        continue;
                    }
                    const std::int32_t plane = static_cast<std::int32_t>(a.originPlane);
                    const auto standableInArea = [&](std::int32_t x, std::int32_t y)
                    {
                        return view.isStandable(x, y, plane)
                            && view.areaAt(x, y, plane) == edges[i].fromArea;
                    };
                    std::int32_t startX = 0;
                    std::int32_t startY = 0;
                    if (!runtime::findNearestTile(a.originX, a.originY,
                                                  data::kTransitionApproachRadius, true,
                                                  standableInArea, a.originX, a.originY,
                                                  startX, startY))
                    {
                        break;
                    }
                    outPick.start      = { startX, startY };
                    outPick.startPlane = plane;
                    outPick.goal       = { a.destX, a.destY };
                    outPick.goalPlane  = static_cast<std::int32_t>(a.destPlane);
                    outPick.edgeIndex  = i;
                    return true;
                }
            }
            return false;
        }

        // Test 4f: the transition the plan picks never lands (the live bug: the
        // campus map chosen, missed and chosen again until the reroute budget
        // ran out). Another loc reaches the same area, so the reroute must go
        // round: the broken loc is clicked once, the spare once, and the run
        // arrives.
        std::size_t testOffCourseWithSpare(ExecContext &ctx)
        {
            CrossAreaPick pick{};
            if (!pickParallelPair(ctx.reader, ctx.view, isFarLandingNonDoor, pick))
            {
                std::printf("  exec:   off-course spare test skipped (no parallel edge pair)\n");
                return 0;
            }
            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateTransition, pick.start.x,
                                              pick.start.y, pick.startPlane);
            harness.landingRecords     = ctx.reader.transitions();
            harness.brokenLandingsLeft = 99;
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwGoal goal{ pick.goal.x, pick.goal.y, pick.goalPlane, 0 };
            const exec::WwStatus status = executor.run(goal);

            std::printf("  exec:   off-course spare edge%zu status=%d (expect 0) broken-loc=%d"
                        " clicks=%d (expect 1) interacts=%d (expect 2) replans=%d (expect >= 1)\n",
                        pick.edgeIndex, static_cast<int>(status), harness.brokenObjectId,
                        harness.brokenInteracts, harness.interactCalls,
                        harness.replanStartedEvents);
            printCallPattern("off-course spare ", harness);
            std::size_t failures = status == exec::WwStatus::Arrived ? 0u : 1u;
            failures += harness.brokenInteracts == 1 ? 0u : 1u;
            failures += harness.interactCalls == 2 ? 0u : 1u;
            failures += harness.replanStartedEvents >= 1 ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // An edge `filter` accepts that is the only edge into its area. Areas
        // are walk-connected components, so nothing but that edge reaches it
        // (the harness snapshot admits no global teleport): once it is ruled
        // out the planner has no route.
        bool pickSoleEntry(const format::ArtifactReader &reader, runtime::WorldView &view,
                           TransitionFilter filter, CrossAreaPick &outPick)
        {
            const auto edges = reader.areaEdges();
            const auto txs   = reader.transitions();
            std::vector<std::uint32_t> entries(reader.areaNodes().size(), 0u);
            for (const format::AreaEdgeRecord &edge : edges)
            {
                if (static_cast<std::size_t>(edge.toArea) < entries.size())
                {
                    ++entries[static_cast<std::size_t>(edge.toArea)];
                }
            }
            for (std::size_t i = 0; i < edges.size(); ++i)
            {
                const format::AreaEdgeRecord &edge = edges[i];
                const format::TransitionRecord &tx = txs[edge.transitionIndex];
                const bool isSole = static_cast<std::size_t>(edge.toArea) < entries.size()
                                 && entries[static_cast<std::size_t>(edge.toArea)] == 1u
                                 && edge.fromArea != edge.toArea;
                if (!isSole || !filter(tx))
                {
                    continue;
                }
                const std::int32_t plane = static_cast<std::int32_t>(tx.originPlane);
                const auto standableInArea = [&](std::int32_t x, std::int32_t y)
                {
                    return view.isStandable(x, y, plane) && view.areaAt(x, y, plane) == edge.fromArea;
                };
                std::int32_t startX = 0;
                std::int32_t startY = 0;
                if (!runtime::findNearestTile(tx.originX, tx.originY,
                                              data::kTransitionApproachRadius, true,
                                              standableInArea, tx.originX, tx.originY,
                                              startX, startY))
                {
                    continue;
                }
                outPick.start      = { startX, startY };
                outPick.startPlane = plane;
                outPick.goal       = { tx.destX, tx.destY };
                outPick.goalPlane  = static_cast<std::int32_t>(tx.destPlane);
                outPick.edgeIndex  = i;
                return true;
            }
            return false;
        }

        // Test 4g: the only transition into the goal's area misses once (a
        // failed jump on the only way across). Ruling it out leaves no route,
        // so the reroute puts it back and tries again: clicked twice, arrived.
        std::size_t testOffCourseSoleEntry(ExecContext &ctx)
        {
            CrossAreaPick pick{};
            if (!pickSoleEntry(ctx.reader, ctx.view, isFarLandingNonDoor, pick))
            {
                std::printf("  exec:   off-course sole-entry test skipped (no such edge)\n");
                return 0;
            }
            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateTransition, pick.start.x,
                                              pick.start.y, pick.startPlane);
            harness.landingRecords     = ctx.reader.transitions();
            harness.brokenLandingsLeft = 1;
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwGoal goal{ pick.goal.x, pick.goal.y, pick.goalPlane, 0 };
            const exec::WwStatus status = executor.run(goal);

            const auto &edge = ctx.reader.areaEdges()[pick.edgeIndex];
            std::printf("  exec:   off-course sole-entry tx%u status=%d (expect 0) clicks=%d"
                        " (expect 2) interacts=%d (expect 2)\n",
                        edge.transitionIndex, static_cast<int>(status), harness.brokenInteracts,
                        harness.interactCalls);
            printCallPattern("off-course sole-entry ", harness);
            std::size_t failures = status == exec::WwStatus::Arrived ? 0u : 1u;
            failures += harness.brokenInteracts == 2 ? 0u : 1u;
            failures += harness.interactCalls == 2 ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // A crossing the refusal tests can watch: an ungated same-floor crossing
        // (stile, gate, door) with a loc and no chain, so the landing is decided
        // at the interact. Its two sides lie within the executor's distance
        // slack of each other, so distance alone cannot tell a refusal.
        bool isShortCrossing(const format::TransitionRecord &tx)
        {
            return acceptUngatedDoor(tx) && tx.objectId > 0 && tx.chainCount == 0u;
        }

        // Whether the loc the harness latched as broken is a same-floor
        // crossing, so a refusal test knows the planner clicked the kind of
        // crossing it meant to refuse and not some detour.
        bool isBrokenLocShort(const ExecHarness &h)
        {
            for (const format::TransitionRecord &tx : h.landingRecords)
            {
                const exec::WwTile origin{ tx.originX, tx.originY,
                                           static_cast<std::int32_t>(tx.originPlane) };
                if (tx.objectId == h.brokenObjectId && isSameTile(origin, h.brokenOrigin))
                {
                    return runtime::isSameFloorCrossing(tx);
                }
            }
            return false;
        }

        // Runs one walk from `pick` with every interact landing on its row's
        // destination except the first loc clicked, which refuses the player
        // `refusals` times (they stay where they clicked), or is not there at
        // all when `isLocAbsent`, and walls that keep each walk inside its area.
        exec::WwStatus runRefusalWalk(ExecContext &ctx, const CrossAreaPick &pick, int refusals,
                                      bool isLocAbsent, ExecHarness &outHarness)
        {
            outHarness = makeHarness(ExecHarnessMode::SimulateTransition, pick.start.x,
                                     pick.start.y, pick.startPlane);
            outHarness.landingRecords     = ctx.reader.transitions();
            outHarness.brokenLandingsLeft = refusals;
            outHarness.isBrokenLocAbsent  = isLocAbsent;
            outHarness.areaWalls          = &ctx.view;
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &outHarness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            return executor.run(exec::WwGoal{ pick.goal.x, pick.goal.y, pick.goalPlane, 0 });
        }

        // Test 4k: the live Tree Gnome Stronghold stile. A crossing two tiles
        // wide refuses the player, who stays within the distance slack of its
        // far side; another crossing joins the same two areas. The player is
        // still in the area they clicked from, so the crossing is excluded
        // after its one attempt (the click and the same-floor re-click) and
        // the run goes round by the spare. The old executor judged by
        // distance, walked on into the wall and spent its stuck budget.
        std::size_t testRefusedCrossingWithSpare(ExecContext &ctx)
        {
            CrossAreaPick pick{};
            if (!pickParallelPair(ctx.reader, ctx.view, isShortCrossing, pick))
            {
                std::printf("  exec:   refused-crossing spare test skipped (no parallel pair)\n");
                return 0;
            }
            ExecHarness harness{};
            const exec::WwStatus status = runRefusalWalk(ctx, pick, 99, false, harness);

            const bool isShort = isBrokenLocShort(harness);
            std::printf("  exec:   refused-crossing spare edge%zu status=%d (expect 0) broken-loc=%d"
                        " short=%d (expect 1) clicks=%d (expect 2) interacts=%d (expect 3)"
                        " stucks=%d (expect 0) replans=%d (expect >= 1)\n",
                        pick.edgeIndex, static_cast<int>(status), harness.brokenObjectId,
                        isShort ? 1 : 0, harness.brokenInteracts, harness.interactCalls,
                        harness.stuckEvents, harness.replanStartedEvents);
            printCallPattern("refused-crossing spare ", harness);
            std::size_t failures = status == exec::WwStatus::Arrived ? 0u : 1u;
            failures += isShort ? 0u : 1u;
            failures += harness.brokenInteracts == 2 ? 0u : 1u;
            failures += harness.interactCalls == 3 ? 0u : 1u;
            failures += harness.stuckEvents == 0 ? 0u : 1u;
            failures += harness.replanStartedEvents >= 1 ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Test 4l: the only crossing into an area refuses the player every
        // time. Each refusal is a reroute (ruled out, nothing else reaches the
        // goal, put back, tried again) and never a walk into the wall, so the
        // run fails on the crossing once the reroute budget is spent: four
        // attempts of two clicks, no Stuck.
        std::size_t testRefusedSoleCrossing(ExecContext &ctx)
        {
            CrossAreaPick pick{};
            if (!pickSoleEntry(ctx.reader, ctx.view, isShortCrossing, pick))
            {
                std::printf("  exec:   refused-crossing sole test skipped (no such edge)\n");
                return 0;
            }
            ExecHarness harness{};
            const exec::WwStatus status = runRefusalWalk(ctx, pick, 99, false, harness);

            const auto &edge = ctx.reader.areaEdges()[pick.edgeIndex];
            std::printf("  exec:   refused-crossing sole tx%u status=%d (expect 1) failedOn=tx%d"
                        " clicks=%d (expect 8) interacts=%d (expect 8) stucks=%d (expect 0)\n",
                        edge.transitionIndex, static_cast<int>(status),
                        harness.failedTransitionIndex, harness.brokenInteracts,
                        harness.interactCalls, harness.stuckEvents);
            printCallPattern("refused-crossing sole ", harness);
            std::size_t failures = status == exec::WwStatus::Failed ? 0u : 1u;
            failures += harness.failedTransitionIndex >= 0 ? 0u : 1u;
            failures += harness.brokenInteracts == 8 ? 0u : 1u;
            failures += harness.interactCalls == 8 ? 0u : 1u;
            failures += harness.stuckEvents == 0 ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Test 4l2: the only crossing into an area is a door already open, and
        // the host clicks the closed loc the open one hides (Sinclair Mansion's
        // gate, Falador castle's doors): every click is issued and none moves
        // the player. No walls, since the doorway is open. After the click and
        // the re-click the executor walks across, and the run arrives on that
        // one attempt instead of rerouting onto the same door until it fails.
        std::size_t testDeadClickOnOpenCrossing(ExecContext &ctx)
        {
            CrossAreaPick pick{};
            if (!pickSoleEntry(ctx.reader, ctx.view, isShortCrossing, pick))
            {
                std::printf("  exec:   dead-click open crossing test skipped (no such edge)\n");
                return 0;
            }
            // Not runRefusalWalk: it walls every area, and this doorway is open.
            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateTransition, pick.start.x,
                                  pick.start.y, pick.startPlane);
            harness.landingRecords     = ctx.reader.transitions();
            harness.brokenLandingsLeft = 99;
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;
            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwStatus status =
                executor.run(exec::WwGoal{ pick.goal.x, pick.goal.y, pick.goalPlane, 0 });

            const auto &edge = ctx.reader.areaEdges()[pick.edgeIndex];
            std::printf("  exec:   dead-click open crossing tx%u status=%d (expect 0) clicks=%d"
                        " (expect 2) replans=%d (expect 0) stucks=%d (expect 0)\n",
                        edge.transitionIndex, static_cast<int>(status), harness.brokenInteracts,
                        harness.replanStartedEvents, harness.stuckEvents);
            printCallPattern("dead-click open crossing ", harness);
            std::size_t failures = status == exec::WwStatus::Arrived ? 0u : 1u;
            failures += harness.brokenInteracts == 2 ? 0u : 1u;
            failures += harness.replanStartedEvents == 0 ? 0u : 1u;
            failures += harness.stuckEvents == 0 ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Move `ioPick`'s goal from its crossing's destination to a standable
        // tile of the destination's area 3 to 6 tiles further from the origin,
        // so the plan ends in a walk beyond the crossing rather than on it: a
        // skipped crossing is only walked through by the walk after it. False
        // when the area has no such tile.
        bool movePickGoalPastCrossing(ExecContext &ctx, CrossAreaPick &ioPick)
        {
            const format::AreaEdgeRecord &edge = ctx.reader.areaEdges()[ioPick.edgeIndex];
            const format::TransitionRecord &tx = ctx.reader.transitions()[edge.transitionIndex];
            const std::int32_t plane = ioPick.goalPlane;
            const std::int32_t destArea = ctx.view.areaAt(ioPick.goal.x, ioPick.goal.y, plane);
            const std::int32_t destReach = std::max(std::abs(tx.destX - tx.originX),
                                                    std::abs(tx.destY - tx.originY));
            for (std::int32_t ring = 3; ring <= 6; ++ring)
            {
                for (std::int32_t dy = -ring; dy <= ring; ++dy)
                {
                    for (std::int32_t dx = -ring; dx <= ring; ++dx)
                    {
                        const std::int32_t x = ioPick.goal.x + dx;
                        const std::int32_t y = ioPick.goal.y + dy;
                        const std::int32_t reach = std::max(std::abs(x - tx.originX),
                                                            std::abs(y - tx.originY));
                        const bool isPast = reach > destReach + 1
                                         && ctx.view.isStandable(x, y, plane)
                                         && ctx.view.areaAt(x, y, plane) == destArea;
                        if (isPast)
                        {
                            ioPick.goal = { x, y };
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        // Test 4o: the live Shantay Pass. A same-floor crossing's loc is not
        // there, so the executor skips it as an open door, but nothing is open
        // and the walk beyond stalls on the near side. The crossing is then
        // excluded like a missing loc and the run goes round by the spare,
        // never clicking it again. The old executor re-planned onto the same
        // crossing after every stall and failed the walk with its re-plan
        // budget spent (four clicks, four Stucks).
        std::size_t testAbsentCrossingWithSpare(ExecContext &ctx)
        {
            CrossAreaPick pick{};
            if (!pickParallelPair(ctx.reader, ctx.view, isShortCrossing, pick)
                || !movePickGoalPastCrossing(ctx, pick))
            {
                std::printf("  exec:   absent-crossing spare test skipped (no parallel pair)\n");
                return 0;
            }
            ExecHarness harness{};
            const exec::WwStatus status = runRefusalWalk(ctx, pick, 0, true, harness);

            const bool isShort = isBrokenLocShort(harness);
            std::printf("  exec:   absent-crossing spare edge%zu status=%d (expect 0) broken-loc=%d"
                        " short=%d (expect 1) clicks=%d (expect 1) stucks=%d (expect 1)"
                        " replans=%d (expect >= 1)\n",
                        pick.edgeIndex, static_cast<int>(status), harness.brokenObjectId,
                        isShort ? 1 : 0, harness.brokenInteracts, harness.stuckEvents,
                        harness.replanStartedEvents);
            printCallPattern("absent-crossing spare ", harness);
            std::size_t failures = status == exec::WwStatus::Arrived ? 0u : 1u;
            failures += isShort ? 0u : 1u;
            failures += harness.brokenInteracts == 1 ? 0u : 1u;
            failures += harness.stuckEvents == 1 ? 0u : 1u;
            failures += harness.replanStartedEvents >= 1 ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // A blocked tile within `radius` of (cx, cy) whose every standable tile
        // within the goal-snap reach (3) lies in `area`, so wherever the
        // planner snaps it stays in the start's area.
        bool pickBlockedGoal(runtime::WorldView &view, std::int32_t cx, std::int32_t cy,
                             std::int32_t plane, std::int32_t area, std::int32_t radius,
                             runtime::TilePoint &outGoal)
        {
            constexpr std::int32_t kSnapReach = 3;
            for (std::int32_t x = cx - radius; x <= cx + radius; ++x)
            {
                for (std::int32_t y = cy - radius; y <= cy + radius; ++y)
                {
                    if (view.isStandable(x, y, plane))
                    {
                        continue;
                    }
                    bool hasStandIn = false;
                    bool isAllInArea = true;
                    for (std::int32_t dx = -kSnapReach; dx <= kSnapReach; ++dx)
                    {
                        for (std::int32_t dy = -kSnapReach; dy <= kSnapReach; ++dy)
                        {
                            if (!view.isStandable(x + dx, y + dy, plane))
                            {
                                continue;
                            }
                            hasStandIn = true;
                            isAllInArea = isAllInArea && view.areaAt(x + dx, y + dy, plane) == area;
                        }
                    }
                    if (hasStandIn && isAllInArea)
                    {
                        outGoal = { x, y };
                        return true;
                    }
                }
            }
            return false;
        }

        // Test 4q: the goal tile is blocked (the Rusty Anchor's bartender at
        // 3050,3257), so the plan ends on the nearest standable tile. Reaching
        // it is arrival; the old executor judged the drained run by the goal
        // tile alone and returned Failed on the last step.
        std::size_t testBlockedGoal(ExecContext &ctx)
        {
            // The first roomy area whose centroid is its own standable tile
            // and has a blocked tile in reach.
            constexpr std::uint32_t kMinTiles = 64;
            const auto nodes = ctx.reader.areaNodes();
            runtime::TilePoint start{};
            runtime::TilePoint goal{};
            std::int32_t plane = 0;
            bool isPicked = false;
            for (std::size_t a = 0; a < nodes.size() && !isPicked; ++a)
            {
                const format::AreaNodeRecord &n = nodes[a];
                plane = static_cast<std::int32_t>(n.plane);
                const auto area = static_cast<std::int32_t>(a);
                start = { n.centroidX, n.centroidY };
                isPicked = n.tileCount >= kMinTiles
                    && ctx.view.areaAt(start.x, start.y, plane) == area
                    && pickBlockedGoal(ctx.view, start.x, start.y, plane, area, 8, goal);
            }
            if (!isPicked)
            {
                std::printf("  exec:   blocked-goal test skipped (no blocked tile in reach)\n");
                return 0;
            }
            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                              start.x, start.y, plane);
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;
            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwStatus status = executor.run(exec::WwGoal{ goal.x, goal.y, plane, 0 });

            const std::int32_t off = std::max(std::abs(harness.position.x - goal.x),
                                              std::abs(harness.position.y - goal.y));
            std::printf("  exec:   blocked-goal status=%d (expect 0) off=%d (expect 1..3)\n",
                        static_cast<int>(status), off);
            printLanding("blocked-goal", harness, goal, plane);
            std::size_t failures = status == exec::WwStatus::Arrived ? 0u : 1u;
            failures += (off >= 1 && off <= 3) ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Test 4r: the goal tile can be stood on but sits in a sealed pocket.
        // East Ardougne's north bank: the walk to the bank targeted the
        // banker's tile 2614,3330 behind the booth row, which no baked edge
        // reaches, and failed with no route eight times (live 2026-09-29). The
        // plan must end across the booth on 2614,3332, where the player banks,
        // not on 2614,3329 behind the bank's south wall, and standing there is
        // arrival. A fixed fixture on purpose: the rule is what is under test,
        // so the case is not re-derived by it.
        std::size_t testPocketGoal(ExecContext &ctx)
        {
            constexpr std::int32_t kPlane = 0;
            const runtime::TilePoint start{ 2614, 3334 };
            const runtime::TilePoint goal{ 2614, 3330 };
            const runtime::TilePoint expected{ 2614, 3332 };
            const std::int32_t startArea = ctx.view.areaAt(start.x, start.y, kPlane);
            const std::int32_t goalArea = ctx.view.areaAt(goal.x, goal.y, kPlane);
            if (startArea < 0 || goalArea < 0 || startArea == goalArea
                || ctx.view.areaAt(expected.x, expected.y, kPlane) != startArea)
            {
                std::printf("  exec:   pocket-goal test skipped (East Ardougne bank not baked"
                            " as a pocket)\n");
                return 0;
            }
            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                              start.x, start.y, kPlane);
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;
            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwStatus status = executor.run(exec::WwGoal{ goal.x, goal.y, kPlane, 0 });

            const bool isOnExpected = harness.position.x == expected.x
                && harness.position.y == expected.y && harness.position.plane == kPlane;
            std::printf("  exec:   pocket-goal status=%d (expect 0) at=(%d,%d) (expect %d,%d)\n",
                        static_cast<int>(status), harness.position.x, harness.position.y,
                        expected.x, expected.y);
            std::size_t failures = status == exec::WwStatus::Arrived ? 0u : 1u;
            failures += isOnExpected ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Test 4p: as 4o, but the absent crossing is the only way into the
        // goal's area. Once the stall shows it is not open, it is excluded and
        // nothing else reaches the goal, so the run fails on that crossing
        // after one click and one Stuck instead of looping on it.
        std::size_t testAbsentSoleCrossing(ExecContext &ctx)
        {
            CrossAreaPick pick{};
            if (!pickSoleEntry(ctx.reader, ctx.view, isShortCrossing, pick)
                || !movePickGoalPastCrossing(ctx, pick))
            {
                std::printf("  exec:   absent-crossing sole test skipped (no such edge)\n");
                return 0;
            }
            ExecHarness harness{};
            const exec::WwStatus status = runRefusalWalk(ctx, pick, 0, true, harness);

            const bool isShort = isBrokenLocShort(harness);
            std::printf("  exec:   absent-crossing sole edge%zu status=%d (expect 1) failedOn=tx%d"
                        " short=%d (expect 1) clicks=%d (expect 1) stucks=%d (expect 1)\n",
                        pick.edgeIndex, static_cast<int>(status), harness.failedTransitionIndex,
                        isShort ? 1 : 0, harness.brokenInteracts, harness.stuckEvents);
            printCallPattern("absent-crossing sole ", harness);
            std::size_t failures = status == exec::WwStatus::Failed ? 0u : 1u;
            failures += harness.failedTransitionIndex >= 0 ? 0u : 1u;
            failures += isShort ? 0u : 1u;
            failures += harness.brokenInteracts == 1 ? 0u : 1u;
            failures += harness.stuckEvents == 1 ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Tests 4m and 4n: a crossing that lets the player through still
        // counts as landed under the area judgement, short (`filter` =
        // isShortCrossing, the two sides within the distance slack) or long
        // (isFarLandingNonDoor, a ladder or ride). One click, no reroute, no
        // Stuck, arrived.
        std::size_t testCrossingLands(ExecContext &ctx, TransitionFilter filter, const char *label)
        {
            CrossAreaPick pick{};
            if (!pickCrossAreaPair(ctx.reader, ctx.view, filter, pick))
            {
                std::printf("  exec:   %s landing test skipped (no such edge)\n", label);
                return 0;
            }
            ExecHarness harness{};
            const exec::WwStatus status = runRefusalWalk(ctx, pick, 0, false, harness);

            const auto &edge = ctx.reader.areaEdges()[pick.edgeIndex];
            std::printf("  exec:   %s landing tx%u status=%d (expect 0) interacts=%d (expect 1)"
                        " replans=%d stucks=%d (expect 0, 0)\n",
                        label, edge.transitionIndex, static_cast<int>(status),
                        harness.interactCalls, harness.replanStartedEvents, harness.stuckEvents);
            printCallPattern("landing ", harness);
            std::size_t failures = status == exec::WwStatus::Arrived ? 0u : 1u;
            failures += harness.interactCalls == 1 ? 0u : 1u;
            failures += (harness.replanStartedEvents == 0 && harness.stuckEvents == 0) ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // An ungated edge whose transition starts at an NPC (its chain opens
        // with ClickNpc), with the pick starting on a standable tile beside the
        // NPC's tile. Only an artifact baked with an enabled `npc` row has one.
        bool pickNpcOrigin(const format::ArtifactReader &reader, runtime::WorldView &view,
                           CrossAreaPick &outPick)
        {
            const auto edges = reader.areaEdges();
            const auto txs   = reader.transitions();
            const auto chain = reader.chainSteps();
            for (std::size_t i = 0; i < edges.size(); ++i)
            {
                const format::AreaEdgeRecord &edge = edges[i];
                const format::TransitionRecord &tx = txs[edge.transitionIndex];
                const bool isNpc = tx.chainCount != 0u && tx.chainStart < chain.size()
                    && chain[tx.chainStart].kind == static_cast<std::uint8_t>(kClickNpcKind);
                if (!isNpc || tx.requirementCount != 0u)
                {
                    continue;
                }
                const std::int32_t plane = static_cast<std::int32_t>(tx.originPlane);
                const auto standableInArea = [&](std::int32_t x, std::int32_t y)
                {
                    return view.isStandable(x, y, plane) && view.areaAt(x, y, plane) == edge.fromArea;
                };
                std::int32_t startX = 0;
                std::int32_t startY = 0;
                if (!runtime::findNearestTile(tx.originX, tx.originY,
                                              data::kTransitionApproachRadius, true,
                                              standableInArea, tx.originX, tx.originY,
                                              startX, startY))
                {
                    continue;
                }
                outPick.start      = { startX, startY };
                outPick.startPlane = plane;
                outPick.goal       = { tx.destX, tx.destY };
                outPick.goalPlane  = static_cast<std::int32_t>(tx.destPlane);
                outPick.edgeIndex  = i;
                return true;
            }
            return false;
        }

        // Test 4h: a transition that starts at an NPC. Present, it is crossed
        // through its chain alone: one ClickNpc, no loc interact. Absent (no
        // NPC, so its interface never opens), it is routed around like a
        // missing loc rather than ending the run: clicked once, then a
        // re-plan. Whether that re-plan finds another way is the artifact's
        // business, not this test's.
        std::size_t testNpcOrigin(ExecContext &ctx, bool isNpcAbsent)
        {
            CrossAreaPick pick{};
            if (!pickNpcOrigin(ctx.reader, ctx.view, pick))
            {
                std::printf("  exec:   npc-origin test skipped (no enabled npc transition)\n");
                return 0;
            }
            const auto &edge = ctx.reader.areaEdges()[pick.edgeIndex];
            const auto &tx   = ctx.reader.transitions()[edge.transitionIndex];
            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateTransition, pick.start.x,
                                              pick.start.y, pick.startPlane);
            harness.transitionDest =
                exec::WwTile{ tx.destX, tx.destY, static_cast<std::int32_t>(tx.destPlane) };
            harness.isNpcAbsent = isNpcAbsent;
            harness.npcOrigin =
                exec::WwTile{ tx.originX, tx.originY, static_cast<std::int32_t>(tx.originPlane) };
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &harness;

            exec::Executor executor(ctx.reader, ctx.pool, cb);
            const exec::WwGoal goal{ pick.goal.x, pick.goal.y, pick.goalPlane, 0 };
            const exec::WwStatus status = executor.run(goal);

            std::printf("  exec:   npc-origin tx%u absent=%d status=%d npc-clicks=%d here=%d"
                        " (expect %s, 1 here) interacts=%d (expect %s) replans=%d (expect %s)\n",
                        edge.transitionIndex, isNpcAbsent ? 1 : 0, static_cast<int>(status),
                        harness.npcClicks, harness.npcOriginClicks, isNpcAbsent ? ">= 1" : "1",
                        harness.interactCalls, isNpcAbsent ? "any" : "0",
                        harness.replanStartedEvents, isNpcAbsent ? ">= 1" : "0");
            printCallPattern("npc-origin ", harness);
            // Absent, the detour may click locs and other NPCs of its own (every
            // NPC is absent here, so another port's charter fails the same
            // way); only this transition's NPC must not be tried again.
            std::size_t failures = harness.npcOriginClicks == 1 ? 0u : 1u;
            if (isNpcAbsent)
            {
                failures += harness.replanStartedEvents >= 1 ? 0u : 1u;
            }
            else
            {
                failures += harness.npcClicks == 1 ? 0u : 1u;
                failures += harness.interactCalls == 0 ? 0u : 1u;
                failures += status == exec::WwStatus::Arrived ? 0u : 1u;
            }
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Walk hops (tests 4s-4x). The executor clicks ahead over a random
        // stride of the plan's Walk steps, so each test drives many pinned
        // seeds and checks a rule every one of them must keep. The two limits
        // mirror the executor's own; a test that re-derived them from the
        // executor would pass whatever the executor did.
        constexpr std::uint32_t kStrideSeeds       = 25;
        constexpr std::int32_t  kMaxClickChebyshev = 32;
        constexpr std::int32_t  kWildernessMargin  = 8;
        constexpr std::int32_t  kOldStepTiles      = 16;  // the fixed click before strides

        std::int32_t tileChebyshev(const exec::WwTile &a, const exec::WwTile &b)
        {
            return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
        }

        exec::WwTile stepTile(const runtime::Step &step)
        {
            return exec::WwTile{ step.targetX, step.targetY, static_cast<std::int32_t>(step.plane) };
        }

        // The plan the executor makes for a walk: the same assembler over the
        // same empty capabilities the harness reports. Each stride test also
        // checks every clicked target against it, so a plan that differed
        // would fail the test rather than hide behind it.
        bool planLikeExecutor(ExecContext &ctx, const exec::WwTile &start,
                              const exec::WwGoal &goal, runtime::Plan &outPlan,
                              std::uint32_t disabledMoves = 0)
        {
            runtime::AreaSearch    areaSearch(ctx.reader);
            runtime::TileSearch    tileSearch(ctx.view);
            runtime::PathAssembler assembler(ctx.reader, ctx.view, areaSearch, tileSearch);
            runtime::CapabilitySnapshot none;
            none.disableMoves(disabledMoves, ctx.reader.moveCategories());
            return assembler.assemble(start.x, start.y, start.plane, goal.x, goal.y, goal.plane,
                                      &none, outPlan)
                && !outPlan.steps.empty();
        }

        // Run one walk from `harness` with the executor's seed pinned.
        exec::WwStatus runSeeded(ExecContext &ctx, ExecHarness &ioHarness, const exec::WwGoal &goal,
                                 std::uint32_t seed)
        {
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &ioHarness;
            exec::Executor executor(ctx.reader, ctx.pool, cb, seed);
            return executor.run(goal);
        }

        // Whether every hop of the first plan clicked its own step's target
        // and the hops only ever moved forward.
        bool hopsFollowPlan(const ExecHarness &h, const runtime::Plan &plan)
        {
            std::int32_t prev = -1;
            for (const HopRecord &hop : h.hops)
            {
                if (hop.plan != 0)
                {
                    continue;
                }
                const bool isInPlan = hop.isClicked && hop.stepIndex > prev
                    && static_cast<std::size_t>(hop.stepIndex) < plan.steps.size()
                    && isSameTile(stepTile(plan.steps[static_cast<std::size_t>(hop.stepIndex)]),
                                  hop.target);
                if (!isInPlan)
                {
                    return false;
                }
                prev = hop.stepIndex;
            }
            return true;
        }

        // A long walk-only route: from the centroid of the largest area whose
        // centroid lies in it to its farthest tile within 96, when the plan
        // has at least eight Walk steps and no Transition.
        bool pickLongWalk(ExecContext &ctx, exec::WwTile &outStart, exec::WwGoal &outGoal,
                          runtime::Plan &outPlan)
        {
            const auto nodes = ctx.reader.areaNodes();
            std::size_t best = nodes.size();
            for (std::size_t a = 0; a < nodes.size(); ++a)
            {
                const format::AreaNodeRecord &n = nodes[a];
                const bool isOwnCentroid = ctx.view.areaAt(n.centroidX, n.centroidY,
                    static_cast<std::int32_t>(n.plane)) == static_cast<std::int32_t>(a);
                if (isOwnCentroid && (best == nodes.size() || n.tileCount > nodes[best].tileCount))
                {
                    best = a;
                }
            }
            if (best == nodes.size())
            {
                return false;
            }
            const format::AreaNodeRecord &n = nodes[best];
            const std::int32_t plane = static_cast<std::int32_t>(n.plane);
            const runtime::TilePoint far = farthestInArea(ctx.view, n.centroidX, n.centroidY, plane,
                                                          static_cast<std::int32_t>(best), 96);
            outStart = exec::WwTile{ n.centroidX, n.centroidY, plane };
            outGoal = exec::WwGoal{ far.x, far.y, plane, 0 };
            if (!planLikeExecutor(ctx, outStart, outGoal, outPlan) || outPlan.steps.size() < 8)
            {
                return false;
            }
            return std::all_of(outPlan.steps.begin(), outPlan.steps.end(), [](const runtime::Step &s)
            {
                return s.kind == runtime::StepKind::Walk;
            });
        }

        // Test 4s: on a long walk, no click ever lands beyond the scene cap,
        // every hop clicks a step of the plan and moves forward, and across
        // the seeds some clicks reach past the old fixed 16 tiles.
        std::size_t testStrideCap(ExecContext &ctx)
        {
            exec::WwTile start{};
            exec::WwGoal goal{};
            runtime::Plan plan;
            if (!pickLongWalk(ctx, start, goal, plan))
            {
                std::printf("  exec:   stride-cap test skipped (no long walk-only route)\n");
                return 0;
            }
            std::size_t failures = 0;
            std::int32_t longest = 0;
            std::int32_t shortest = std::numeric_limits<std::int32_t>::max();
            int overCap = 0;
            int hopCount = 0;
            for (std::uint32_t seed = 1; seed <= kStrideSeeds; ++seed)
            {
                ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                                  start.x, start.y, start.plane);
                const exec::WwStatus status = runSeeded(ctx, harness, goal, seed);
                failures += (status == exec::WwStatus::Arrived && hopsFollowPlan(harness, plan)
                             && harness.unexpectedActions == 0) ? 0u : 1u;
                for (const HopRecord &hop : harness.hops)
                {
                    const std::int32_t click = tileChebyshev(hop.from, hop.target);
                    longest = std::max(longest, click);
                    shortest = std::min(shortest, click);
                    overCap += click > kMaxClickChebyshev ? 1 : 0;
                    ++hopCount;
                }
            }
            std::printf("  exec:   stride-cap (%d,%d)->(%d,%d) plan=%zu steps: %d hops over %u seeds,"
                        " click %d..%d tiles (expect max > %d, <= %d) over-cap=%d (expect 0)\n",
                        start.x, start.y, goal.x, goal.y, plan.steps.size(), hopCount, kStrideSeeds,
                        shortest, longest, kOldStepTiles, kMaxClickChebyshev, overCap);
            failures += overCap == 0 ? 0u : 1u;
            failures += longest > kOldStepTiles ? 0u : 1u;
            return failures;
        }

        bool isSameHops(const ExecHarness &a, const ExecHarness &b)
        {
            if (a.hops.size() != b.hops.size() || a.sleepTicksCalls != b.sleepTicksCalls)
            {
                return false;
            }
            for (std::size_t i = 0; i < a.hops.size(); ++i)
            {
                const HopRecord &x = a.hops[i];
                const HopRecord &y = b.hops[i];
                if (x.stepIndex != y.stepIndex || !isSameTile(x.from, y.from)
                    || !isSameTile(x.target, y.target))
                {
                    return false;
                }
            }
            return true;
        }

        // Test 4t: a pinned seed replays the same walk (the same hops and the
        // same idles); another seed walks it differently.
        std::size_t testStrideDeterministic(ExecContext &ctx)
        {
            exec::WwTile start{};
            exec::WwGoal goal{};
            runtime::Plan plan;
            if (!pickLongWalk(ctx, start, goal, plan))
            {
                std::printf("  exec:   stride-seed test skipped (no long walk-only route)\n");
                return 0;
            }
            ExecHarness first = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                            start.x, start.y, start.plane);
            ExecHarness again = first;
            ExecHarness other = first;
            const exec::WwStatus s1 = runSeeded(ctx, first, goal, 7);
            const exec::WwStatus s2 = runSeeded(ctx, again, goal, 7);
            const exec::WwStatus s3 = runSeeded(ctx, other, goal, 8);
            const bool isReplayed = isSameHops(first, again);
            const bool isVaried = !isSameHops(first, other);
            std::printf("  exec:   stride-seed status=%d,%d,%d (expect 0,0,0) hops=%zu,%zu,%zu"
                        " sleeps=%d,%d,%d same-seed-equal=%d (expect 1) other-seed-differs=%d"
                        " (expect 1)\n",
                        static_cast<int>(s1), static_cast<int>(s2), static_cast<int>(s3),
                        first.hops.size(), again.hops.size(), other.hops.size(),
                        first.sleepTicksCalls, again.sleepTicksCalls, other.sleepTicksCalls,
                        isReplayed ? 1 : 0, isVaried ? 1 : 0);
            std::size_t failures = (s1 == exec::WwStatus::Arrived && s2 == exec::WwStatus::Arrived
                                    && s3 == exec::WwStatus::Arrived) ? 0u : 1u;
            failures += isReplayed ? 0u : 1u;
            failures += isVaried ? 0u : 1u;
            return failures;
        }

        // How many Walk steps come straight before step `t` of `plan`.
        std::size_t walksBefore(const runtime::Plan &plan, std::size_t t)
        {
            std::size_t n = 0;
            while (n < t && plan.steps[t - 1 - n].kind == runtime::StepKind::Walk)
            {
                ++n;
            }
            return n;
        }

        // A route that walks at least three steps up to a crossing: from the
        // far side of the first short crossing's area to its destination.
        bool pickWalkToCrossing(ExecContext &ctx, exec::WwTile &outStart, exec::WwGoal &outGoal,
                                runtime::Plan &outPlan)
        {
            CrossAreaPick pick{};
            if (!pickCrossAreaPair(ctx.reader, ctx.view, isShortCrossing, pick))
            {
                return false;
            }
            const std::int32_t area = ctx.view.areaAt(pick.start.x, pick.start.y, pick.startPlane);
            const runtime::TilePoint far =
                farthestInArea(ctx.view, pick.start.x, pick.start.y, pick.startPlane, area, 48);
            outStart = exec::WwTile{ far.x, far.y, pick.startPlane };
            outGoal = exec::WwGoal{ pick.goal.x, pick.goal.y, pick.goalPlane, 0 };
            if (!planLikeExecutor(ctx, outStart, outGoal, outPlan))
            {
                return false;
            }
            for (std::size_t t = 0; t < outPlan.steps.size(); ++t)
            {
                if (outPlan.steps[t].kind == runtime::StepKind::Transition && walksBefore(outPlan, t) >= 3)
                {
                    return true;
                }
            }
            return false;
        }

        // Whether the first plan's announced steps kept every Transition they
        // reached: each one announced straight after the step before it, and
        // no two announced steps with a Transition between them. Counts the
        // hops that passed over steps into ioSkips.
        bool keepsEveryCrossing(const ExecHarness &h, const runtime::Plan &plan, int &ioSkips)
        {
            std::int32_t prev = -1;
            for (const StepRecord &rec : h.steps)
            {
                if (rec.plan != 0)
                {
                    break;
                }
                for (std::int32_t k = prev + 1; k < rec.stepIndex; ++k)
                {
                    if (plan.steps[static_cast<std::size_t>(k)].kind != runtime::StepKind::Walk)
                    {
                        return false;  // passed over a Transition
                    }
                }
                const std::size_t at = static_cast<std::size_t>(rec.stepIndex);
                if (rec.isTransition && rec.stepIndex > 0 && prev != rec.stepIndex - 1)
                {
                    return false;  // its approach walk was not the last thing done
                }
                if (rec.isTransition != (plan.steps[at].kind == runtime::StepKind::Transition))
                {
                    return false;
                }
                ioSkips += rec.stepIndex > prev + 1 ? 1 : 0;
                prev = rec.stepIndex;
            }
            return prev >= 0;
        }

        // Test 4u: striding never passes a Transition. Walking a few steps up
        // to a crossing, the walk clicked last before it is always the step
        // straight before it, whatever the seed, while hops elsewhere still
        // pass over steps.
        std::size_t testStrideStopsAtCrossing(ExecContext &ctx)
        {
            exec::WwTile start{};
            exec::WwGoal goal{};
            runtime::Plan plan;
            if (!pickWalkToCrossing(ctx, start, goal, plan))
            {
                std::printf("  exec:   stride-crossing test skipped (no long walk to a crossing)\n");
                return 0;
            }
            std::size_t failures = 0;
            int skips = 0;
            int broken = 0;
            for (std::uint32_t seed = 1; seed <= kStrideSeeds; ++seed)
            {
                ExecHarness harness = makeHarness(ExecHarnessMode::SimulateTransition,
                                                  start.x, start.y, start.plane);
                harness.landingRecords = ctx.reader.transitions();
                const exec::WwStatus status = runSeeded(ctx, harness, goal, seed);
                const bool isKept = status == exec::WwStatus::Arrived
                                 && harness.replanStartedEvents == 0
                                 && hopsFollowPlan(harness, plan)
                                 && keepsEveryCrossing(harness, plan, skips)
                                 && harness.unexpectedActions == 0;
                broken += isKept ? 0 : 1;
            }
            std::printf("  exec:   stride-crossing (%d,%d)->(%d,%d) plan=%zu steps: broken=%d of %u"
                        " seeds (expect 0) skipping-hops=%d (expect > 0)\n",
                        start.x, start.y, goal.x, goal.y, plan.steps.size(), broken, kStrideSeeds,
                        skips);
            failures += broken == 0 ? 0u : 1u;
            failures += skips > 0 ? 0u : 1u;
            return failures;
        }

        bool isNearWilderness(const format::ArtifactReader &reader, const exec::WwTile &at)
        {
            for (const format::WildernessRegion &w : reader.wildernessRegions())
            {
                const std::int32_t dx = std::max({ w.minX - at.x, at.x - w.maxX, 0 });
                const std::int32_t dy = std::max({ w.minY - at.y, at.y - w.maxY, 0 });
                const bool isOnPlane = at.plane >= w.planeMin && at.plane <= w.planeMax;
                if (isOnPlane && std::max(dx, dy) <= kWildernessMargin)
                {
                    return true;
                }
            }
            return false;
        }

        // Test 4v: along the Wilderness ditch north of Edgeville, a hop never
        // passes over a step near the fence: there each step is clicked in
        // turn, since the game's own pathfinder would route a far click
        // through the fenced tiles. A fixed fixture on purpose (the ditch is
        // what is under test); skipped when the artifact does not bake it.
        std::size_t testStrideWildernessFence(ExecContext &ctx)
        {
            const exec::WwTile start{ 3094, 3491, 0 };
            const exec::WwGoal goal{ 3130, 3516, 0, 0 };
            runtime::Plan plan;
            std::size_t nearSteps = 0;
            if (ctx.reader.wildernessRegions().empty() || !planLikeExecutor(ctx, start, goal, plan))
            {
                std::printf("  exec:   stride-wilderness test skipped (no Edgeville ditch walk)\n");
                return 0;
            }
            for (const runtime::Step &step : plan.steps)
            {
                nearSteps += isNearWilderness(ctx.reader, stepTile(step)) ? 1u : 0u;
            }
            if (nearSteps == 0)
            {
                std::printf("  exec:   stride-wilderness test skipped (route keeps clear of the"
                            " fence)\n");
                return 0;
            }
            std::size_t failures = 0;
            int crossedNear = 0;
            int nearHops = 0;
            for (std::uint32_t seed = 1; seed <= kStrideSeeds; ++seed)
            {
                ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                                  start.x, start.y, start.plane);
                const exec::WwStatus status = runSeeded(ctx, harness, goal, seed);
                failures += (status == exec::WwStatus::Arrived && hopsFollowPlan(harness, plan))
                                ? 0u : 1u;
                std::int32_t prev = -1;
                for (const HopRecord &hop : harness.hops)
                {
                    bool isSpanNear = false;
                    for (std::int32_t k = prev + 1; k <= hop.stepIndex; ++k)
                    {
                        isSpanNear = isSpanNear || isNearWilderness(ctx.reader,
                            stepTile(plan.steps[static_cast<std::size_t>(k)]));
                    }
                    nearHops += isSpanNear ? 1 : 0;
                    crossedNear += (isSpanNear && hop.stepIndex > prev + 1) ? 1 : 0;
                    prev = hop.stepIndex;
                }
            }
            std::printf("  exec:   stride-wilderness plan=%zu steps (%zu near the fence):"
                        " near-fence hops=%d multi-step-near=%d (expect 0)\n",
                        plan.steps.size(), nearSteps, nearHops, crossedNear);
            failures += crossedNear == 0 ? 0u : 1u;
            failures += nearHops > 0 ? 0u : 1u;
            return failures;
        }

        // Test 4w: a long hop winds away from its target for several polls
        // (round a building) before it gets there. The player moves on every
        // poll, so it is not a stall: no re-click, no Stuck. The old test,
        // distance to the target not falling, re-clicked at the third poll
        // and gave up at the sixth.
        std::size_t testWindingHopNoStall(ExecContext &ctx)
        {
            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                              ctx.node.centroidX, ctx.node.centroidY, ctx.plane);
            harness.wanderPollsLeft = 8;
            const exec::WwGoal goal{ ctx.farthest.x, ctx.farthest.y, ctx.plane, 0 };
            const exec::WwStatus status = runSeeded(ctx, harness, goal, 3);

            const int reclicks = harness.walkToCalls - static_cast<int>(harness.hops.size());
            std::printf("  exec:   winding-hop status=%d (expect 0) stucks=%d replans=%d re-clicks=%d"
                        " (expect 0, 0, 0)\n",
                        static_cast<int>(status), harness.stuckEvents, harness.replanStartedEvents,
                        reclicks);
            std::size_t failures = status == exec::WwStatus::Arrived ? 0u : 1u;
            failures += (harness.stuckEvents == 0 && harness.replanStartedEvents == 0
                         && reclicks == 0) ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Test 4x: the stuck deadline grows with the hop. The arithmetic
        // against fixed values, then the live clock: a player who paces
        // forever (moving, so never a stall) is called Stuck no sooner than
        // the deadline of the hop's length and not long after. Takes that
        // deadline (about 9 s) of wall-clock time.
        std::size_t testStuckDeadline(ExecContext &ctx)
        {
            const bool isScaled = exec::Executor::stuckDeadlineMs(0) == 8000
                               && exec::Executor::stuckDeadlineMs(1) == 8900
                               && exec::Executor::stuckDeadlineMs(35) == 39500;
            const runtime::TilePoint near = farthestInArea(ctx.view, ctx.node.centroidX,
                                                           ctx.node.centroidY, ctx.plane,
                                                           ctx.view.areaAt(ctx.node.centroidX,
                                                                           ctx.node.centroidY,
                                                                           ctx.plane),
                                                           2);
            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                              ctx.node.centroidX, ctx.node.centroidY, ctx.plane);
            harness.isWanderingForever = true;
            const exec::WwStatus status =
                runSeeded(ctx, harness, exec::WwGoal{ near.x, near.y, ctx.plane, 0 }, 1);

            const std::int32_t hopTiles = harness.hops.empty()
                ? 0 : tileChebyshev(harness.hops[0].from, harness.hops[0].target);
            const std::int64_t wantMs = exec::Executor::stuckDeadlineMs(hopTiles);
            constexpr std::int64_t kLateSlackMs = 2000;
            const bool isOnTime = harness.wanderStuckMs >= wantMs
                               && harness.wanderStuckMs <= wantMs + kLateSlackMs;
            std::printf("  exec:   stuck-deadline table=%d (expect 1) hop=%d tiles stuck after %lld ms"
                        " (expect %lld..%lld) stucks=%d (expect 1) status=%d (expect 0)\n",
                        isScaled ? 1 : 0, hopTiles, static_cast<long long>(harness.wanderStuckMs),
                        static_cast<long long>(wantMs),
                        static_cast<long long>(wantMs + kLateSlackMs), harness.stuckEvents,
                        static_cast<int>(status));
            std::size_t failures = isScaled ? 0u : 1u;
            failures += (!harness.hops.empty() && isOnTime && harness.stuckEvents == 1) ? 0u : 1u;
            failures += status == exec::WwStatus::Arrived ? 0u : 1u;
            failures += harness.unexpectedActions == 0 ? 0u : 1u;
            return failures;
        }

        // Interaction hints (test 4y). The value the WwEvent contract promises
        // for a walk clicked at plan step i, worked out from the plan alone:
        // 1 + the Chebyshev path tiles from its target through the following
        // Walk steps up to the next Transition or the plan's end, capped.
        constexpr std::int32_t kMaxInteractionHint = 1001;

        std::int32_t promisedHint(const runtime::Plan &plan, std::size_t i)
        {
            std::int64_t tiles = 0;
            for (std::size_t k = i + 1;
                 k < plan.steps.size() && plan.steps[k].kind == runtime::StepKind::Walk; ++k)
            {
                tiles += tileChebyshev(stepTile(plan.steps[k - 1]), stepTile(plan.steps[k]));
            }
            return static_cast<std::int32_t>(std::min<std::int64_t>(tiles + 1, kMaxInteractionHint));
        }

        // What one run's first plan got wrong about hints: walk hops whose
        // hint is not the promised one (or 0), walk hops whose hint did not
        // fall from the hop before them on the way to the same interaction,
        // and Transition announcements carrying anything but 0. Also counts
        // the hops that ended on the step right before an interaction and
        // said so with a 1.
        struct HintTally
        {
            int wrong{0};
            int notFalling{0};
            int transitionNonZero{0};
            int approachOnes{0};
        };

        void tallyHints(const ExecHarness &h, const runtime::Plan &plan, HintTally &io)
        {
            std::int32_t lastHint = 0;  // 0: no walk hop since the last Transition
            for (const StepRecord &rec : h.steps)
            {
                if (rec.plan != 0)
                {
                    break;
                }
                if (rec.isTransition)
                {
                    io.transitionNonZero += rec.hint != 0 ? 1 : 0;
                    lastHint = 0;
                    continue;
                }
                const std::size_t at = static_cast<std::size_t>(rec.stepIndex);
                const bool isBeforeInteraction = at + 1 == plan.steps.size()
                    || plan.steps[at + 1].kind != runtime::StepKind::Walk;
                io.wrong += (rec.hint <= 0 || rec.hint != promisedHint(plan, at)) ? 1 : 0;
                io.notFalling += (lastHint != 0 && rec.hint >= lastHint) ? 1 : 0;
                io.approachOnes += (isBeforeInteraction && rec.hint == 1) ? 1 : 0;
                lastHint = rec.hint;
            }
        }

        // Test 4y: every walk hop's StepAdvanced carries the interaction hint
        // and comes before its walkTo (the harness fills each hop's target
        // from the walkTo straight after it, so a click made before its event
        // lands on the wrong hop and fails hopsFollowPlan). On a walk up to a
        // crossing the hint counts down to 1 on the crossing's approach step,
        // and the crossing's own StepAdvanced carries 0; on a long walk-only
        // route it counts down to 1 on the step at the goal.
        std::size_t testInteractionHint(ExecContext &ctx)
        {
            struct Fixture
            {
                const char       *label;
                bool              isFound;
                exec::WwTile      start;
                exec::WwGoal      goal;
                runtime::Plan     plan;
                ExecHarnessMode   mode;
            };
            Fixture fixtures[2]{};
            fixtures[0].label = "crossing";
            fixtures[0].mode = ExecHarnessMode::SimulateTransition;
            fixtures[0].isFound = pickWalkToCrossing(ctx, fixtures[0].start, fixtures[0].goal,
                                                     fixtures[0].plan);
            fixtures[1].label = "goal";
            fixtures[1].mode = ExecHarnessMode::SimulateInstantWalk;
            fixtures[1].isFound = pickLongWalk(ctx, fixtures[1].start, fixtures[1].goal,
                                               fixtures[1].plan);
            std::size_t failures = 0;
            for (Fixture &f : fixtures)
            {
                if (!f.isFound)
                {
                    std::printf("  exec:   hint %s test skipped (no such route)\n", f.label);
                    continue;
                }
                HintTally tally{};
                int unordered = 0;
                for (std::uint32_t seed = 1; seed <= kStrideSeeds; ++seed)
                {
                    ExecHarness harness = makeHarness(f.mode, f.start.x, f.start.y, f.start.plane);
                    harness.landingRecords = ctx.reader.transitions();
                    const exec::WwStatus status = runSeeded(ctx, harness, f.goal, seed);
                    const bool isOrdered = status == exec::WwStatus::Arrived
                                        && harness.replanStartedEvents == 0
                                        && hopsFollowPlan(harness, f.plan);
                    unordered += isOrdered ? 0 : 1;
                    tallyHints(harness, f.plan, tally);
                }
                std::printf("  exec:   hint %s plan=%zu steps over %u seeds: unordered=%d wrong=%d"
                            " not-falling=%d transition-nonzero=%d (expect 0,0,0,0)"
                            " approach-ones=%d (expect %u)\n",
                            f.label, f.plan.steps.size(), kStrideSeeds, unordered, tally.wrong,
                            tally.notFalling, tally.transitionNonZero, tally.approachOnes,
                            kStrideSeeds);
                failures += (unordered == 0 && tally.wrong == 0 && tally.notFalling == 0
                             && tally.transitionNonZero == 0) ? 0u : 1u;
                failures += tally.approachOnes >= static_cast<int>(kStrideSeeds) ? 0u : 1u;
            }
            return failures;
        }

        // Transitions of movement category `category` the run announced, in
        // any of its plans.
        int crossingsIn(const ExecHarness &h, const format::ArtifactReader &reader,
                        std::uint8_t category)
        {
            const auto categories = reader.moveCategories();
            int n = 0;
            for (const StepRecord &rec : h.steps)
            {
                const bool isInCategory = rec.isTransition && rec.transitionIndex >= 0
                    && static_cast<std::size_t>(rec.transitionIndex) < categories.size()
                    && categories[static_cast<std::size_t>(rec.transitionIndex)] == category;
                n += isInCategory ? 1 : 0;
            }
            return n;
        }

        // A door the executor's plan crosses from beside it, and which it can
        // also walk round when doors are disabled: the masked plan exists and
        // starts with a walk (so a dropped click can stall it). Searches the
        // first kMaxDetourTries door edges.
        constexpr std::size_t kMaxDetourTries = 5000;

        bool pickDetouredDoor(ExecContext &ctx, exec::WwTile &outStart, exec::WwGoal &outGoal)
        {
            constexpr auto kDoors = static_cast<std::uint8_t>(format::MoveCategory::Doors);
            const auto edges = ctx.reader.areaEdges();
            const auto txs = ctx.reader.transitions();
            std::size_t tries = 0;
            for (std::size_t i = 0; i < edges.size() && tries < kMaxDetourTries; ++i)
            {
                const std::uint32_t t = edges[i].transitionIndex;
                if (t >= txs.size() || ctx.reader.moveCategories()[t] != kDoors
                    || !isShortCrossing(txs[t]))
                {
                    continue;
                }
                ++tries;
                const format::TransitionRecord &tx = txs[t];
                const std::int32_t plane = static_cast<std::int32_t>(tx.originPlane);
                const auto isInFromArea = [&](std::int32_t x, std::int32_t y)
                {
                    return ctx.view.isStandable(x, y, plane)
                        && ctx.view.areaAt(x, y, plane) == edges[i].fromArea;
                };
                std::int32_t x = 0;
                std::int32_t y = 0;
                if (!runtime::findNearestTile(tx.originX, tx.originY,
                                              data::kTransitionApproachRadius, true, isInFromArea,
                                              tx.originX, tx.originY, x, y))
                {
                    continue;
                }
                const exec::WwTile start{ x, y, plane };
                const exec::WwGoal goal{ tx.destX, tx.destY, static_cast<std::int32_t>(tx.destPlane),
                                         0 };
                runtime::Plan direct;
                runtime::Plan detour;
                const bool isUsable = planLikeExecutor(ctx, start, goal, direct)
                    && planLikeExecutor(ctx, start, goal, detour, 1u << kDoors)
                    && detour.steps.front().kind == runtime::StepKind::Walk
                    && std::any_of(direct.steps.begin(), direct.steps.end(),
                                   [&](const runtime::Step &s)
                                   {
                                       return s.kind == runtime::StepKind::Transition
                                           && ctx.reader.moveCategories()[s.transitionIndex]
                                                  == kDoors;
                                   });
                if (isUsable)
                {
                    outStart = start;
                    outGoal = goal;
                    return true;
                }
            }
            return false;
        }

        // Test 4z: the executor's disabledMoves keeps every plan of the run,
        // the first and each re-plan, off the category. Beside a door that
        // can also be walked round: with the mask off the run goes through
        // the door; with doors disabled and the first two walk clicks dropped
        // (a stall, so a Stuck and a re-plan) it walks round, and no door is
        // announced in any plan.
        std::size_t testDisabledMoves(ExecContext &ctx)
        {
            exec::WwTile start{};
            exec::WwGoal goal{};
            if (!pickDetouredDoor(ctx, start, goal))
            {
                std::printf("  exec:   disabled-moves test skipped (no door with a way round)\n");
                return 0;
            }
            const auto category = static_cast<std::uint8_t>(format::MoveCategory::Doors);
            ExecHarness open = makeHarness(ExecHarnessMode::SimulateTransition,
                                           start.x, start.y, start.plane);
            std::printf("  exec:   disabled-moves door walk (%d,%d,p%d)->(%d,%d,p%d)\n", start.x,
                        start.y, start.plane, goal.x, goal.y, goal.plane);
            open.landingRecords = ctx.reader.transitions();
            ExecHarness shut = open;
            shut.droppedWalkClicks = 2;
            const exec::WwStatus openStatus = runSeeded(ctx, open, goal, 1);
            exec::Callbacks cb = kCallbackPrototype;
            cb.user = &shut;
            exec::Executor executor(ctx.reader, ctx.pool, cb, 1u, 1u << category);
            const exec::WwStatus shutStatus = executor.run(goal);
            const int openUses = crossingsIn(open, ctx.reader, category);
            const int shutUses = crossingsIn(shut, ctx.reader, category);
            std::printf("  exec:   disabled-moves category=%s off: status=%d uses=%d (expect 0, >= 1)"
                        " on: status=%d uses=%d (expect 0, 0) stucks=%d replans=%d (expect >= 1)\n",
                        format::moveCategoryName(static_cast<format::MoveCategory>(category)),
                        static_cast<int>(openStatus), openUses, static_cast<int>(shutStatus),
                        shutUses, shut.stuckEvents, shut.replanStartedEvents);
            std::size_t failures = (openStatus == exec::WwStatus::Arrived && openUses >= 1) ? 0u : 1u;
            failures += (shutStatus == exec::WwStatus::Arrived && shutUses == 0
                         && shut.replanStartedEvents >= 1) ? 0u : 1u;
            failures += (open.unexpectedActions == 0 && shut.unexpectedActions == 0) ? 0u : 1u;
            return failures;
        }

        // Test 4aa: the live hang of 2026-09-30. FortWithUs walked from the
        // Varrock lodestone to Fort Forinthry's hub; after the hop clicked at
        // (3292,3474) the run went quiet for minutes with the player standing on
        // that very tile. Replay the leg the live re-plan walked (walk-only; the
        // Varrock east transport was not taken) across the stride seeds, the
        // player landing exactly on each clicked tile as it did live. Every run
        // must arrive with no Stuck and no re-plan, the hops must follow the
        // plan, and a hop that lands on (3292,3474) must never be the last one:
        // the executor itself always clicks on from there.
        std::size_t testFortWalkReplay(ExecContext &ctx)
        {
            const exec::WwTile start{ 3222, 3377, 0 };
            const exec::WwTile frozeAt{ 3292, 3474, 0 };
            const exec::WwGoal goal{ 3286, 3555, 0, 1 };
            const std::uint32_t noTransports =
                1u << static_cast<std::uint8_t>(format::MoveCategory::Transports);
            runtime::Plan plan;
            const bool isPlanned = ctx.view.areaAt(start.x, start.y, start.plane) >= 0
                && planLikeExecutor(ctx, start, goal, plan, noTransports);
            const bool isOnRoute = isPlanned
                && std::any_of(plan.steps.begin(), plan.steps.end(), [&](const runtime::Step &s)
                {
                    return s.kind == runtime::StepKind::Walk && isSameTile(stepTile(s), frozeAt);
                });
            if (!isOnRoute)
            {
                std::printf("  exec:   fort-walk replay skipped (route does not pass (3292,3474))\n");
                return 0;
            }
            std::size_t failures = 0;
            int arrived = 0;
            int landedThere = 0;
            int stoppedThere = 0;
            int stucks = 0;
            for (std::uint32_t seed = 1; seed <= kStrideSeeds; ++seed)
            {
                ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                                  start.x, start.y, start.plane);
                exec::Callbacks cb = kCallbackPrototype;
                cb.user = &harness;
                exec::Executor executor(ctx.reader, ctx.pool, cb, seed, noTransports);
                const exec::WwStatus status = executor.run(goal);
                arrived += status == exec::WwStatus::Arrived ? 1 : 0;
                stucks += harness.stuckEvents + harness.replanStartedEvents;
                for (std::size_t k = 0; k < harness.hops.size(); ++k)
                {
                    if (!isSameTile(harness.hops[k].target, frozeAt))
                    {
                        continue;
                    }
                    ++landedThere;
                    stoppedThere += k + 1 == harness.hops.size() ? 1 : 0;
                }
                failures += (hopsFollowPlan(harness, plan) && harness.unexpectedActions == 0)
                    ? 0u : 1u;
            }
            std::printf("  exec:   fort-walk replay (%d,%d)->(%d,%d) plan=%zu steps: arrived=%d/%u"
                        " stucks+replans=%d (expect 0) hops onto (3292,3474)=%d, last hop there=%d"
                        " (expect 0)\n",
                        start.x, start.y, goal.x, goal.y, plan.steps.size(), arrived, kStrideSeeds,
                        stucks, landedThere, stoppedThere);
            failures += arrived == static_cast<int>(kStrideSeeds) ? 0u : 1u;
            failures += (stucks == 0 && stoppedThere == 0) ? 0u : 1u;
            return failures;
        }

        // Lead clicks (test 4ab). The live walks of 2026-09-30 clicked each
        // next waypoint only once the player stood on the last, so the avatar
        // stopped at every click. The paced harness walks the player there a
        // tick at a time, and every Walk-to-Walk handoff is judged by where
        // the player stood when the next hop was announced. The limits mirror
        // the executor's own lead range and full-stop share (5%, with room for
        // 25 seeds' worth of chance); a test that re-derived them from the
        // executor would pass whatever the executor did.
        constexpr std::int32_t kLeadMaxTiles       = 5;
        constexpr int          kMaxFullStopPercent = 15;
        constexpr int          kRunTilesPerTick    = 2;
        constexpr int          kWalkTilesPerTick   = 1;
        constexpr std::int32_t kApproachChebyshev  = 1;

        // What the paced walks showed at their Walk-to-Walk handoffs: clicked
        // on with the player still moving and 2..kLeadMaxTiles short of the
        // last click (leading), on or next to it (stopped: a full stop), still
        // moving but farther out (early), or 2+ short and not moving (late).
        // Plus walk clicks beyond one per hop, and hops that clicked the tile
        // the hop before them clicked.
        struct LeadTally
        {
            int handoffs;
            int leading;
            int stopped;
            int early;
            int late;
            int reclicks;
            int repeats;
            std::int32_t nearest;
            std::int32_t farthest;
        };

        // Whether a Transition of `plan` lies between its steps `from` and
        // `to` (exclusive): the hop after it starts from the far side.
        bool isCrossingBetween(const runtime::Plan &plan, std::int32_t from, std::int32_t to)
        {
            const auto size = static_cast<std::int32_t>(plan.steps.size());
            for (std::int32_t k = from + 1; k < to && k < size; ++k)
            {
                if (plan.steps[static_cast<std::size_t>(k)].kind != runtime::StepKind::Walk)
                {
                    return true;
                }
            }
            return false;
        }

        // Tallies the first plan's hops (hopsFollowPlan vouches that their
        // step indices are that plan's).
        void tallyLeads(const ExecHarness &h, const runtime::Plan &plan, LeadTally &io)
        {
            io.reclicks += h.walkToCalls - static_cast<int>(h.hops.size());
            for (std::size_t k = 1; k < h.hops.size(); ++k)
            {
                const HopRecord &prev = h.hops[k - 1];
                const HopRecord &hop = h.hops[k];
                if (prev.plan != 0 || hop.plan != 0
                    || isCrossingBetween(plan, prev.stepIndex, hop.stepIndex))
                {
                    continue;
                }
                ++io.handoffs;
                io.repeats += isSameTile(prev.target, hop.target) ? 1 : 0;
                const std::int32_t left = tileChebyshev(hop.from, prev.target);
                if (left <= kApproachChebyshev)
                {
                    ++io.stopped;
                    continue;
                }
                io.nearest = std::min(io.nearest, left);
                io.farthest = std::max(io.farthest, left);
                if (!hop.isMoving)
                {
                    ++io.late;
                }
                else if (left > kLeadMaxTiles)
                {
                    ++io.early;
                }
                else
                {
                    ++io.leading;
                }
            }
        }

        // Test 4ab (open ground): on a long walk-only route, walked (1 tile a
        // tick) and run (2), every seed arrives exactly on the goal with the
        // player at rest and the hops following the plan; each handoff clicks
        // on while the player is still moving, 2..kLeadMaxTiles short of the
        // last click, bar the occasional full stop; no click is repeated.
        std::size_t testLeadClicks(ExecContext &ctx, int tilesPerTick, const char *pace)
        {
            exec::WwTile start{};
            exec::WwGoal goal{};
            runtime::Plan plan;
            if (!pickLongWalk(ctx, start, goal, plan))
            {
                std::printf("  exec:   lead-click test skipped (no long walk-only route)\n");
                return 0;
            }
            LeadTally tally{ 0, 0, 0, 0, 0, 0, 0, std::numeric_limits<std::int32_t>::max(), 0 };
            int broken = 0;
            int ticks = 0;
            const exec::WwTile goalTile{ goal.x, goal.y, goal.plane };
            for (std::uint32_t seed = 1; seed <= kStrideSeeds; ++seed)
            {
                ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                                  start.x, start.y, start.plane);
                harness.tilesPerTick = tilesPerTick;
                const exec::WwStatus status = runSeeded(ctx, harness, goal, seed);
                const bool isWhole = status == exec::WwStatus::Arrived
                                  && isSameTile(harness.position, goalTile)
                                  && !harness.hasWalkGoal && harness.stuckEvents == 0
                                  && harness.replanStartedEvents == 0
                                  && hopsFollowPlan(harness, plan)
                                  && harness.unexpectedActions == 0;
                broken += isWhole ? 0 : 1;
                ticks += harness.ticksSlept;
                tallyLeads(harness, plan, tally);
            }
            std::printf("  exec:   lead-click %s (%d,%d)->(%d,%d) plan=%zu steps, %u seeds: broken=%d"
                        " (expect 0) handoffs=%d leading=%d stopped=%d (expect <= %d%%) early=%d"
                        " late=%d re-clicks=%d repeats=%d (expect 0,0,0,0) lead %d..%d tiles"
                        " ticks=%d\n",
                        pace, start.x, start.y, goal.x, goal.y, plan.steps.size(), kStrideSeeds,
                        broken, tally.handoffs, tally.leading, tally.stopped, kMaxFullStopPercent,
                        tally.early, tally.late, tally.reclicks, tally.repeats,
                        tally.leading > 0 ? tally.nearest : 0, tally.farthest, ticks);
            std::size_t failures = broken == 0 ? 0u : 1u;
            failures += tally.leading > 0 ? 0u : 1u;
            failures += tally.stopped * 100 <= kMaxFullStopPercent * tally.handoffs ? 0u : 1u;
            failures += (tally.early == 0 && tally.late == 0) ? 0u : 1u;
            failures += (tally.reclicks == 0 && tally.repeats == 0) ? 0u : 1u;
            return failures;
        }

        // Whether every Transition the first plan announced found the player
        // within kApproachChebyshev of the walk clicked straight before it.
        bool isEveryCrossingReached(const ExecHarness &h, int &ioCrossings)
        {
            std::size_t hop = 0;
            bool hasHop = false;
            exec::WwTile approach{};
            for (const StepRecord &rec : h.steps)
            {
                if (rec.plan != 0)
                {
                    break;
                }
                if (!rec.isTransition)
                {
                    approach = h.hops[hop].target;
                    hasHop = true;
                    ++hop;
                    continue;
                }
                ++ioCrossings;
                if (!hasHop || tileChebyshev(rec.at, approach) > kApproachChebyshev)
                {
                    return false;
                }
            }
            return true;
        }

        // Test 4ab (crossing): walked and run up to a crossing, the walk
        // before it still arrives on its approach tile before the loc is
        // clicked, on every seed, while the hops before that lead; and the
        // run still ends exactly on the goal.
        std::size_t testLeadStopsAtCrossing(ExecContext &ctx, int tilesPerTick, const char *pace)
        {
            exec::WwTile start{};
            exec::WwGoal goal{};
            runtime::Plan plan;
            if (!pickWalkToCrossing(ctx, start, goal, plan))
            {
                std::printf("  exec:   lead-crossing test skipped (no long walk to a crossing)\n");
                return 0;
            }
            int broken = 0;
            int crossings = 0;
            int skips = 0;
            LeadTally tally{ 0, 0, 0, 0, 0, 0, 0, std::numeric_limits<std::int32_t>::max(), 0 };
            const exec::WwTile goalTile{ goal.x, goal.y, goal.plane };
            for (std::uint32_t seed = 1; seed <= kStrideSeeds; ++seed)
            {
                ExecHarness harness = makeHarness(ExecHarnessMode::SimulateTransition,
                                                  start.x, start.y, start.plane);
                harness.landingRecords = ctx.reader.transitions();
                harness.tilesPerTick = tilesPerTick;
                const exec::WwStatus status = runSeeded(ctx, harness, goal, seed);
                const bool isKept = status == exec::WwStatus::Arrived
                                 && isSameTile(harness.position, goalTile)
                                 && harness.replanStartedEvents == 0
                                 && hopsFollowPlan(harness, plan)
                                 && keepsEveryCrossing(harness, plan, skips)
                                 && isEveryCrossingReached(harness, crossings)
                                 && harness.unexpectedActions == 0;
                broken += isKept ? 0 : 1;
                tallyLeads(harness, plan, tally);
            }
            std::printf("  exec:   lead-crossing %s (%d,%d)->(%d,%d) plan=%zu steps: broken=%d of %u"
                        " (expect 0) crossings reached=%d (expect > 0) walk handoffs=%d leading=%d"
                        " early=%d late=%d (expect 0,0)\n",
                        pace, start.x, start.y, goal.x, goal.y, plan.steps.size(), broken,
                        kStrideSeeds, crossings, tally.handoffs, tally.leading, tally.early,
                        tally.late);
            std::size_t failures = broken == 0 ? 0u : 1u;
            failures += crossings > 0 ? 0u : 1u;
            failures += (tally.early == 0 && tally.late == 0) ? 0u : 1u;
            return failures;
        }

        // Test 4c: in combat at the first plan, out of it after the first hop.
        // The walk-only goal needs no teleport, but the flip back out of combat
        // must still re-plan once, since that is the moment teleports come
        // back into consideration (V1 interrupted its walk the same way). The
        // walk is the long one, past the longest stride: a walk one hop can
        // cover arrives before the flip is ever looked at.
        std::size_t testCombatEndReplan(ExecContext &ctx)
        {
            exec::WwTile start{};
            exec::WwGoal goal{};
            runtime::Plan plan;
            if (!pickLongWalk(ctx, start, goal, plan))
            {
                std::printf("  exec:   combat test skipped (no long walk-only route)\n");
                return 0;
            }
            if (!runtime::isTeleportAllowed(ctx.reader, start.x, start.y, start.plane))
            {
                std::printf("  exec:   combat test skipped (start tile is not teleport-allowed)\n");
                return 0;
            }

            ExecHarness harness = makeHarness(ExecHarnessMode::SimulateInstantWalk,
                                              start.x, start.y, start.plane);
            harness.inCombatReadsLeft = 1;
            const exec::WwStatus status = runSeeded(ctx, harness, goal, 1);

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
        failures += testOffCourseWithSpare(ctx);
        failures += testOffCourseSoleEntry(ctx);
        failures += testRefusedCrossingWithSpare(ctx);
        failures += testRefusedSoleCrossing(ctx);
        failures += testDeadClickOnOpenCrossing(ctx);
        failures += testAbsentCrossingWithSpare(ctx);
        failures += testAbsentSoleCrossing(ctx);
        failures += testBlockedGoal(ctx);
        failures += testPocketGoal(ctx);
        failures += testCrossingLands(ctx, isShortCrossing, "short-crossing");
        failures += testCrossingLands(ctx, isFarLandingNonDoor, "far-crossing");
        failures += testNpcOrigin(ctx, false);
        failures += testNpcOrigin(ctx, true);
        failures += testWalkThroughDoor(ctx, 0, false);
        failures += testWalkThroughDoor(ctx, 1, false);
        failures += testWalkThroughDoor(ctx, 0, true);
        failures += testChatDuringWalk(ctx);
        failures += testChatNeverCloses(ctx);
        failures += testZoneAnswer(ctx);
        failures += testNoZoneNeverAnswers(ctx);
        failures += testStrideCap(ctx);
        failures += testStrideDeterministic(ctx);
        failures += testStrideStopsAtCrossing(ctx);
        failures += testStrideWildernessFence(ctx);
        failures += testWindingHopNoStall(ctx);
        failures += testStuckDeadline(ctx);
        failures += testInteractionHint(ctx);
        failures += testDisabledMoves(ctx);
        failures += testFortWalkReplay(ctx);
        failures += testLeadClicks(ctx, kRunTilesPerTick, "run");
        failures += testLeadClicks(ctx, kWalkTilesPerTick, "walk");
        failures += testLeadStopsAtCrossing(ctx, kRunTilesPerTick, "run");
        failures += testLeadStopsAtCrossing(ctx, kWalkTilesPerTick, "walk");
        failures += testFfi(ctx, artifactPath);
        return failures;
    }
}
