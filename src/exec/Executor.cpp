#include "exec/Executor.h"

#include "data/Transitions.h"
#include "format/Artifact.h"
#include "runtime/SearchContext.h"
#include "runtime/TeleportPolicy.h"
#include "runtime/TransitionShape.h"
#include "runtime/WorldView.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <utility>

namespace ww::exec
{
    namespace
    {
        // Tunables, all open per CONTEXT.md. Polling cadence is in game ticks;
        // the stuck deadline is wall-clock (executor links the CRT DLL).
        constexpr int32_t kPollTicks            = 2;     // ~1.2s game time between polls
        // Near a hop's handoff the walk polls every tick instead, so the next
        // click fires within a tick of the player reaching the handoff point
        // rather than up to kPollTicks late - at a run that is 4 tiles, the
        // whole lead, and the player would have stopped on the tile first.
        constexpr int32_t kNearPollTicks        = 1;
        constexpr int32_t kArrivalChebyshev     = 1;     // accept being within 1 tile of step target
        // Movement pace, in tiles a game tick: walking 1, running 2.
        constexpr int32_t kWalkTilesPerTick     = 1;
        constexpr int32_t kRunTilesPerTick      = 2;
        // Human walkers don't wait to land on each waypoint before clicking the
        // next - they click ahead while still moving, so motion is continuous.
        // When another Walk step follows on the same walkable stretch, the hop
        // hands off once the player is within a lead drawn per hop from this
        // range of its target, and never less than one tick of travel plus a
        // tile at the observed pace (so a running player, 2 tiles a tick, is
        // never stepped past the lead between two 1-tick polls). The tight
        // kArrivalChebyshev is kept for the last walk before an interact /
        // Transition (and the final approach to goal), where landing on the
        // exact interact tile matters. Open tuning per CONTEXT.md.
        constexpr int32_t kLeadMinTiles         = 2;
        constexpr int32_t kLeadMaxTiles         = 5;
        // Now and then a walker stops dead and stands a moment before the next
        // click: this share of Walk-to-Walk handoffs arrives within
        // kArrivalChebyshev instead, then idles 0..kFullStopMaxIdleTicks.
        constexpr int32_t kFullStopPercent      = 5;
        constexpr int32_t kFullStopMaxIdleTicks = 2;
        // Game ticks with the player's tile unchanged before a hop is stalled
        // (three polls of kPollTicks, as it was when every poll was two ticks).
        constexpr int32_t kStalledTicksTrip     = 6;
        // Stuck deadline per Walk hop: a base plus an allowance per path tile.
        // Walking without run is ~0.6s a tile, so 0.9s leaves room for a
        // winding hop and a slow client without letting a dead one linger.
        constexpr int64_t kStuckBaseMs          = 8000;
        constexpr int64_t kStuckMsPerPathTile   = 900;

        // Walk leg: the straight-line distance, in tiles, from the player to
        // a walk click, drawn per hop uniform in [kLegMinTiles, kLegMaxTiles].
        // An off-screen walk is clicked on the minimap (nxt-library #70), and
        // a click the minimap cannot reach is refused. Its clickable region is
        // the ellipse inscribed in the 220 px minimap at 4 px a tile, a circle
        // of 27.5 tiles at default zoom that turns with the camera, so the
        // bound is a radius and not a Chebyshev box: (22, 22) is 31 tiles out.
        // 22 leaves room for the player still moving and for zoom drift. A
        // leg ends part-way along a planned step where it has to, so its
        // length is not rounded to the planner's step.
        constexpr int32_t kLegMinTiles        = 14;
        constexpr int32_t kLegMaxTiles        = 22;
        // A refused walk click reads exactly like a stall (the player never
        // moves), so a walk hop that ends stuck lowers the leg ceiling by
        // kLegShrinkTiles, never under kLegFloorTiles, and below the leg that
        // stalled; the re-plan's hops are clicked shorter. Each hop that
        // arrives raises it again by kLegRecoverTiles, but only up to
        // kLegShrinkTiles below the leg that stalled: the reach that refused
        // it is unknown, and every refusal costs a stall and a re-plan from a
        // budget of kMaxReplans.
        constexpr int32_t kLegShrinkTiles     = 7;
        constexpr int32_t kLegRecoverTiles    = 3;
        constexpr int32_t kLegFloorTiles      = 8;
        // The lowest leg drawn: legs are drawn from up to kLegMaxTiles -
        // kLegMinTiles below the ceiling, but never below this, so a ceiling
        // at its floor still leaves a range rather than one fixed length.
        constexpr int32_t kLegLowestTiles     = 5;
        // A part-way tile is at least this far from the player, so the hop
        // moves them.
        constexpr int32_t kLegMinPartTiles    = 2;
        // Part-way hops in a row that may arrive with the player not moved
        // before the run re-plans from where they stand.
        constexpr int32_t kMaxStillPartWayHops = 2;
        // Path tiles a hop may run past its leg, so a winding stretch is not
        // clicked far along: the game's pathfinder takes the click and knows
        // nothing of the planner's fences.
        constexpr int32_t kLegWindTiles       = 8;
        // A leg that would stop fewer tiles than this short of a step takes
        // the whole step instead, while that is inside the ceiling, so no
        // click is made only to finish the last tile or two.
        constexpr int32_t kLegMinRestTiles    = 4;
        static_assert(kLegLowestTiles <= kLegFloorTiles && kLegFloorTiles <= kLegMinTiles
                          && kLegMinTiles <= kLegMaxTiles,
                      "leg range is ordered");
        // A human clicks a portal, door or stairs once it is on screen and
        // lets the game walk the last tiles (the Draynor Underworld portal,
        // 2026-10-27: walking right up to it first looked wrong). The client
        // paths an interaction to any loc in its loaded scene, and the default
        // camera shows about 15 tiles round the player, so 12 tiles of planned
        // path keeps the loc on screen and inside any walk leg. The
        // range is drawn per approach.
        constexpr int32_t kClickRangeMinTiles = 5;
        constexpr int32_t kClickRangeMaxTiles = 12;
        // After a click from range: within kRangeNearTiles of the origin or
        // the approach tile the player is at the loc (a big loc like that
        // portal is used from a ring round its footprint, and its origin is a
        // corner). The game sets the player walking the tick after a click,
        // so kRangeStillTicks still anywhere short of it mean the click was
        // dropped or led nowhere. kRangeSlackTicks on top of the walk.
        constexpr int32_t kRangeNearTiles     = 3;
        constexpr int32_t kRangeStillTicks    = 3;
        constexpr int32_t kRangeSlackTicks    = 4;
        static_assert(kRangeNearTiles < kClickRangeMinTiles, "a click from range is never at it");
        // Salts the run's seed for clickRng, so it draws apart from rng.
        constexpr uint32_t kClickSeedSalt     = 0x9E3779B9U;
        // A click hands the route to the game's pathfinder, which knows
        // nothing of the planner's Wilderness fence. Within this many tiles of
        // a Wilderness box the executor clicks each planned step in turn, so
        // the game has no room to cut through.
        constexpr int32_t kWildernessMargin   = 8;
        // Steps from the player within which isShortestWalkClear looks for
        // the game's walks near that fence: twice the longest leg and a
        // little, so a walk round an obstacle is still seen whole. A tile
        // only reached by a longer walk is not shown clear.
        constexpr int32_t kShortestWalkReach  = 2 * kLegMaxTiles + 4;
        // Ceiling of WwEvent::interactionHint: 1 + 1000 path tiles. Past that
        // an interaction is no nearer than "far".
        constexpr int32_t kMaxInteractionHint = 1001;

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
        // pit), within kLandingMaxTicks. A long local hop holds the player
        // still for several ticks before the move (Everlight's bridge, live
        // 2026-10-16: still at 3s, across at 4.5s), so for one stillness
        // mid-crossing is not a landing, and the budget gets a tick per tile
        // of the hop on top.
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

        // A route around a local transition that landed off course is refused
        // as a detour when it costs more than kDetourCostFactor times the route
        // through that transition and at least kDetourMinExcess more (plan cost
        // units, about a tile or a tick each). A door judged refused next to
        // the goal once sent the walk off by lodestone and ~190 tiles to reach
        // a tile 8 away (Amberfell bank, 2026-09-30); retrying the crossing
        // once is the cheaper bet.
        constexpr float kDetourCostFactor = 4.0F;
        constexpr float kDetourMinExcess  = 40.0F;

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

        // RS3's Content Warning System modal (1262 CWS_WARNING_MAIN, "Are you
        // sure you want to climb down?"), raised by locs such as the
        // Stronghold of Security ladders. Until Yes is clicked the player
        // stands still, so the landing wait judged the climb refused. Yes is
        // 1262:2 (CWS_WARNING_MAIN__YES_TEXT), clicked as a COMPONENT action
        // with option 1, no sub component: the same GameAction(COMPONENT, 1,
        // -1, 82706434) BotWithUs-Scripts' WarningModal.confirm sends. 1262:3
        // is No, and 1262:5 (CWS_TOGGLE_BUTTON, "Don't ask me this again")
        // changes the account's settings; the walker deliberately never
        // touches either.
        constexpr int32_t kContentWarningInterface = 1262;
        constexpr int32_t kContentWarningYes       = (kContentWarningInterface << 16) | 2;
        constexpr int32_t kContentWarningOption    = 1;
        static_assert(kContentWarningYes == 82706434, "1262:2 CWS_WARNING_MAIN__YES_TEXT");

        // Dialog actions (continues, answers and warning confirms) a stretch
        // of the run may spend: the walks before the first transition, then
        // each transition step with the walks after it, until the next one
        // refills it. Per transition rather than per run, because the
        // Stronghold of Security crosses ~12 doors a level and each can ask a
        // question; a per-run 20 failed the walk at the second or third door.
        // The Executor adds the largest dialog zone's answer count to this
        // (dialogBudget), so one step can try every answer once and still
        // continue the pages around them. Still bounded per step, so a page
        // that never closes cannot pin the run: the steps themselves are
        // bounded by the re-plan and reroute budgets.
        constexpr int32_t kMaxDialogActions = 20;

        // The most answers any one dialog zone carries.
        int32_t largestAnswerCount(std::span<const format::DialogZoneRecord> zones)
        {
            int32_t largest = 0;
            for (const format::DialogZoneRecord &zone : zones)
            {
                largest = std::max<int32_t>(largest, zone.answerCount);
            }
            return largest;
        }

        // Chebyshev distance on the same plane; INT32_MAX on plane mismatch so
        // a teleport mid-walk reads as "infinitely far": never within an
        // arrival radius, and never inside a hop's click cap.
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

        // Whether `b` is within `leg` tiles of `a` in a straight line, on the
        // same plane: the reach of a minimap click (see kLegMaxTiles).
        bool isWithinLeg(const WwTile &a, const WwTile &b, int32_t leg)
        {
            if (a.plane != b.plane)
            {
                return false;
            }
            const int64_t dx = a.x - b.x;
            const int64_t dy = a.y - b.y;
            return dx * dx + dy * dy <= static_cast<int64_t>(leg) * leg;
        }

        WwTile targetOf(const runtime::Step &step)
        {
            return WwTile{ step.targetX, step.targetY, static_cast<int32_t>(step.plane) };
        }

        // The tile `d` Chebyshev steps from `from` on the line to `to`, `len`
        // steps away (0 < len).
        WwTile tileAlong(const WwTile &from, const WwTile &to, int32_t d, int32_t len)
        {
            return WwTile{ from.x + (to.x - from.x) * d / len, from.y + (to.y - from.y) * d / len,
                           from.plane };
        }

        // Uniform integer in [lo, hi], both inclusive.
        int32_t drawBetween(std::mt19937 &rng, int32_t lo, int32_t hi)
        {
            return std::uniform_int_distribution<int32_t>(lo, hi)(rng);
        }

        // A step that works the dialog a carried teleport item opens, which a
        // worn-direct click never opens.
        bool isItemDialogStep(const format::ChainStepRecord &cs)
        {
            const auto kind = static_cast<data::ChainStepKind>(cs.kind);
            return kind == data::ChainStepKind::WaitInterface
                || kind == data::ChainStepKind::DialogueSelect;
        }
    }

    Executor::Executor(const format::ArtifactReader &reader,
                       runtime::ContextPool &pool,
                       const Callbacks &callbacks,
                       std::optional<uint32_t> fixedSeed,
                       uint32_t disabledMoves,
                       std::vector<uint32_t> excludedTransitions)
        : artifact(&reader),
          pool(&pool),
          callbacks(&callbacks),
          planVarbitIds(reader.requirementVarbitIds().begin(),
                        reader.requirementVarbitIds().end()),
          requirementItemIds(reader.requirementItemIds()),
          dialogBudget(kMaxDialogActions + largestAnswerCount(reader.dialogZones())),
          fixedSeed(fixedSeed),
          disabledMoves(disabledMoves),
          callerExcludedTransitions(std::move(excludedTransitions))
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

    void Executor::emit(WwEventKind kind, int32_t stepIndex, int32_t transitionIndex,
                        int32_t interactionHint) const
    {
        if (callbacks->onEvent == nullptr)
        {
            return;
        }
        const WwEvent event{ static_cast<int32_t>(kind), interactionHint, stepIndex,
                             transitionIndex };
        callbacks->onEvent(callbacks->user, &event);
    }

    WwStatus Executor::failRun(int32_t stepIndex, int32_t transitionIndex) const
    {
        emit(WwEventKind::Failed, stepIndex, transitionIndex);
        return WwStatus::Failed;
    }

    int64_t Executor::stuckDeadlineMs(int32_t pathTiles)
    {
        return kStuckBaseMs + kStuckMsPerPathTile * std::max<int64_t>(pathTiles, 0);
    }

    int32_t Executor::handoffRadius(const Handoff &handoff) const
    {
        if (!handoff.isLeading)
        {
            return handoff.radius;
        }
        // Between two 1-tick polls the player covers tilesPerTick tiles, so a
        // lead one past that is never stepped over: the click always fires
        // with the player still short of the target, still moving.
        return std::max(handoff.radius, tilesPerTick + 1);
    }

    int32_t Executor::pollTicksFor(const WwTile &at, const WwTile &target, int32_t radius)
    {
        // Poll every tick once one kPollTicks poll at a run could carry the
        // player inside the radius; farther out, every kPollTicks. A plane
        // mismatch is infinitely far (chebyshev), so it polls at the slow rate.
        const int64_t outside = static_cast<int64_t>(chebyshev(at, target)) - radius;
        return outside <= kPollTicks * kRunTilesPerTick ? kNearPollTicks : kPollTicks;
    }

    void Executor::notePace(const WwTile &from, const WwTile &to, int32_t ticks)
    {
        // Tiles a tick over the last poll, rounded up: running reads 2, walking
        // 1. Standing still says nothing about the pace, and a jump farther
        // than a run covers (Surge, Dive, a teleport) is no pace at all.
        const int32_t moved = chebyshev(from, to);
        if (ticks <= 0 || moved <= 0 || moved > ticks * kRunTilesPerTick)
        {
            return;
        }
        const int32_t pace = (moved + ticks - 1) / ticks;
        tilesPerTick = std::clamp(pace, kWalkTilesPerTick, kRunTilesPerTick);
    }

    Executor::WalkProgress Executor::judgeProgress(const WwTile &pos, const WwTile &lastPos,
                                                   const WwTile &target, int32_t ticks,
                                                   int64_t deadlineMs, WalkWatch &io)
    {
        // Progress is the player moving at all: a long hop winds round walls
        // and can head away from its target for several polls. A player on
        // another plane than the target (a teleport mid-walk) is not making
        // progress on this hop however they moved.
        const bool isUnmoved = isSameTile(pos, lastPos) || pos.plane != target.plane;
        io.stalledTicks = isUnmoved ? io.stalledTicks + ticks : 0;
        const auto elapsed = std::chrono::steady_clock::now() - io.start;
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        const bool isStalled = io.stalledTicks >= kStalledTicksTrip;
        const bool isTimedOut = elapsedMs >= deadlineMs;
        if (isStalled && !io.hasReclicked && !isTimedOut)
        {
            // The game drops a walk click issued while the player is still in
            // a forced move, which reads exactly like a stall. Click once more
            // before calling it stuck.
            io.hasReclicked = true;
            io.stalledTicks = 0;
            return WalkProgress::Reclick;
        }
        return (isStalled || isTimedOut) ? WalkProgress::Stuck : WalkProgress::Moving;
    }

    bool Executor::reclickInReach(bool isDue, const WwTile &pos, const WwTile &target)
    {
        if (!isDue)
        {
            return true;
        }
        if (!isWithinLeg(pos, target, legCeilingTiles))
        {
            return false;
        }
        callbacks->walkTo(callbacks->user, target);
        return true;
    }

    WwStatus Executor::walkOneStep(const runtime::Step &step, int32_t stepIndex,
                                   const Handoff &handoff, int32_t pathTiles,
                                   int32_t interactionHint, WwTile &outPosition,
                                   bool &outIsDisplaced)
    {
        outIsDisplaced = false;
        const WwTile target = targetOf(step);
        const int64_t deadlineMs = stuckDeadlineMs(pathTiles);
        // The event goes first: a host that holds back Surge / Dive near an
        // interaction reads the hint when the walkTo arrives.
        emit(WwEventKind::StepAdvanced, stepIndex, -1, interactionHint);
        callbacks->walkTo(callbacks->user, target);

        WalkWatch watch{ std::chrono::steady_clock::now(), 0, false };
        WwTile lastPos{};
        callbacks->readPosition(callbacks->user, &lastPos);
        outPosition = lastPos;
        bool isResumePending = false;

        while (true)
        {
            if (callbacks->shouldCancel(callbacks->user) != 0)
            {
                return WwStatus::Cancelled;
            }
            const int32_t ticks = pollTicksFor(lastPos, target, handoffRadius(handoff));
            callbacks->sleepTicks(callbacks->user, ticks);

            WwTile pos{};
            callbacks->readPosition(callbacks->user, &pos);
            outPosition = pos;
            notePace(lastPos, pos, ticks);
            if (chebyshev(pos, target) <= handoffRadius(handoff))
            {
                return WwStatus::Arrived;
            }
            if (handleOpenDialog(pos))
            {
                // A conversation holds the player where they are: not a
                // stall, and the walk it interrupted is clicked again once
                // it closes.
                watch = WalkWatch{ std::chrono::steady_clock::now(), 0, watch.hasReclicked };
                isResumePending = true;
                lastPos = pos;
                continue;
            }
            // A click again is made from where the player now stands: after a
            // forced move or a dialog that may be far from where the hop was
            // chosen, so past the leg the run re-plans from here instead.
            const bool isResumed = reclickInReach(std::exchange(isResumePending, false), pos, target);
            const WalkProgress progress =
                judgeProgress(pos, lastPos, target, ticks, deadlineMs, watch);
            if (!isResumed || !reclickInReach(progress == WalkProgress::Reclick, pos, target))
            {
                outIsDisplaced = true;
                return WwStatus::Failed;
            }
            if (progress == WalkProgress::Stuck)
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

    bool Executor::dispatchClickItem(const format::TransitionRecord &tx,
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
        // a..d = worn variant, e..h = backpack variant, i = flags.
        // The worn variant is a plain component click (never "special").
        const int32_t iface   = isWorn ? cs.a : cs.e;
        const int32_t comp    = isWorn ? cs.b : cs.f;
        const int32_t option  = isWorn ? cs.c : cs.g;
        const int32_t sub     = isWorn ? cs.d : cs.h;
        const int32_t special = isWorn ? 0 : (cs.i & data::kClickItemBackpackSpecial);
        // For the backpack variant the baked sub-component (slot) is unreliable —
        // the item can sit in any slot — so pass the carried item id and let the
        // host resolve the live slot (the baked `sub` remains a fallback). The
        // worn variant addresses a fixed equipment slot, so it needs no lookup;
        // pass 0 to skip resolution there.
        const int32_t slotItem = isWorn ? 0 : carriedItem;
        callbacks->runChainStep(callbacks->user,
                                static_cast<int32_t>(data::ChainStepKind::ClickItem),
                                iface, comp, option, sub, special, slotItem, 0, 0, 0);
        return isWorn;
    }

    WwStatus Executor::runChainStep(const format::TransitionRecord &tx,
                                    const format::ChainStepRecord &cs,
                                    bool &outSkipsDialog) const
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
                // forward only the chosen variant. A worn-direct op has
                // already teleported, so the dialog steps after it are moot.
                const bool isWorn = dispatchClickItem(tx, cs);
                outSkipsDialog = isWorn && (cs.i & data::kClickItemWornDirect) != 0;
                return WwStatus::Arrived;
            }
            case data::ChainStepKind::ClickNpc:
            {
                // The host picks the live NPC (type range, plane, radius) and
                // clicks it; the chain's WaitInterface is what notices a miss.
                dispatchChainStep(cs);
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

    bool Executor::hasNpcOrigin(const format::TransitionRecord &tx) const
    {
        if (tx.chainCount == 0)
        {
            return false;
        }
        const auto chain = artifact->chainSteps();
        return tx.chainStart < chain.size()
            && chain[tx.chainStart].kind == static_cast<uint8_t>(data::ChainStepKind::ClickNpc);
    }

    WwStatus Executor::runChain(const format::TransitionRecord &tx) const
    {
        const auto chain = artifact->chainSteps();
        const std::size_t chainStart = tx.chainStart;
        bool skipsDialog = false;
        for (std::size_t i = 0; i < tx.chainCount; ++i)
        {
            if (callbacks->shouldCancel(callbacks->user) != 0)
            {
                return WwStatus::Cancelled;
            }
            const format::ChainStepRecord &cs = chain[chainStart + i];
            if (skipsDialog && isItemDialogStep(cs))
            {
                continue;
            }
            const WwStatus st = runChainStep(tx, cs, skipsDialog);
            if (st != WwStatus::Arrived)
            {
                return st;
            }
        }
        return WwStatus::Arrived;
    }

    WwStatus Executor::executeTransitionStep(const runtime::Step &step, int32_t stepIndex,
                                             const RangeClick *rangeClick,
                                             runtime::WorldView &view, WwTile &outPosition,
                                             StepReport &outReport)
    {
        outReport = StepReport{};
        // Every transition may raise its own conversation (a Stronghold of
        // Security door's question), so each starts with a full dialog budget
        // rather than what the doors before it left (see kMaxDialogActions).
        dialogActionsLeft = dialogBudget;
        // outPosition arrives holding the live position the prior step left,
        // which is where the player clicks this transition from, or, clicked
        // from range, the approach tile the walk would have left them on.
        const WwTile start = rangeClick != nullptr ? rangeClick->approach : outPosition;
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
        const bool isNpcOrigin = hasNpcOrigin(tx);
        emit(WwEventKind::StepAdvanced, stepIndex, transitionIndex);
        if (isGlobal || isNpcOrigin)
        {
            emit(WwEventKind::TeleportInitiated, stepIndex, transitionIndex);
        }
        // Clicked from range and under way, there is nothing to click again.
        bool isStarted = false;
        if (rangeClick != nullptr)
        {
            const WwStatus ranged =
                clickFromRange(tx, isNpcOrigin, *rangeClick, outPosition, isStarted,
                               outReport.isDisplaced);
            if (ranged != WwStatus::Arrived)
            {
                return ranged;
            }
        }

        bool hasIssuedAction = true;
        if (!isStarted && !isGlobal && !isNpcOrigin)
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
                outReport.isLocMissing = true;
                return WwStatus::Failed;
            }
            hasIssuedAction = outcome == LocInteract::Issued;
            outReport.isCrossingSkipped = outcome == LocInteract::SkippedOpenCrossing;
        }

        // An NPC clicked from range ran its whole chain already.
        const bool isChainDone = isStarted && isNpcOrigin;
        const WwStatus chainResult = isChainDone ? WwStatus::Arrived : runChain(tx);
        if (chainResult == WwStatus::Failed && isNpcOrigin)
        {
            // The NPC was not there to click, or its interface never opened:
            // the same dead origin a missing loc is, so route around it
            // rather than end the run on it.
            outReport.isLocMissing = true;
        }
        if (chainResult == WwStatus::Failed && isGlobal)
        {
            // The teleport's dialog never opened, or a pick in it failed. A
            // player still on the tile it was clicked from can be routed
            // around it; one who moved anyway (a late teleport) fails the run.
            callbacks->readPosition(callbacks->user, &outPosition);
            outReport.isChainRefused = isSameTile(outPosition, start);
        }
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
            awaitLanding(tx, start, outPosition, view);
            if (runtime::isSameFloorCrossing(tx) && isShortOfCrossing(tx, start, outPosition))
            {
                retryUnmovedCrossing(tx, start, outPosition, view);
            }
        }
        outReport.isOffCourse = didAct && hasMissedLanding(tx, start, outPosition, view);
        return WwStatus::Arrived;
    }

    WwStatus Executor::clickFromRange(const format::TransitionRecord &tx, bool isNpcOrigin,
                                      const RangeClick &click, WwTile &ioPosition,
                                      bool &outIsStarted, bool &outIsDisplaced)
    {
        outIsStarted = false;
        outIsDisplaced = false;
        const RangeStart started = issueFromRange(tx, isNpcOrigin, click, ioPosition);
        if (started != RangeStart::NotStarted)
        {
            outIsStarted = started == RangeStart::Started;
            return outIsStarted ? WwStatus::Arrived : WwStatus::Cancelled;
        }
        // Nothing took from range: walk the rest as the walk would have, onto
        // the approach tile, and the click is made once more from there.
        const std::size_t approachStep = click.step - 1;
        if (!isWithinLeg(ioPosition, targetOf(plan.steps[approachStep]), legCeilingTiles))
        {
            outIsDisplaced = true;  // not where the stop was: re-plan from here
            return WwStatus::Failed;
        }
        return walkOneStep(plan.steps[approachStep], static_cast<int32_t>(approachStep),
                           Handoff{ kArrivalChebyshev, 0, false }, click.pathTiles,
                           interactionHintFor(approachStep), ioPosition, outIsDisplaced);
    }

    Executor::RangeStart Executor::issueFromRange(const format::TransitionRecord &tx,
                                                  bool isNpcOrigin, const RangeClick &click,
                                                  WwTile &ioPosition)
    {
        if (isNpcOrigin)
        {
            const WwStatus chained = runChain(tx);
            if (chained == WwStatus::Cancelled)
            {
                return RangeStart::Cancelled;
            }
            if (chained != WwStatus::Arrived)
            {
                return RangeStart::NotStarted;
            }
        }
        else if (interactWithLoc(tx) != LocInteract::Issued)
        {
            return RangeStart::NotStarted;
        }
        return awaitApproach(tx, click, ioPosition);
    }

    Executor::RangeStart Executor::awaitApproach(const format::TransitionRecord &tx,
                                                 const RangeClick &click, WwTile &ioPosition)
    {
        const int32_t maxTicks =
            (click.pathTiles + kLeadMaxTiles) / kWalkTilesPerTick + kRangeSlackTicks;
        int32_t stillTicks = 0;
        for (int32_t tick = 0; tick < maxTicks; ++tick)
        {
            if (callbacks->shouldCancel(callbacks->user) != 0)
            {
                return RangeStart::Cancelled;
            }
            if (isAtRangeTarget(tx, click, ioPosition) || isDialogUp())
            {
                return RangeStart::Started;
            }
            callbacks->sleepTicks(callbacks->user, 1);
            WwTile pos{};
            callbacks->readPosition(callbacks->user, &pos);
            stillTicks = isSameTile(pos, ioPosition) ? stillTicks + 1 : 0;
            ioPosition = pos;
            if (stillTicks >= kRangeStillTicks && !isAtRangeTarget(tx, click, pos))
            {
                return RangeStart::NotStarted;
            }
        }
        // Still walking at the end of the budget: the landing wait takes over.
        return RangeStart::Started;
    }

    bool Executor::isAtRangeTarget(const format::TransitionRecord &tx, const RangeClick &click,
                                   const WwTile &at)
    {
        const WwTile origin{ tx.originX, tx.originY, static_cast<int32_t>(tx.originPlane) };
        return at.plane != origin.plane || chebyshev(at, origin) <= kRangeNearTiles
            || chebyshev(at, click.approach) <= kRangeNearTiles
            || hasLanded(tx, click.approach, at);
    }

    bool Executor::isDialogUp() const
    {
        for (const ChatPage &page : kChatPages)
        {
            if (callbacks->isInterfaceOpen(callbacks->user, page.interfaceId) != 0)
            {
                return true;
            }
        }
        return callbacks->isInterfaceOpen(callbacks->user, kOptionListInterface) != 0
            || callbacks->isInterfaceOpen(callbacks->user, kContentWarningInterface) != 0;
    }

    void Executor::retryUnmovedCrossing(const format::TransitionRecord &tx, const WwTile &start,
                                        WwTile &ioPosition, runtime::WorldView &view)
    {
        // Still short of the crossing: the click was dropped, a door that
        // walks you through (Draynor Manor) has not started, or the click
        // walked the player up to the door and opened it without taking them
        // through (Amberfell bank). Click it once more. An open door is no
        // longer found, so the host no-ops.
        if (interactWithLoc(tx) == LocInteract::Issued)
        {
            // The chain goes with the click. Shantay Pass's click raises a
            // warning its chain confirms; a bare re-click only raised it
            // again, and the walk below then closed it (2026-10-03). A
            // failed chain leaves the player short, which is judged below.
            if (runChain(tx) == WwStatus::Cancelled)
            {
                return;
            }
            callbacks->sleepTicks(callbacks->user, kPostChainSettleTicks);
            callbacks->readPosition(callbacks->user, &ioPosition);
            awaitLanding(tx, start, ioPosition, view);
        }
        if (!isShortOfCrossing(tx, start, ioPosition))
        {
            return;
        }
        // Two clicks and the player is still on this side: the door opened in
        // front of them, or the click reached a loc that is not really there.
        // An open door leaves its closed loc hidden in the scene, and a host
        // that finds the hidden one clicks it to no effect (Sinclair Mansion's
        // gate, Falador castle's doors). Walk across, as for an open door the
        // host skipped: an open doorway lets the player through, a shut one
        // holds them on this side for the landing judgement to refuse.
        const WwTile dest{ tx.destX, tx.destY, static_cast<int32_t>(tx.destPlane) };
        callbacks->walkTo(callbacks->user, dest);
        callbacks->sleepTicks(callbacks->user, kPostChainSettleTicks);
        callbacks->readPosition(callbacks->user, &ioPosition);
        awaitLanding(tx, start, ioPosition, view);
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
        // side. The player must be on the destination, or have moved to near
        // it and off both the tile they clicked from and the row's origin.
        const WwTile dest{ tx.destX, tx.destY, static_cast<int32_t>(tx.destPlane) };
        return isSameTile(at, dest)
            || (!isShortOfCrossing(tx, start, at) && chebyshev(at, dest) <= kArrivalChebyshev);
    }

    bool Executor::isShortOfCrossing(const format::TransitionRecord &tx, const WwTile &start,
                                     const WwTile &at)
    {
        // The approach walk hands over within kArrivalChebyshev of the origin,
        // so the click can be made a tile short of it; a door's click then
        // walks the player onto the origin and opens the door there without
        // taking them through. Both tiles are this side of the crossing.
        const WwTile origin{ tx.originX, tx.originY, static_cast<int32_t>(tx.originPlane) };
        return isSameTile(at, start) || isSameTile(at, origin);
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

    bool Executor::confirmContentWarning() const
    {
        if (callbacks->isInterfaceOpen(callbacks->user, kContentWarningInterface) == 0)
        {
            return false;
        }
        callbacks->runChainStep(callbacks->user,
                                static_cast<int32_t>(data::ChainStepKind::Click),
                                kComponentActionId, kContentWarningOption, kNoSubComponent,
                                kContentWarningYes, 0, 0, 0, 0, 0);
        return true;
    }

    bool Executor::handleOpenDialog(const WwTile &at)
    {
        if (dialogActionsLeft <= 0)
        {
            return false;
        }
        // The warning first: it sits over whatever raised it, and nothing
        // behind it moves until it is confirmed.
        const bool isHandled =
            confirmContentWarning() || answerOptionList(at) || continueOpenChat();
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
                                WwTile &ioPosition, runtime::WorldView &view)
    {
        const bool isLongHop = runtime::isLongLocalHop(tx);
        const int32_t maxTicks =
            isLongHop ? kLandingMaxTicks + runtime::transitionHop(tx) : kLandingMaxTicks;
        int32_t stillPolls = 0;
        int32_t tick = 0;
        while (tick < maxTicks)
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
            const bool isAtRest = stillPolls >= kLandingStillPolls
                               && !(isLongHop && isMidCrossing(tx, start, ioPosition, view));
            if (hasLanded(tx, start, ioPosition) || isAtRest)
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

    bool Executor::isMidCrossing(const format::TransitionRecord &tx, const WwTile &start,
                                 const WwTile &at, runtime::WorldView &view)
    {
        if (isShortOfCrossing(tx, start, at))
        {
            return true;
        }
        return !view.isInstanced() && view.areaAt(at.x, at.y, at.plane) < 0;
    }

    bool Executor::isOffCourse(const format::TransitionRecord &tx, const WwTile &at)
    {
        const WwTile dest{ tx.destX, tx.destY, static_cast<int32_t>(tx.destPlane) };
        return chebyshev(at, dest) > kLandingSlack;
    }

    Executor::AreaVerdict Executor::judgeLandingArea(const format::TransitionRecord &tx,
                                                     const WwTile &start, const WwTile &at,
                                                     runtime::WorldView &view)
    {
        // The area grid is the static world's; inside an instance it answers
        // for unrelated ground (WorldView::areaAt), so it has no say there.
        if (view.isInstanced())
        {
            return AreaVerdict::Unknown;
        }
        const int32_t startArea = view.areaAt(start.x, start.y, start.plane);
        const int32_t destArea =
            view.areaAt(tx.destX, tx.destY, static_cast<int32_t>(tx.destPlane));
        const int32_t atArea = view.areaAt(at.x, at.y, at.plane);
        const bool isDecidable = startArea >= 0 && destArea >= 0 && atArea >= 0
                              && startArea != destArea;
        if (!isDecidable)
        {
            return AreaVerdict::Unknown;
        }
        return atArea == destArea ? AreaVerdict::Crossed : AreaVerdict::Refused;
    }

    bool Executor::isOnNearSide(const format::TransitionRecord &tx, const WwTile &start,
                                const WwTile &at)
    {
        const WwTile dest{ tx.destX, tx.destY, static_cast<int32_t>(tx.destPlane) };
        return chebyshev(at, start) < chebyshev(at, dest);
    }

    bool Executor::isHeldBackBy(const format::TransitionRecord &tx, const WwTile &from,
                                const WwTile &at, runtime::WorldView &view)
    {
        if (!view.isInstanced())
        {
            const int32_t fromArea = view.areaAt(from.x, from.y, from.plane);
            const int32_t destArea =
                view.areaAt(tx.destX, tx.destY, static_cast<int32_t>(tx.destPlane));
            const int32_t atArea = view.areaAt(at.x, at.y, at.plane);
            const bool isDecidable = fromArea >= 0 && destArea >= 0 && atArea >= 0
                                  && fromArea != destArea;
            if (isDecidable)
            {
                return atArea == fromArea;
            }
        }
        return isOnNearSide(tx, from, at);
    }

    bool Executor::isStalledBehindSkip(const std::optional<SkippedCrossing> &skipped,
                                       const runtime::Step &step, const WwTile &at,
                                       runtime::WorldView &view) const
    {
        if (!skipped.has_value() || step.kind != runtime::StepKind::Walk)
        {
            return false;
        }
        const format::TransitionRecord &tx = artifact->transitions()[skipped->transitionIndex];
        return isHeldBackBy(tx, skipped->from, at, view);
    }

    bool Executor::hasMissedLanding(const format::TransitionRecord &tx, const WwTile &start,
                                    const WwTile &at, runtime::WorldView &view)
    {
        if (isOffCourse(tx, at))
        {
            return true;
        }
        if ((tx.flags & format::kTransitionFlagGlobalOrigin) != 0)
        {
            return false;
        }
        const AreaVerdict verdict = judgeLandingArea(tx, start, at, view);
        if (verdict != AreaVerdict::Unknown)
        {
            return verdict == AreaVerdict::Refused;
        }
        return runtime::isSameFloorCrossing(tx) && isOnNearSide(tx, start, at);
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
                            std::span<const uint32_t> excludedTransitions,
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
        for (const uint32_t transitionIndex : excludedTransitions)
        {
            snapshot.excludeTransition(transitionIndex);
        }
        for (const uint32_t transitionIndex : callerExcludedTransitions)
        {
            snapshot.excludeTransition(transitionIndex);
        }
        snapshot.disableMoves(disabledMoves, artifact->moveCategories(), artifact->membersOnly());

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
        io.rangeClick.reset();
        if (!planFrom(io.position, goal, io.excludedTransitions, context, plan))
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

    bool Executor::replanOnBudget(const WwGoal &goal, runtime::SearchContext &context,
                                  int32_t stepIndex, RunState &io, WwStatus &outTerminal)
    {
        if (io.replansUsed >= kMaxReplans)
        {
            outTerminal = failRun(stepIndex, -1);
            return false;
        }
        ++io.replansUsed;
        io.stillPartWayHops = 0;
        return isRestart(replan(goal, context, stepIndex, io), stepIndex, outTerminal);
    }

    bool Executor::isHopMoving(bool isPartWay, const WwTile &hopFrom, RunState &io)
    {
        if (!isPartWay || !isSameTile(hopFrom, io.position))
        {
            io.stillPartWayHops = 0;
            return true;
        }
        return ++io.stillPartWayHops <= kMaxStillPartWayHops;
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

    void Executor::excludeTransitionsOfLoc(const format::TransitionRecord &missing,
                                           std::vector<uint32_t> &ioExcluded) const
    {
        appendTransitionsOfLoc(artifact->transitions(), missing, ioExcluded);
    }

    void appendTransitionsOfLoc(std::span<const format::TransitionRecord> txs,
                                const format::TransitionRecord &missing,
                                std::vector<uint32_t> &ioExcluded)
    {
        for (std::size_t i = 0; i < txs.size(); ++i)
        {
            const format::TransitionRecord &tx = txs[i];
            const bool isSameLoc = tx.objectId == missing.objectId
                                && tx.originX == missing.originX
                                && tx.originY == missing.originY
                                && tx.originPlane == missing.originPlane
                                && (tx.flags & format::kTransitionFlagGlobalOrigin) == 0;
            if (isSameLoc)
            {
                ioExcluded.push_back(static_cast<uint32_t>(i));
            }
        }
    }

    void Executor::excludeForReroute(uint32_t transitionIndex,
                                     std::vector<uint32_t> &ioExcluded) const
    {
        const format::TransitionRecord &tx = artifact->transitions()[transitionIndex];
        if ((tx.flags & format::kTransitionFlagGlobalOrigin) != 0)
        {
            ioExcluded.push_back(transitionIndex);
            return;
        }
        excludeTransitionsOfLoc(tx, ioExcluded);
    }

    bool Executor::rerouteAroundDeadTransition(uint32_t transitionIndex, const WwGoal &goal,
                                               runtime::SearchContext &context, int32_t stepIndex,
                                               RunState &io, WwStatus &outStatus)
    {
        const int32_t txIndex = static_cast<int32_t>(transitionIndex);
        if (io.reroutesUsed >= kMaxReroutes)
        {
            outStatus = failRun(stepIndex, txIndex);
            return false;
        }
        ++io.reroutesUsed;
        excludeForReroute(transitionIndex, io.excludedTransitions);
        callbacks->readPosition(callbacks->user, &io.position);
        const ReplanOutcome outcome = replan(goal, context, stepIndex, io);
        if (outcome == ReplanOutcome::Restarted)
        {
            return true;
        }
        // No route without it: the run still failed on this transition.
        outStatus = outcome == ReplanOutcome::Arrived ? WwStatus::Arrived
                                                      : failRun(stepIndex, txIndex);
        return false;
    }

    bool Executor::rerouteAroundOffCourse(uint32_t transitionIndex, const WwGoal &goal,
                                          runtime::SearchContext &context, int32_t stepIndex,
                                          RunState &io, WwStatus &outStatus)
    {
        if (io.reroutesUsed >= kMaxReroutes)
        {
            outStatus = failRun(stepIndex, static_cast<int32_t>(transitionIndex));
            return false;
        }
        ++io.reroutesUsed;
        // Without the exclusion the planner, asked the same question from
        // the same place, picks the same transition again, so a transport
        // that never lands was chosen, missed and chosen again until the
        // budget ran out. A global transition has no loc, so only its own
        // row is ruled out.
        const std::size_t keptCount = io.excludedTransitions.size();
        const format::TransitionRecord &tx = artifact->transitions()[transitionIndex];
        const bool isGlobal = (tx.flags & format::kTransitionFlagGlobalOrigin) != 0;
        // A local miss judged by area alone, the player still within the
        // landing slack, may be a misjudgement (a door opened in front of the
        // player); one far from the destination did not cross, whatever a
        // retry costs. The first such miss of a transition is weighed against
        // the detour; its cost through the transition is planned before the
        // exclusion, so the snapshot the kept plan reads is its own.
        const bool isWeighed = !isGlobal && !isOffCourse(tx, io.position)
            && std::find(io.detourRefusals.begin(), io.detourRefusals.end(), transitionIndex)
                   == io.detourRefusals.end();
        std::optional<float> throughCost;
        if (isWeighed)
        {
            throughCost = planCostFrom(io.position, goal, io.excludedTransitions, context);
        }
        excludeForReroute(transitionIndex, io.excludedTransitions);
        ReplanOutcome outcome = replan(goal, context, stepIndex, io);
        if (outcome == ReplanOutcome::Failed && isGlobal)
        {
            // A global teleport that did not land was refused on state the
            // walker cannot see (a lodestone the server says has vanished, a
            // pick on a component that moved), not lost to a failure rate, so
            // a second try from the same tile gets the same answer. It stays
            // excluded and the run fails on it.
            outStatus = failRun(stepIndex, static_cast<int32_t>(transitionIndex));
            return false;
        }
        const bool isAbsurdDetour = outcome == ReplanOutcome::Restarted
                                 && throughCost.has_value()
                                 && isDetourAbsurd(*throughCost, plan.cost);
        if (isAbsurdDetour)
        {
            // Refused once only: a second miss of the same transition is
            // not a misjudgement, and the long way round is then the way.
            io.detourRefusals.push_back(transitionIndex);
        }
        if (outcome == ReplanOutcome::Failed || isAbsurdDetour)
        {
            // Nothing else reaches the goal, or only a detour many times the
            // way through does. Put the rows back rather than fail a run a
            // second try might finish (a failed jump on the only way across)
            // or send it the long way round on one judgement; the list returns
            // to exactly what it held before.
            io.excludedTransitions.resize(keptCount);
            outcome = replan(goal, context, stepIndex, io);
        }
        return isRestart(outcome, stepIndex, outStatus);
    }

    std::optional<float> Executor::planCostFrom(const WwTile &start, const WwGoal &goal,
                                                std::span<const uint32_t> excludedTransitions,
                                                runtime::SearchContext &context)
    {
        if (!planFrom(start, goal, excludedTransitions, context, costProbe))
        {
            return std::nullopt;
        }
        return costProbe.cost;
    }

    bool Executor::isDetourAbsurd(float throughCost, float detourCost)
    {
        return detourCost > throughCost * kDetourCostFactor
            && detourCost - throughCost >= kDetourMinExcess;
    }

    Executor::Handoff Executor::handoffFor(std::size_t i, const WwGoal &goal, const WwTile &from,
                                           bool isPartWay)
    {
        // A leg that ends part-way along step i is followed by the rest of
        // that same step, a Walk.
        const bool isNextWalk = isPartWay
            || ((i + 1 < plan.steps.size()) && plan.steps[i + 1].kind == runtime::StepKind::Walk);
        if (isNextWalk)
        {
            // All three drawn before anything can rule the lead out, so every
            // Walk-to-Walk hop spends the same draws.
            const bool isFullStop = drawBetween(rng, 0, 99) < kFullStopPercent;
            const int32_t lead = drawBetween(rng, kLeadMinTiles, kLeadMaxTiles);
            const int32_t idle = drawBetween(rng, 0, kFullStopMaxIdleTicks);
            if (isFullStop)
            {
                return Handoff{ drawBetween(rng, 0, kArrivalChebyshev), idle, false };
            }
            // Walking out of a tile that allows no teleport onto one that
            // does, chooseHop ends the hop on the first such target so the
            // re-plan onto a teleport fires there: arrive on it, as a
            // Transition's approach does, or the check runs a hop late.
            const WwTile target = targetOf(plan.steps[i]);
            const bool isTeleportFlip =
                !runtime::isTeleportAllowed(*artifact, from.x, from.y, from.plane)
                && runtime::isTeleportAllowed(*artifact, target.x, target.y, target.plane);
            return isTeleportFlip ? Handoff{ kArrivalChebyshev, 0, false }
                                  : Handoff{ lead, 0, true };
        }
        const bool isFinalStep = (i + 1 == plan.steps.size());
        if (isFinalStep && goal.radius <= 0)
        {
            return Handoff{ 0, 0, false };
        }
        return Handoff{ kArrivalChebyshev, 0, false };
    }

    int32_t Executor::drawLeg()
    {
        const int32_t ceiling = std::clamp(legCeilingTiles, kLegFloorTiles, kLegMaxTiles);
        const int32_t floor = std::max(kLegLowestTiles, ceiling - (kLegMaxTiles - kLegMinTiles));
        return drawBetween(rng, floor, ceiling);
    }

    void Executor::shortenLegs(int32_t stalledLeg)
    {
        legRecoverCapTiles = std::clamp(stalledLeg - kLegShrinkTiles, kLegFloorTiles, kLegMaxTiles);
        legCeilingTiles = std::max(kLegFloorTiles,
                                   std::min(legCeilingTiles - kLegShrinkTiles, legRecoverCapTiles));
    }

    void Executor::recoverLegs()
    {
        legCeilingTiles = std::min(legRecoverCapTiles, legCeilingTiles + kLegRecoverTiles);
    }

    bool Executor::isWildernessFenced(const WwTile &from, const WwGoal &goal) const
    {
        return !runtime::isInWilderness(*artifact, from.x, from.y, from.plane)
            && !runtime::isInWilderness(*artifact, goal.x, goal.y, goal.plane);
    }

    bool Executor::isNearWildernessFence(const WwTile &at) const
    {
        for (const format::WildernessRegion &w : artifact->wildernessRegions())
        {
            if (at.plane < w.planeMin || at.plane > w.planeMax)
            {
                continue;
            }
            const int32_t dx = std::max({ w.minX - at.x, at.x - w.maxX, 0 });
            const int32_t dy = std::max({ w.minY - at.y, at.y - w.maxY, 0 });
            if (std::max(dx, dy) <= kWildernessMargin)
            {
                return true;
            }
        }
        return false;
    }

    Executor::Hop Executor::chooseHop(std::size_t first, const WwTile &from, const WwGoal &goal,
                                      runtime::SearchContext &context)
    {
        const WwTile firstTarget = targetOf(plan.steps[first]);
        // Drawn before anything can cut the hop short, so every Walk hop
        // spends the same draws and a pinned seed replays the same walk.
        const int32_t leg = drawLeg();
        const LegScope scope{ from, leg, leg + kLegWindTiles, isWildernessFenced(from, goal) };
        if (firstTarget.plane != from.plane || (scope.isFenced && isNearWildernessFence(firstTarget)))
        {
            // Another plane is no distance at all: the player is not where
            // the plan thinks. Near the fence the hop is the next step alone.
            return stepAlone(scope, first, context);
        }
        const bool isTeleBlockedHere = !runtime::isTeleportAllowed(*artifact, from.x, from.y,
                                                                   from.plane);
        std::optional<Hop> hop;
        WwTile prev = first == 0 ? from : targetOf(plan.steps[first - 1]);
        int32_t pathTiles = 0;
        for (std::size_t j = first; j < plan.steps.size(); ++j)
        {
            const WwTile target = targetOf(plan.steps[j]);
            if (plan.steps[j].kind != runtime::StepKind::Walk || target.plane != from.plane)
            {
                break;  // the approach walk to a Transition is clicked itself
            }
            if (scope.isFenced && isNearWildernessFence(target))
            {
                // The game's pathfinder could cut through the fence from
                // here: only the next step.
                return stepAlone(scope, first, context);
            }
            // The first step's length is the player's own distance to it.
            const int32_t stepTiles = j == first ? chebyshev(from, target) : chebyshev(prev, target);
            if (!isWithinLeg(from, target, leg) || pathTiles + stepTiles > scope.budget)
            {
                if (const std::optional<Hop> cut = cutLeg(scope, j, prev, pathTiles, stepTiles, context);
                    cut.has_value())
                {
                    hop = cut;
                }
                break;
            }
            pathTiles += stepTiles;
            hop = Hop{ j, pathTiles, std::nullopt, leg };
            prev = target;
            if (isTeleBlockedHere
                && runtime::isTeleportAllowed(*artifact, target.x, target.y, target.plane))
            {
                break;  // re-plan with teleports from here, as a step at a time did
            }
        }
        return hop.has_value() ? *hop : stepAlone(scope, first, context);
    }

    Executor::Hop Executor::stepAlone(const LegScope &scope, std::size_t first,
                                      runtime::SearchContext &context) const
    {
        const WwTile target = targetOf(plan.steps[first]);
        // Off the plan only when the player is: on another plane, or farther
        // from the step than any leg reaches. A step merely past this hop's
        // drawn leg is cut to it, as every other hop is.
        if (!isWithinLeg(scope.from, target, kLegMaxTiles))
        {
            return reachTowards(first, scope.from, scope.leg);
        }
        // Near the fence the step itself is clicked only if the game cannot
        // walk to it through a box, whichever least-step walk it takes: a
        // hand-off a lead short of the last step can leave the player where
        // the walk on to this one cuts a box's corner. Else a tile short of
        // it is looked for, as for a step past the leg.
        const bool isStepInLeg = isWithinLeg(scope.from, target, scope.leg);
        const bool isStepFenceClear = !scope.isFenced || !isNearWildernessFence(target)
            || isShortestWalkClear(scope.from, target, context);
        if (isStepInLeg && isStepFenceClear)
        {
            return Hop{ first, chebyshev(scope.from, target), std::nullopt, scope.leg };
        }
        // A tile short of the step: within the leg on the straight line to
        // it, else within the ceiling a refusal may have lowered (a click
        // past it is refused again and comes back displaced, so the ceiling
        // would never learn), on that line or on a walk to the step (the
        // line may cross ground nobody stands on). Near the fence every such
        // tile may be within the margin; then one that no least-step walk
        // from the player, the game's pathfinder's choice, can take into a
        // box, nor lies in one.
        const FenceRule margin = scope.isFenced ? FenceRule::Margin : FenceRule::None;
        const FenceRule walks = scope.isFenced ? FenceRule::ShortestWalk : FenceRule::None;
        struct Search
        {
            bool      isOnWalk;
            int32_t   reach;
            FenceRule fence;
        };
        const Search searches[] = {
            { false, scope.leg, margin },
            { false, legCeilingTiles, margin },
            { true, legCeilingTiles, margin },
            { false, legCeilingTiles, walks },
            { true, legCeilingTiles, walks },
        };
        for (const Search &search : searches)
        {
            const std::optional<WwTile> part = search.isOnWalk
                ? partWayOnWalk(scope, target, search.reach, search.fence, context)
                : partWayAlong(scope.from, target, scope.from, search.reach,
                               std::numeric_limits<int32_t>::max(), search.fence, context);
            if (part.has_value())
            {
                return Hop{ first, chebyshev(scope.from, *part), part, search.reach };
            }
        }
        if (const std::optional<WwTile> part = partWayClearOfBoxes(scope, target, context))
        {
            return Hop{ first, chebyshev(scope.from, *part), part, legCeilingTiles };
        }
        // Else the step whole, inside the cap but possibly past the ceiling,
        // rather than a re-plan that would rebuild the same geometry. This is
        // the one walk click that can pass a lowered ceiling, and only when
        // no standable tile within it lies on the line or on a walk to the
        // step that passes the fence rules above. Past the ceiling the
        // minimap may refuse it again; then the hop comes back displaced and
        // the run re-plans. Near the fence it is the planned step itself,
        // when no tile two or more steps on towards it is clear of every box.
        return Hop{ first, chebyshev(scope.from, target), std::nullopt,
                    isStepInLeg ? scope.leg : kLegMaxTiles };
    }

    std::optional<Executor::Hop> Executor::cutLeg(const LegScope &scope, std::size_t j,
                                                  const WwTile &prev, int32_t pathTiles,
                                                  int32_t stepTiles,
                                                  runtime::SearchContext &context) const
    {
        const WwTile target = targetOf(plan.steps[j]);
        const FenceRule fence = scope.isFenced ? FenceRule::Margin : FenceRule::None;
        const std::optional<WwTile> part = partWayAlong(prev, target, scope.from, scope.leg,
                                                        scope.budget - pathTiles, fence, context);
        const bool isNearlyThere =
            !part.has_value() || chebyshev(*part, target) < kLegMinRestTiles;
        if (isNearlyThere && isWithinLeg(scope.from, target, legCeilingTiles)
            && pathTiles + stepTiles <= legCeilingTiles + kLegWindTiles)
        {
            return Hop{ j, pathTiles + stepTiles, std::nullopt, legCeilingTiles };
        }
        if (part.has_value())
        {
            return Hop{ j, pathTiles + chebyshev(prev, *part), part, scope.leg };
        }
        return std::nullopt;
    }

    std::optional<WwTile> Executor::partWayAlong(const WwTile &start, const WwTile &end,
                                                 const WwTile &from, int32_t leg,
                                                 int32_t pathLeft, FenceRule fence,
                                                 runtime::SearchContext &context) const
    {
        const int32_t len = chebyshev(start, end);
        if (len <= 1 || len == std::numeric_limits<int32_t>::max())
        {
            return std::nullopt;
        }
        // Furthest first: the tile nearest `end` that is in reach, inside the
        // path budget, standable, clear of the Wilderness fence, and nearer
        // `end` than the player already is (else the hop would go nowhere and
        // the cursor, held on the step, would choose it again). Counted back
        // from `end`, as findRangeStop counts, so both land on the same tiles
        // of the line.
        const int32_t nearest = std::max(1, len - std::min(pathLeft, len));
        const int32_t playerToEnd = chebyshev(from, end);
        for (int32_t back = nearest; back < len && back < playerToEnd; ++back)
        {
            const WwTile at = tileAlong(end, start, back, len);
            if (isWithinLeg(from, at, leg) && chebyshev(from, at) >= kLegMinPartTiles
                && context.view.isStandable(at.x, at.y, at.plane)
                && isClearOfFence(from, at, fence, context))
            {
                return at;
            }
        }
        return std::nullopt;
    }

    std::optional<WwTile> Executor::partWayOnWalk(const LegScope &scope, const WwTile &end,
                                                  int32_t leg, FenceRule fence,
                                                  runtime::SearchContext &context) const
    {
        const runtime::WalkFence walkFence = scope.isFenced
            ? runtime::WalkFence{ artifact->wildernessRegions(), nullptr }
            : runtime::WalkFence{};
        runtime::TilePath walk;
        if (end.plane != scope.from.plane
            || !context.tileSearch.findPath(scope.from.x, scope.from.y, end.x, end.y, end.plane,
                                            runtime::TileSearch::kAnyArea, walkFence, walk)
            || walk.tiles.size() < 2)
        {
            return std::nullopt;
        }
        // Furthest first, short of the step: the walk's last tile is `end`.
        for (auto it = std::next(walk.tiles.rbegin()); it != walk.tiles.rend(); ++it)
        {
            const WwTile at{ it->x, it->y, end.plane };
            if (isWithinLeg(scope.from, at, leg) && chebyshev(scope.from, at) >= kLegMinPartTiles
                && isClearOfFence(scope.from, at, fence, context))
            {
                return at;
            }
        }
        return std::nullopt;
    }

    std::optional<WwTile> Executor::partWayClearOfBoxes(const LegScope &scope, const WwTile &end,
                                                        runtime::SearchContext &context) const
    {
        if (!scope.isFenced || end.plane != scope.from.plane || !isNearWildernessFence(end))
        {
            return std::nullopt;
        }
        const std::optional<runtime::TilePoint> at = context.tileSearch.clearestStepTowards(
            scope.from.x, scope.from.y, runtime::TilePoint{ end.x, end.y }, end.plane,
            artifact->wildernessRegions(), kShortestWalkReach, kLegMinPartTiles, legCeilingTiles);
        if (!at.has_value())
        {
            return std::nullopt;
        }
        return WwTile{ at->x, at->y, end.plane };
    }

    bool Executor::isClearOfFence(const WwTile &from, const WwTile &at, FenceRule fence,
                                  runtime::SearchContext &context) const
    {
        switch (fence)
        {
        case FenceRule::None:
            return true;
        case FenceRule::Margin:
            return !isNearWildernessFence(at);
        case FenceRule::ShortestWalk:
            return isShortestWalkClear(from, at, context);
        }
        return false;
    }

    bool Executor::isShortestWalkClear(const WwTile &from, const WwTile &at,
                                       runtime::SearchContext &context) const
    {
        return from.plane == at.plane
            && !context.tileSearch.canShortestWalkEnter(from.x, from.y, runtime::TilePoint{ at.x, at.y },
                                                        at.plane, artifact->wildernessRegions(),
                                                        kShortestWalkReach);
    }

    Executor::Hop Executor::reachTowards(std::size_t first, const WwTile &from, int32_t leg) const
    {
        const WwTile target = targetOf(plan.steps[first]);
        // More than a leg off the plan, or on another plane (a forced move,
        // or a dialog that held the player while the plan moved on): the run
        // re-plans from here, and nothing is clicked past the leg.
        const bool isOffPlan = !isWithinLeg(from, target, leg);
        const int32_t tiles = isOffPlan ? 0 : chebyshev(from, target);
        return Hop{ first, tiles, std::nullopt, leg, isOffPlan };
    }

    std::optional<Executor::RangeClick> Executor::planRangeClick(std::size_t first, Hop &ioHop,
                                                                const WwTile &from,
                                                                const WwGoal &goal,
                                                                runtime::WorldView &view)
    {
        std::size_t k = first;
        while (k < plan.steps.size() && plan.steps[k].kind == runtime::StepKind::Walk)
        {
            ++k;
        }
        if (k == first || k == plan.steps.size() || !isRangeClickable(plan.steps[k], from, goal))
        {
            return std::nullopt;
        }
        // Drawn only ahead of a transition that may be clicked from range, and
        // from clickRng, so the walk's own draws are left as they were.
        const int32_t range = drawBetween(clickRng, kClickRangeMinTiles, kClickRangeMaxTiles);
        const std::optional<Hop> stop = findRangeStop(first, k, ioHop.last, from, range,
                                                      isWildernessFenced(from, goal), view);
        if (!stop.has_value())
        {
            return std::nullopt;
        }
        const WwTile target = targetOf(plan.steps[stop->last]);
        const WwTile stopTile = stop->stopAt.value_or(target);
        // Both walk clicks this makes stay within reach: the one to the stop,
        // and the fallback walk-in from the stop to the approach tile should
        // the click from range not take (clickFromRange). The walk-in is
        // measured here from the stop, and again there from where the player
        // stands, which may be a hand-off lead short of the stop. That second
        // check is the one that holds: where the two disagree the run
        // re-plans from there and clicks nothing, at the cost of one re-plan.
        // They never disagree the other way, as nothing between them lowers
        // the ceiling (an arrived hop only raises it).
        const WwTile approach = targetOf(plan.steps[k - 1]);
        if (!isWithinLeg(from, stopTile, ioHop.leg)
            || !isWithinLeg(stopTile, approach, legCeilingTiles))
        {
            return std::nullopt;  // a walk click past the leg would be refused
        }
        const int32_t stopShort = chebyshev(stopTile, target);
        ioHop = Hop{ stop->last, hopPathTiles(from, first, stop->last) - stopShort, stop->stopAt,
                     ioHop.leg };
        return RangeClick{ k, targetOf(plan.steps[k - 1]), stop->pathTiles };
    }

    std::optional<Executor::Hop> Executor::findRangeStop(std::size_t first, std::size_t k,
                                                         std::size_t last, const WwTile &from,
                                                         int32_t range, bool isFenced,
                                                         runtime::WorldView &view) const
    {
        const format::TransitionRecord &tx = artifact->transitions()[plan.steps[k].transitionIndex];
        const WwTile origin{ tx.originX, tx.originY, static_cast<int32_t>(tx.originPlane) };
        std::optional<Hop> stop;
        int32_t toEnd = 0;  // planned path tiles from plan.steps[e]'s target to the approach
        // Back from the approach along the plan, tile by tile on the line
        // between consecutive targets; every tile passed is walked by the
        // game's pathfinder once the click is made, so the Wilderness fence
        // rules each one out as chooseHop does.
        for (std::size_t e = k; e-- > first;)
        {
            const WwTile end = targetOf(plan.steps[e]);
            const WwTile start = e == first ? from : targetOf(plan.steps[e - 1]);
            if (end.plane != origin.plane || start.plane != origin.plane)
            {
                return stop;
            }
            // On the first hop's line `start` is the player's own tile (d ==
            // len): already in range, the click is made from there.
            const int32_t len = chebyshev(end, start);
            const int32_t lastD = e == first ? len : len - 1;
            for (int32_t d = e + 1 == k ? 1 : 0; d <= lastD; ++d)
            {
                // Past `range` only while nothing within it qualified (a path
                // that bends near the loc), and never past kClickRangeMaxTiles.
                const WwTile at = tileAlong(end, start, d, len);
                const bool isPastRange = toEnd + d > range;
                if ((isPastRange && stop.has_value()) || toEnd + d > kClickRangeMaxTiles
                    || (isFenced && isNearWildernessFence(at)))
                {
                    return stop;
                }
                if (e <= last && chebyshev(at, origin) >= kClickRangeMinTiles
                    && view.isStandable(at.x, at.y, at.plane))
                {
                    stop = Hop{ e, toEnd + d, d == 0 ? std::nullopt : std::optional<WwTile>{ at } };
                }
            }
            toEnd += len;
        }
        return stop;
    }

    bool Executor::isRangeClickable(const runtime::Step &step, const WwTile &from,
                                    const WwGoal &goal) const
    {
        const auto txs = artifact->transitions();
        if (step.kind != runtime::StepKind::Transition || step.transitionIndex >= txs.size())
        {
            return false;
        }
        const format::TransitionRecord &tx = txs[step.transitionIndex];
        const WwTile origin{ tx.originX, tx.originY, static_cast<int32_t>(tx.originPlane) };
        const bool isGlobal = (tx.flags & format::kTransitionFlagGlobalOrigin) != 0;
        return !isGlobal && origin.plane == from.plane
            && chebyshev(from, origin) >= kClickRangeMinTiles
            && !(isWildernessFenced(from, goal) && isNearWildernessFence(origin));
    }

    int32_t Executor::hopPathTiles(const WwTile &from, std::size_t first, std::size_t last) const
    {
        int32_t tiles = chebyshev(from, targetOf(plan.steps[first]));
        for (std::size_t j = first + 1; j <= last; ++j)
        {
            tiles += chebyshev(targetOf(plan.steps[j - 1]), targetOf(plan.steps[j]));
        }
        return tiles;
    }

    int32_t Executor::interactionHintFor(std::size_t i) const
    {
        int64_t tiles = 0;
        for (std::size_t k = i + 1; k < plan.steps.size(); ++k)
        {
            if (plan.steps[k].kind != runtime::StepKind::Walk || tiles >= kMaxInteractionHint)
            {
                break;
            }
            tiles += chebyshev(targetOf(plan.steps[k - 1]), targetOf(plan.steps[k]));
        }
        return static_cast<int32_t>(std::min<int64_t>(tiles + 1, kMaxInteractionHint));
    }

    WwStatus Executor::executeStep(std::size_t i, int32_t hopTiles, const WwGoal &goal,
                                   const RangeClick *rangeClick, std::optional<WwTile> stopAt,
                                   bool isPartWay, runtime::WorldView &view,
                                   WwTile &outPosition, StepReport &outReport)
    {
        const runtime::Step &step = plan.steps[i];
        const int32_t stepIndex = static_cast<int32_t>(i);
        outReport = StepReport{};
        if (step.kind != runtime::StepKind::Walk)
        {
            return executeTransitionStep(step, stepIndex, rangeClick, view, outPosition,
                                         outReport);
        }
        runtime::Step walk = step;
        if (stopAt.has_value())
        {
            walk.targetX = stopAt->x;
            walk.targetY = stopAt->y;
        }
        Handoff handoff = handoffFor(i, goal, outPosition, isPartWay);
        if (isPartWay && chebyshev(outPosition, *stopAt) <= handoffRadius(handoff))
        {
            // A lead as long as the hop would hand off before the player moved.
            handoff = Handoff{ kArrivalChebyshev, 0, false };
        }
        // Short of step i, the rest of it still lies between the click and
        // the next interaction.
        const int32_t rest = isPartWay ? chebyshev(*stopAt, targetOf(step)) : 0;
        const int32_t hint = std::min(interactionHintFor(i) + rest, kMaxInteractionHint);
        const WwStatus walked = walkOneStep(walk, stepIndex, handoff, hopTiles, hint,
                                            outPosition, outReport.isDisplaced);
        if (walked != WwStatus::Arrived || handoff.idleTicks == 0)
        {
            return walked;
        }
        return sleepCancellable(handoff.idleTicks);
    }

    bool Executor::isAtGoalStandIn(const WwGoal &goal, runtime::WorldView &view,
                                   const WwTile &at) const
    {
        if (plan.steps.empty())
        {
            return false;
        }
        const runtime::Step &last = plan.steps.back();
        const WwTile standIn{ last.targetX, last.targetY, static_cast<int32_t>(last.plane) };
        const WwTile goalTile{ goal.x, goal.y, goal.plane };
        if (last.kind != runtime::StepKind::Walk || isSameTile(standIn, goalTile)
            || !isSameTile(at, standIn))
        {
            return false;
        }
        // The planner ends a walk off the goal tile only when it snapped the
        // goal: the tile is blocked, or it sits in a sealed pocket (a banker
        // behind the booths) whose area the stand-in is not in.
        return !view.isStandable(goal.x, goal.y, goal.plane)
            || view.areaAt(goal.x, goal.y, goal.plane)
                   != view.areaAt(standIn.x, standIn.y, standIn.plane);
    }

    WwStatus Executor::judgeDrainedRun(const WwGoal &goal, runtime::WorldView &view,
                                       WwTile &ioPosition)
    {
        const auto isArrived = [&]()
        {
            return isInsideGoal(ioPosition, goal) || isAtGoalStandIn(goal, view, ioPosition);
        };
        if (!isArrived())
        {
            // The walk poll often samples mid-stride; give the engine one
            // tick to commit the final tile before judging.
            callbacks->sleepTicks(callbacks->user, 1);
            callbacks->readPosition(callbacks->user, &ioPosition);
        }
        if (isArrived())
        {
            emit(WwEventKind::Arrived);
            return WwStatus::Arrived;
        }
        return failRun(static_cast<int32_t>(plan.steps.size()) - 1, -1);
    }

    WwStatus Executor::run(WwGoal goal)
    {
        RunState st;
        dialogActionsLeft = dialogBudget;
        answerCursor = 0;
        tilesPerTick = kRunTilesPerTick;
        const uint32_t seed = fixedSeed.has_value() ? *fixedSeed : std::random_device{}();
        rng.seed(seed);
        clickRng.seed(seed ^ kClickSeedSalt);
        legCeilingTiles = kLegMaxTiles;
        legRecoverCapTiles = kLegMaxTiles;
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
        if (!planFrom(st.position, goal, st.excludedTransitions, context, plan))
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
            // A Walk is clicked as a hop over several of the plan's steps. The
            // cursor moves onto the step clicked before it runs, so the steps
            // passed over are simply never executed and everything below
            // (events, re-plans, a skipped crossing's blame) names the step
            // the player was actually sent to.
            //
            // A hop that stops short of a Transition to click it from range
            // (planRangeClick) leaves the click on the run state; the cursor
            // then moves straight onto the Transition, and the click goes with
            // it.
            int32_t hopTiles = 0;
            std::optional<WwTile> stopAt;
            // The hop's leg ran out part-way along step i (not a click from
            // range): the cursor stays on step i for the rest of it.
            bool isPartWay = false;
            int32_t hopLeg = 0;
            const std::optional<RangeClick> rangeClick = std::exchange(st.rangeClick, std::nullopt);
            if (plan.steps[i].kind == runtime::StepKind::Walk)
            {
                // Read afresh: the hop is measured from where the player is
                // now, not where the last step left them (a forced move in
                // between would put the click past the leg).
                callbacks->readPosition(callbacks->user, &st.position);
                Hop hop = chooseHop(i, st.position, goal, context);
                if (hop.isOffPlan)
                {
                    WwStatus terminal = WwStatus::Failed;
                    if (!replanOnBudget(goal, context, static_cast<int32_t>(i), st, terminal))
                    {
                        return terminal;
                    }
                    i = 0;
                    continue;
                }
                st.rangeClick = planRangeClick(i, hop, st.position, goal, context.view);
                const WwTile hopTarget = hop.stopAt.value_or(targetOf(plan.steps[hop.last]));
                if (st.rangeClick.has_value()
                    && chebyshev(hopTarget, st.position) <= kArrivalChebyshev)
                {
                    // In range where the player stands (or a step from it): no
                    // walk, the click goes from here.
                    i = st.rangeClick->step;
                    continue;
                }
                i = hop.last;
                hopTiles = hop.pathTiles;
                hopLeg = hop.leg;
                stopAt = hop.stopAt;
                isPartWay = stopAt.has_value() && !st.rangeClick.has_value();
            }
            const int32_t stepIndex = static_cast<int32_t>(i);
            const WwTile hopFrom = st.position;
            StepReport report{};
            const RangeClick *clicked = rangeClick.has_value() ? &*rangeClick : nullptr;
            const WwStatus stepResult = executeStep(i, hopTiles, goal, clicked, stopAt, isPartWay,
                                                    context.view, st.position, report);
            // Only the step right after a skip may blame it, so every step
            // takes it off the run state.
            const std::optional<SkippedCrossing> skipped =
                std::exchange(st.skippedCrossing, std::nullopt);
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
                // A walk that stalled behind a crossing skipped as an open
                // door: nothing was open there (Shantay Pass without a pass
                // re-planned onto the same gate until the budget ran out).
                const bool isHeldBack =
                    isStalledBehindSkip(skipped, step, st.position, context.view);
                if (report.isLocMissing || report.isChainRefused || isHeldBack)
                {
                    const uint32_t dead =
                        isHeldBack ? skipped->transitionIndex : step.transitionIndex;
                    WwStatus terminal = WwStatus::Failed;
                    if (!rerouteAroundDeadTransition(dead, goal, context, stepIndex, st, terminal))
                    {
                        return terminal;
                    }
                    i = 0;
                    continue;
                }
                if (isTransition && !report.isDisplaced)
                {
                    return failRun(stepIndex, static_cast<int32_t>(step.transitionIndex));
                }
                callbacks->readPosition(callbacks->user, &st.position);
                if (!report.isDisplaced)
                {
                    shortenLegs(hopLeg);  // a displaced player is no fault of the leg
                }
                WwStatus terminal = WwStatus::Failed;
                if (!replanOnBudget(goal, context, stepIndex, st, terminal))
                {
                    return terminal;
                }
                i = 0;
                continue;
            }
            if (plan.steps[i].kind == runtime::StepKind::Walk)
            {
                recoverLegs();
                if (!isHopMoving(isPartWay, hopFrom, st))
                {
                    WwStatus terminal = WwStatus::Failed;
                    if (!replanOnBudget(goal, context, stepIndex, st, terminal))
                    {
                        return terminal;
                    }
                    i = 0;
                    continue;
                }
            }

            // Step Arrived; `st.position` holds the live position, so the goal
            // check and the teleport-allowed flip need no redundant read.
            if (isInsideGoal(st.position, goal))
            {
                emit(WwEventKind::Arrived);
                return WwStatus::Arrived;
            }
            if (report.isOffCourse)
            {
                // The transition did not put the player across it: a failed
                // agility obstacle, a refused teleport, a stile or gate that
                // turned them back into the area they clicked from. Plan
                // again from where they are, around it where possible, on the
                // reroute budget.
                WwStatus terminal = WwStatus::Failed;
                if (!rerouteAroundOffCourse(plan.steps[i].transitionIndex, goal, context,
                                            stepIndex, st, terminal))
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
            if (report.isCrossingSkipped)
            {
                st.skippedCrossing = SkippedCrossing{ plan.steps[i].transitionIndex, st.position };
            }
            if (st.rangeClick.has_value())
            {
                i = st.rangeClick->step;
            }
            else if (!isPartWay)
            {
                ++i;
            }
        }
        return judgeDrainedRun(goal, context.view, st.position);
        // lease destructor returns the context to the pool here.
    }
}
