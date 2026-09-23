#include "exec/Executor.h"

#include "data/Transitions.h"
#include "format/Artifact.h"
#include "runtime/SearchContext.h"
#include "runtime/TeleportPolicy.h"
#include "runtime/TransitionShape.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

        // Transition-step tunables. Interface-open polling lets the executor
        // wait for an interact-opened dialog before clicking inside it; the
        // budget is wall-clock-cheap because each poll is one isInterfaceOpen
        // call plus a short sleep. The settle wait absorbs the engine tick
        // between the chain's final action and the position committing at the
        // destination, so the next walkOneStep reads a stable position.
        constexpr int32_t kInterfaceOpenPollTicks = 2;   // ~1.2s between isInterfaceOpen polls
        constexpr int32_t kInterfaceOpenMaxPolls  = 10;  // ~12s budget per Click step
        constexpr int32_t kPostChainSettleTicks   = 2;   // ~1.2s wait for the engine to commit dest

        // Host action id for an interface-component interaction (ActionTypes.
        // COMPONENT on the Java side). A chain action with this id carries its
        // target interface as param3>>16, which the chain loop gates on.
        constexpr int32_t kComponentActionId = 57;

        // Re-plan budget. Each walk-stuck recovery and each teleport-allowed
        // flip consumes one re-plan; the cap stops a pathological loop (e.g.,
        // a planner that keeps proposing the same unreachable step) from
        // running forever. Three is enough for the realistic worst cases (one
        // stuck recovery + one wilderness-exit teleport re-plan + a margin)
        // without inviting tail-latency surprises.
        constexpr int32_t kMaxReplans = 3;

        // Interact attempts on a loc the host reports absent before a
        // non-door transition fails. A loc can drop out of the scene snapshot
        // for one sample, so one miss is not proof; three, a tick apart, is.
        constexpr int32_t kMissingLocAttempts = 3;

        // Landing after an issued local transition. An agility obstacle moves
        // the player for several ticks after the click, and the game drops a
        // walk click issued during that forced move, so the executor waits for
        // the player to land: near the destination, or standing still for
        // kLandingStillPolls ticks somewhere else (a failed jump drops you in a
        // pit), within kLandingMaxTicks.
        constexpr int32_t kLandingMaxTicks   = 8;   // ~4.8s past the fixed settle
        constexpr int32_t kLandingStillPolls = 3;

        // How far from a transition's destination the player may end up and
        // still count as having crossed it. A skipped open door leaves the
        // player on the near side, at most a same-floor hop plus the stand
        // tile away; anything farther, or on another plane, is off course.
        constexpr int32_t kLandingSlack = runtime::kMaxSameFloorHop + 1;

        // Re-plans spent on a transition that landed off course (a failed
        // agility obstacle, a refused teleport). A budget of their own, so
        // falling into a pit does not spend the stuck-recovery budget, and
        // bounded, so a transition that never lands cannot loop forever.
        constexpr int32_t kMaxReroutes = 3;

        // Plain chat pages a transition can raise (Draynor Manor's front door
        // says its piece the first time): interface and its continue button.
        // Only pages that take a continue; the option list (1188 CHOICE_V2) is
        // deliberately absent, since the walker has no business choosing.
        struct ChatPage
        {
            int32_t interfaceId;
            int32_t continueComponent;
        };
        constexpr ChatPage kChatPages[] = {
            { 1184, 15 },  // CHAT_V2_LEFT, npc chat
            { 1191, 15 },  // CHAT_V2_RIGHT, player chat
            { 1187, 20 },  // CHAT_V2_PAIR
            { 1186, 8 },   // MESBOX_V2, a plain message
            { 1189, 20 },  // OBJBOX_V2, a message with an item
        };

        // The queued continue: DIALOGUE (ActionTypes.DIALOGUE on the host),
        // param 0, no sub component, then (interface << 16) | component. The
        // same action the scripts' Dialogs sends.
        constexpr int32_t kDialogueActionId = 30;
        constexpr int32_t kDialogueParam    = 0;
        constexpr int32_t kNoSubComponent   = -1;

        // The option list a conversation asks its question in (1188
        // CHOICE_V2). Answered only inside a dialog zone.
        constexpr int32_t kOptionListInterface = 1188;

        // Dialog actions (continues and answers) one run may spend, across
        // every walk and landing. A route that keeps talking past this is a
        // conversation the walker should not be holding, and a page that
        // never closes must not pin the run forever.
        constexpr int32_t kMaxDialogActions = 20;

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
          planVarbitIds(reader.requirementVarbitIds().begin(),
                        reader.requirementVarbitIds().end()),
          requirementItemIds(reader.requirementItemIds())
    {
        if (std::find(planVarbitIds.begin(), planVarbitIds.end(), runtime::kInCombatVarbitId)
            == planVarbitIds.end())
        {
            planVarbitIds.push_back(runtime::kInCombatVarbitId);
        }
        // Distinct varbit / item id lists are built once on the artifact, so
        // the Executor pays no per-construction scan over the requirement
        // pool; the varbit list is copied only to add the combat varbit.
        // ww_executor_run constructs a fresh Executor on every run, so the
        // savings matter even at one call per game tick.
        //
        // Size the batched-callback output buffers once to the (fixed) lengths
        // of the id lists. resize() fills with zero so a host that bails out
        // and writes nothing (e.g. callback threw on first id) still leaves
        // sentinel-zero values for the planner to read.
        varbitValues.resize(planVarbitIds.size());
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

    WwStatus Executor::failRun(int32_t stepIndex, int32_t transitionIndex) const
    {
        emit(WwEventKind::Failed, stepIndex, transitionIndex);
        return WwStatus::Failed;
    }

    WwStatus Executor::walkOneStep(const runtime::Step &step, int32_t stepIndex,
                                   int32_t arrivalRadius, WwTile &outPosition)
    {
        const WwTile target{ step.targetX, step.targetY, static_cast<int32_t>(step.plane) };
        callbacks->walkTo(callbacks->user, target);
        emit(WwEventKind::StepAdvanced, stepIndex);

        auto stepStart = std::chrono::steady_clock::now();
        WwTile lastPos{};
        callbacks->readPosition(callbacks->user, &lastPos);
        outPosition = lastPos;
        int32_t stalledPolls = 0;
        bool hasReclicked = false;
        bool isResumePending = false;

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
            if (handleOpenDialog(pos))
            {
                // A conversation holds the player where they are: not a
                // stall, and the walk it interrupted is clicked again once
                // it closes.
                stalledPolls = 0;
                isResumePending = true;
                stepStart = std::chrono::steady_clock::now();
                lastPos = pos;
                continue;
            }
            if (isResumePending)
            {
                callbacks->walkTo(callbacks->user, target);
                isResumePending = false;
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
            const bool isStalled = stalledPolls >= kStalledPollsTrip;
            const bool isTimedOut = elapsedMs >= kStuckTimeoutMs;
            if (isStalled && !hasReclicked && !isTimedOut)
            {
                // The game drops a walk click issued while the player is still
                // in a forced move, which reads exactly like a stall. Click once
                // more before calling it stuck.
                callbacks->walkTo(callbacks->user, target);
                hasReclicked = true;
                stalledPolls = 0;
            }
            else if (isStalled || isTimedOut)
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

    WwStatus Executor::sleepCancellable(int32_t ticks) const
    {
        for (int32_t t = 0; t < ticks; ++t)
        {
            if (callbacks->shouldCancel(callbacks->user) != 0)
            {
                return WwStatus::Cancelled;
            }
            callbacks->sleepTicks(callbacks->user, 1);
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
        bool isWorn = false;
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
                    isWorn = true;
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
        const int32_t iface   = isWorn ? cs.a : cs.e;
        const int32_t comp    = isWorn ? cs.b : cs.f;
        const int32_t option  = isWorn ? cs.c : cs.g;
        const int32_t sub     = isWorn ? cs.d : cs.h;
        const int32_t special = isWorn ? 0 : cs.i;
        // For the backpack variant the baked sub-component (slot) is unreliable —
        // the item can sit in any slot — so pass the carried item id and let the
        // host resolve the live slot (the baked `sub` remains a fallback). The
        // worn variant addresses a fixed equipment slot, so it needs no lookup;
        // pass 0 to skip resolution there.
        const int32_t slotItem = isWorn ? 0 : carriedItem;
        callbacks->runChainStep(callbacks->user,
                                static_cast<int32_t>(data::ChainStepKind::ClickItem),
                                iface, comp, option, sub, special, slotItem, 0, 0, 0);
    }

    WwStatus Executor::runChainStep(const format::TransitionRecord &tx,
                                    const format::ChainStepRecord &cs) const
    {
        switch (static_cast<data::ChainStepKind>(cs.kind))
        {
            case data::ChainStepKind::Wait:
            {
                // a=ticks to sleep.
                return sleepCancellable(cs.a);
            }
            case data::ChainStepKind::WaitInterface:
            {
                // Block until interface `a` is open (e.g. a teleport dialog the
                // prior click opened). Times out to Failed so a chain that never
                // opens its dialog re-plans rather than hangs.
                return waitForInterface(cs.a);
            }
            case data::ChainStepKind::Click:
            {
                // Generic queued action: a=actionId, b/c/d=param1..3. For a
                // COMPONENT click the target interface is packed as param3>>16
                // ((iface<<16)|comp); wait for it to appear before clicking.
                // Non-component actions dispatch immediately.
                if (cs.a == kComponentActionId)
                {
                    const WwStatus st = waitForInterface(cs.d >> 16);
                    if (st != WwStatus::Arrived)
                    {
                        return st;
                    }
                }
                dispatchChainStep(cs);
                return WwStatus::Arrived;
            }
            case data::ChainStepKind::ClickItem:
            {
                // Pick the worn or carried variant of the item click. The
                // worn-vs-backpack decision needs the transition's item
                // requirements (the candidate item ids) — which the host does
                // not have — so resolve it here via the isItemWorn callback and
                // forward only the chosen variant.
                dispatchClickItem(tx, cs);
                return WwStatus::Arrived;
            }
            default:
            {
                // DialogueSelect: the host resolves the option component against
                // the live (possibly paged) dialogue. Any interface gating is
                // expressed as explicit WaitInterface steps, so just forward the
                // descriptors.
                dispatchChainStep(cs);
                return WwStatus::Arrived;
            }
        }
    }

    WwStatus Executor::runChain(const format::TransitionRecord &tx) const
    {
        const auto chain = artifact->chainSteps();
        const std::size_t chainStart = tx.chainStart;
        for (std::size_t i = 0; i < tx.chainCount; ++i)
        {
            if (callbacks->shouldCancel(callbacks->user) != 0)
            {
                return WwStatus::Cancelled;
            }
            const WwStatus st = runChainStep(tx, chain[chainStart + i]);
            if (st != WwStatus::Arrived)
            {
                return st;
            }
        }
        return WwStatus::Arrived;
    }

    WwStatus Executor::executeTransitionStep(const runtime::Step &step, int32_t stepIndex,
                                             WwTile &outPosition, bool &outIsOffCourse)
    {
        outIsOffCourse = false;
        // outPosition arrives holding the live position the prior step left,
        // which is where the player clicks this transition from.
        const WwTile start = outPosition;
        // The terminal Failed event is emitted by run() with both stepIndex
        // and transitionIndex; failure paths here just return WwStatus::Failed
        // so the dispatch site can carry the indices through.
        const auto txs = artifact->transitions();
        if (step.transitionIndex >= txs.size())
        {
            return WwStatus::Failed;
        }
        const format::TransitionRecord &tx = txs[step.transitionIndex];
        const std::size_t chainEnd = static_cast<std::size_t>(tx.chainStart) + tx.chainCount;
        if (chainEnd > artifact->chainSteps().size())
        {
            return WwStatus::Failed;
        }

        const int32_t transitionIndex = static_cast<int32_t>(step.transitionIndex);
        const bool isGlobal = (tx.flags & format::kTransitionFlagGlobalOrigin) != 0;
        emit(WwEventKind::StepAdvanced, stepIndex, transitionIndex);

        bool hasIssuedAction = true;
        if (!isGlobal)
        {
            // Click the world object from the interact-tile (the prior Walk
            // step put the player there). The object tile itself may be
            // blocked; the engine resolves the click from an adjacent tile.
            // An open door skipped here issued nothing, so there is no action
            // to settle for; we skip the post-chain wait below and the next
            // Walk step flows straight through the doorway.
            const LocInteract outcome = interactWithLoc(tx);
            if (outcome == LocInteract::Missing)
            {
                return WwStatus::Failed;
            }
            hasIssuedAction = outcome == LocInteract::Issued;
        }
        else
        {
            emit(WwEventKind::TeleportInitiated, stepIndex, transitionIndex);
        }

        const WwStatus chainResult = runChain(tx);
        if (chainResult != WwStatus::Arrived)
        {
            return chainResult;
        }

        // Let the engine commit the destination position before sampling it.
        // run() uses this position to decide whether the goal is satisfied
        // and whether to re-plan on a teleport-allowed flip. Skip the wait when
        // nothing was actually done: a no-op interact on an already-open door
        // (hasIssuedAction == false, no chain) leaves the avatar exactly where
        // the prior Walk left it, so there is no late-committing destination
        // to absorb — pausing here is the dead "walk up, stop, wait" the door
        // never needed. Teleports/stairs (global or chain-bearing) and any
        // issued click still settle as before.
        const bool didAct = isGlobal || hasIssuedAction || tx.chainCount > 0;
        if (didAct)
        {
            callbacks->sleepTicks(callbacks->user, kPostChainSettleTicks);
        }
        callbacks->readPosition(callbacks->user, &outPosition);
        if (!isGlobal && hasIssuedAction)
        {
            awaitLanding(tx, start, outPosition);
            if (runtime::isSameFloorCrossing(tx) && isSameTile(outPosition, start))
            {
                // Still where the click was made: the click was dropped, or a
                // door that walks you through (Draynor Manor) has not started.
                // Click it once more. An open door is no longer found, so the
                // host no-ops and the walk goes on through the doorway.
                if (interactWithLoc(tx) == LocInteract::Issued)
                {
                    callbacks->sleepTicks(callbacks->user, kPostChainSettleTicks);
                    callbacks->readPosition(callbacks->user, &outPosition);
                    awaitLanding(tx, start, outPosition);
                }
            }
        }
        outIsOffCourse = didAct && isOffCourse(tx, outPosition);
        return WwStatus::Arrived;
    }

    bool Executor::isSameTile(const WwTile &a, const WwTile &b)
    {
        return a.x == b.x && a.y == b.y && a.plane == b.plane;
    }

    bool Executor::hasLanded(const format::TransitionRecord &tx, const WwTile &start,
                             const WwTile &at)
    {
        // Near the destination is not enough on its own: on a one-tile door
        // the tile the click is made from is already within a tile of the far
        // side. The player must be on the destination, or have moved to near it.
        const WwTile dest{ tx.destX, tx.destY, static_cast<int32_t>(tx.destPlane) };
        return isSameTile(at, dest)
            || (!isSameTile(at, start) && chebyshev(at, dest) <= kArrivalChebyshev);
    }

    bool Executor::continueOpenChat() const
    {
        for (const ChatPage &page : kChatPages)
        {
            if (callbacks->isInterfaceOpen(callbacks->user, page.interfaceId) == 0)
            {
                continue;
            }
            const int32_t hash = (page.interfaceId << 16) | page.continueComponent;
            callbacks->runChainStep(callbacks->user,
                                    static_cast<int32_t>(data::ChainStepKind::Click),
                                    kDialogueActionId, kDialogueParam, kNoSubComponent, hash,
                                    0, 0, 0, 0, 0);
            return true;
        }
        return false;
    }

    bool Executor::handleOpenDialog(const WwTile &at)
    {
        if (dialogActionsLeft <= 0)
        {
            return false;
        }
        const bool isHandled = answerOptionList(at) || continueOpenChat();
        if (isHandled)
        {
            --dialogActionsLeft;
        }
        return isHandled;
    }

    const format::DialogZoneRecord *Executor::dialogZoneAt(const WwTile &at) const
    {
        for (const format::DialogZoneRecord &zone : artifact->dialogZones())
        {
            const bool isInside = at.x >= zone.minX && at.x <= zone.maxX
                               && at.y >= zone.minY && at.y <= zone.maxY
                               && at.plane >= zone.planeMin && at.plane <= zone.planeMax;
            if (isInside && zone.answerCount > 0)
            {
                return &zone;
            }
        }
        return nullptr;
    }

    bool Executor::answerOptionList(const WwTile &at)
    {
        if (callbacks->isInterfaceOpen(callbacks->user, kOptionListInterface) == 0)
        {
            return false;
        }
        const format::DialogZoneRecord *zone = dialogZoneAt(at);
        if (zone == nullptr)
        {
            return false;
        }
        // One answer per poll, in the zone's order: when the first matches, the
        // list closes and the rest are never sent; when it does not, the next
        // poll tries the next. Sending them all at once could queue two picks.
        const std::size_t index = zone->answerStart + answerCursor % zone->answerCount;
        ++answerCursor;
        const format::DialogAnswerRecord &answer = artifact->dialogAnswers()[index];
        int32_t slots[9]{};
        static_assert(sizeof(slots) == sizeof(answer.text));
        std::memcpy(slots, answer.text, sizeof(slots));
        callbacks->runChainStep(callbacks->user,
                                static_cast<int32_t>(data::ChainStepKind::DialogueAnswer),
                                slots[0], slots[1], slots[2], slots[3], slots[4], slots[5],
                                slots[6], slots[7], slots[8]);
        return true;
    }

    void Executor::awaitLanding(const format::TransitionRecord &tx, const WwTile &start,
                                WwTile &ioPosition)
    {
        int32_t stillPolls = 0;
        int32_t tick = 0;
        while (tick < kLandingMaxTicks)
        {
            // A conversation holds the player where they are until it is
            // answered, so it neither spends the landing budget nor counts as
            // standing still.
            if (handleOpenDialog(ioPosition))
            {
                // The continue is what lets the transition go ahead, so it
                // gets the same settle as the click, and the still count
                // starts over from it.
                callbacks->sleepTicks(callbacks->user, kPostChainSettleTicks);
                callbacks->readPosition(callbacks->user, &ioPosition);
                stillPolls = 0;
                continue;
            }
            if (hasLanded(tx, start, ioPosition) || stillPolls >= kLandingStillPolls)
            {
                return;
            }
            ++tick;
            callbacks->sleepTicks(callbacks->user, 1);
            WwTile pos{};
            callbacks->readPosition(callbacks->user, &pos);
            stillPolls = isSameTile(pos, ioPosition) ? stillPolls + 1 : 0;
            ioPosition = pos;
        }
    }

    bool Executor::isOffCourse(const format::TransitionRecord &tx, const WwTile &at)
    {
        const WwTile dest{ tx.destX, tx.destY, static_cast<int32_t>(tx.destPlane) };
        return chebyshev(at, dest) > kLandingSlack;
    }

    Executor::LocInteract Executor::interactWithLoc(const format::TransitionRecord &tx) const
    {
        const WwTile origin{ tx.originX, tx.originY, static_cast<int32_t>(tx.originPlane) };
        const bool isSkippable = runtime::isSameFloorCrossing(tx);
        for (int32_t attempt = 0; attempt < kMissingLocAttempts; ++attempt)
        {
            if (attempt > 0)
            {
                callbacks->sleepTicks(callbacks->user, 1);
            }
            if (callbacks->interact(callbacks->user, tx.objectId, origin,
                                    static_cast<int32_t>(tx.optionIndex)) != 0)
            {
                return LocInteract::Issued;
            }
            if (isSkippable)
            {
                return LocInteract::SkippedOpenCrossing;
            }
        }
        return LocInteract::Missing;
    }

    void Executor::refreshRequirementValues()
    {
        // One batched call per list collapses what was N sequential pipe
        // round-trips (~25-30 for the lodestone-unlock varbits) into one
        // host-side call, which the Java bridge in turn services with at most
        // two batched RPCs instead of N synchronous ones. This was the
        // dominant cost in pre-walk latency.
        callbacks->readVarbits(callbacks->user,
                               planVarbitIds.data(),
                               planVarbitIds.size(),
                               varbitValues.data());
        for (std::size_t i = 0; i < planVarbitIds.size(); ++i)
        {
            snapshot.setVarbit(planVarbitIds[i], varbitValues[i]);
        }
        // Likewise the live count of every item a requirement references (e.g.
        // the dungeoneering cape). Without this an item-gated teleport is
        // rejected against count 0 and the planner falls back to a walk.
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
        refreshRequirementValues();

        // Re-derive the scene's dynamic-region grid on every (re-)plan, for the
        // same reason the capability snapshot is re-pulled: a single run can
        // cross an instance boundary — walking out of a house portal, or into
        // one — and a grid captured once at entry would then resolve every tile
        // through the wrong scene. A static scene answers with a zeroed struct,
        // which clears the map.
        WwInstanceChunks chunks{};
        callbacks->readInstance(callbacks->user, &chunks);
        installInstance(context.instance, &chunks);

        return context.assembler.assemble(
            start.x, start.y, start.plane,
            goal.x, goal.y, goal.plane,
            &snapshot, outPlan);
    }

    Executor::ReplanOutcome Executor::replan(const WwGoal &goal, runtime::SearchContext &context,
                                             int32_t stepIndex, RunState &io)
    {
        emit(WwEventKind::ReplanStarted, stepIndex);
        if (!planFrom(io.position, goal, context, plan))
        {
            return ReplanOutcome::Failed;
        }
        if (plan.steps.empty())
        {
            // Planner agrees we're at the goal even though the live position
            // fell outside the explicit radius (e.g., the goal tile is
            // unwalkable but the start tile lies on its acceptance set at
            // the area level).
            emit(WwEventKind::Arrived);
            return ReplanOutcome::Arrived;
        }
        io.isTeleAllowedAtLastPlan = isTeleAllowedForPlan(io.position);
        return ReplanOutcome::Restarted;
    }

    bool Executor::isTeleAllowedForPlan(const WwTile &at) const
    {
        return runtime::isTeleportAllowed(*artifact, &snapshot, at.x, at.y, at.plane);
    }

    bool Executor::isTeleAllowedLive(const WwTile &at) const
    {
        if (!runtime::isTeleportAllowed(*artifact, at.x, at.y, at.plane))
        {
            return false;
        }
        const int32_t id = runtime::kInCombatVarbitId;
        int32_t inCombat = 0;
        callbacks->readVarbits(callbacks->user, &id, 1, &inCombat);
        return inCombat != 1;
    }

    bool Executor::isRestart(ReplanOutcome outcome, int32_t stepIndex, WwStatus &outStatus) const
    {
        if (outcome == ReplanOutcome::Restarted)
        {
            return true;
        }
        outStatus = outcome == ReplanOutcome::Arrived ? WwStatus::Arrived : failRun(stepIndex, -1);
        return false;
    }

    int32_t Executor::arrivalRadiusFor(std::size_t i, const WwGoal &goal) const
    {
        const bool isNextWalk = (i + 1 < plan.steps.size())
            && plan.steps[i + 1].kind == runtime::StepKind::Walk;
        if (isNextWalk)
        {
            return kHandoffChebyshev;
        }
        const bool isFinalStep = (i + 1 == plan.steps.size());
        if (isFinalStep && goal.radius <= 0)
        {
            return 0;
        }
        return kArrivalChebyshev;
    }

    WwStatus Executor::executeStep(std::size_t i, const WwGoal &goal, WwTile &outPosition,
                                   bool &outIsOffCourse)
    {
        const runtime::Step &step = plan.steps[i];
        const int32_t stepIndex = static_cast<int32_t>(i);
        outIsOffCourse = false;
        if (step.kind == runtime::StepKind::Walk)
        {
            return walkOneStep(step, stepIndex, arrivalRadiusFor(i, goal), outPosition);
        }
        return executeTransitionStep(step, stepIndex, outPosition, outIsOffCourse);
    }

    WwStatus Executor::judgeDrainedRun(const WwGoal &goal, WwTile &ioPosition)
    {
        if (!isInsideGoal(ioPosition, goal))
        {
            // The walk poll often samples mid-stride; give the engine one
            // tick to commit the final tile before judging.
            callbacks->sleepTicks(callbacks->user, 1);
            callbacks->readPosition(callbacks->user, &ioPosition);
        }
        if (isInsideGoal(ioPosition, goal))
        {
            emit(WwEventKind::Arrived);
            return WwStatus::Arrived;
        }
        return failRun(static_cast<int32_t>(plan.steps.size()) - 1, -1);
    }

    WwStatus Executor::run(WwGoal goal)
    {
        RunState st;
        dialogActionsLeft = kMaxDialogActions;
        answerCursor = 0;
        callbacks->readPosition(callbacks->user, &st.position);
        if (isInsideGoal(st.position, goal))
        {
            emit(WwEventKind::Arrived);
            return WwStatus::Arrived;
        }

        // Borrow a SearchContext for the entire run so re-plans reuse the
        // same context without re-acquiring through the pool (ADR 0007:
        // contexts are heap-allocated and never relocated). The lease's
        // destructor returns it to the pool on every exit path — including
        // the implicit throw paths inside planFrom() / walkOneStep() — so
        // there is no "forgot to release on this branch" failure mode here.
        runtime::ContextLease lease = pool->acquire();
        runtime::SearchContext &context = *lease;

        // The plan member's vector grows once and is reused across re-plans
        // — outPlan.steps.clear() inside the assembler keeps the capacity.
        if (!planFrom(st.position, goal, context, plan))
        {
            return failRun(-1, -1);
        }
        if (plan.steps.empty())
        {
            emit(WwEventKind::Arrived);
            return WwStatus::Arrived;
        }
        // Anchor the teleport-allowed predicate at the planner's start so the
        // post-step check can detect a false→true flip and re-plan with global
        // teleports newly considerable (ADR 0009). It is evaluated fresh per
        // step — it changes at wilderness-level (8-tile) and no-tele-box
        // granularity, and when combat ends, so any coarser caching detects
        // the flip late. The anchor reads the snapshot the plan itself used,
        // so the plan and its anchor cannot disagree about combat.
        st.isTeleAllowedAtLastPlan = isTeleAllowedForPlan(st.position);

        std::size_t i = 0;
        while (i < plan.steps.size())
        {
            const int32_t stepIndex = static_cast<int32_t>(i);
            bool isOffCourse = false;
            const WwStatus stepResult = executeStep(i, goal, st.position, isOffCourse);
            if (stepResult == WwStatus::Cancelled)
            {
                return WwStatus::Cancelled;
            }
            if (stepResult == WwStatus::Failed)
            {
                // Transition failures and exhausted re-plan budgets are
                // terminal. Walk failures (the Stuck event was emitted inside
                // walkOneStep) consume one re-plan from the live position —
                // re-read, because a host that teleports us between samples
                // may have moved further than walkOneStep's last sample.
                const runtime::Step &step = plan.steps[i];
                const bool isTransition = step.kind == runtime::StepKind::Transition;
                if (isTransition || st.replansUsed >= kMaxReplans)
                {
                    return failRun(stepIndex,
                                   isTransition ? static_cast<int32_t>(step.transitionIndex) : -1);
                }
                callbacks->readPosition(callbacks->user, &st.position);
                ++st.replansUsed;
                WwStatus terminal = WwStatus::Failed;
                if (!isRestart(replan(goal, context, stepIndex, st), stepIndex, terminal))
                {
                    return terminal;
                }
                i = 0;
                continue;
            }

            // Step Arrived; `st.position` holds the live position, so the goal
            // check and the teleport-allowed flip need no redundant read.
            if (isInsideGoal(st.position, goal))
            {
                emit(WwEventKind::Arrived);
                return WwStatus::Arrived;
            }
            if (isOffCourse)
            {
                // The transition put the player somewhere other than its
                // destination: a failed agility obstacle, a refused teleport.
                // Plan again from where they are, on the reroute budget.
                const int32_t txIndex = static_cast<int32_t>(plan.steps[i].transitionIndex);
                if (st.reroutesUsed >= kMaxReroutes)
                {
                    return failRun(stepIndex, txIndex);
                }
                ++st.reroutesUsed;
                WwStatus terminal = WwStatus::Failed;
                if (!isRestart(replan(goal, context, stepIndex, st), stepIndex, terminal))
                {
                    return terminal;
                }
                i = 0;
                continue;
            }
            const bool isTeleAllowedNow = isTeleAllowedLive(st.position);
            if (isTeleAllowedNow && !st.isTeleAllowedAtLastPlan && st.replansUsed < kMaxReplans)
            {
                ++st.replansUsed;
                WwStatus terminal = WwStatus::Failed;
                if (!isRestart(replan(goal, context, stepIndex, st), stepIndex, terminal))
                {
                    return terminal;
                }
                i = 0;
                continue;
            }
            st.isTeleAllowedAtLastPlan = isTeleAllowedNow;
            ++i;
        }
        return judgeDrainedRun(goal, st.position);
        // lease destructor returns the context to the pool here.
    }
}
