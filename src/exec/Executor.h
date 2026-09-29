#ifndef WORLDWALKER_EXEC_EXECUTOR_H
#define WORLDWALKER_EXEC_EXECUTOR_H

#include "exec/Callbacks.h"
#include "format/ArtifactReader.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/ContextPool.h"
#include "runtime/PathAssembler.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
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
    class WorldView;
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
    // "walk out of wilderness, then teleport" path (ADR 0009). Combat counts
    // too: in combat no teleport is planned, and the flip back out of combat
    // re-plans the same way (runtime::kInCombatVarbitId). Walk failures
    // (stuck / stall) consume one re-plan from a bounded budget and retry from
    // the live position; the budget runs out before infinite-loop pathologies
    // do.
    //
    // Walk hop: the planner's Walk steps are fine-grained, and the executor
    // clicks ahead over a random stride of them (chooseHop): the furthest
    // following Walk within a stride drawn per hop, never past a Transition,
    // never beyond kMaxClickChebyshev of the player, and only the next step
    // near the Wilderness fence the game's own pathfinder does not know about.
    // The steps passed over are not executed; the cursor moves straight to
    // the one clicked, so every event and re-plan names that step. Then
    // walkTo(target), and poll readPosition with sleepTicks between samples;
    // arrival = within the hop's radius of its target (random per hop when
    // another Walk follows, so the next click fires mid-stride at an uneven
    // point, now and then after a full stop and a short idle;
    // kArrivalChebyshev when the next action needs an exact tile). A stall
    // counter (the player's tile unchanged for N polls; the first trip only
    // re-clicks, since the game drops a click made mid forced-move) and a
    // wall-clock deadline scaled by the hop's length together detect "stuck"
    // and surface as Failed; shouldCancel
    // polled before every sleep aborts with Cancelled. The final live
    // position is written out so run() can drive re-plan / teleport-allowed
    // checks without re-reading.
    //
    // Transition step: look up the TransitionRecord by step.transitionIndex.
    // For local-origin (the common case) fire interact(objectId, originTile,
    // optionIndex); for global-origin emit TeleportInitiated and skip the
    // interact. A loc the host cannot find is skipped for a same-floor
    // crossing (an open door) and fails the transition otherwise. Then
    // iterate the embedded chain — for each Click, poll
    // isInterfaceOpen(targetInterface) with sleepTicks between polls until it
    // opens (or the budget elapses → Failed) then runChainStep(interface,
    // component, option); for each Wait, sleepTicks(ticks). After a short
    // post-chain settle the live position is read out (the engine commits the
    // destination during the settle). Transition Failed is terminal — re-plan
    // does not retry it — with one exception: a local transition whose loc
    // the host cannot find is excluded (with every row on that loc) for the
    // rest of the run, and the run re-plans around it on the reroute budget,
    // so a stale row costs a detour instead of the whole walk and the plan is
    // not rebuilt onto the same dead edge. After an issued local transition
    // the executor waits for the player to land (awaitLanding) and then judges
    // the landing (hasMissedLanding): by distance to the destination, and for
    // a local transition also by which area of the baked grid the player
    // stands in, since a refused stile two tiles wide leaves the player within
    // any distance slack. One that missed, like a failed agility jump into a
    // pit or a gate that turns the player away, is excluded and re-planned
    // around on a reroute budget of its own rather than the stuck-recovery
    // budget. A same-floor crossing whose loc was missing is walked on through
    // as an open door; when the walk after it stalls with the player still on
    // the near side, nothing was open (a gate that needs an item, a loc the
    // host cannot see from the row's origin), and the crossing is handled as
    // a missing loc: excluded with its loc's rows and re-planned around.
    //
    // The pool borrow is held across the loop so re-plans reuse the same
    // SearchContext without re-entering the blocking acquire path.
    //
    // `disabledMoves` (WW_MOVE_* bits) keeps every plan of the run off those
    // movement categories.
    //
    // Stride, handoff radius and idle are drawn from a generator the Executor
    // owns and seeds at every run() entry: from std::random_device, or from
    // `fixedSeed` when one is given, so a test replays the same walk exactly.
    // The planner stays deterministic; all of the randomness lives here.
    //
    // Non-copyable, non-movable (it holds references to the borrowed artifact,
    // pool, and callbacks — relocation would dangle them).
    class Executor
    {
    public:
        Executor(const format::ArtifactReader &reader,
                 runtime::ContextPool &pool,
                 const Callbacks &callbacks,
                 std::optional<uint32_t> fixedSeed = std::nullopt,
                 uint32_t disabledMoves = 0);

        // Wall-clock budget of a walk hop covering `pathTiles` path tiles:
        // a fixed base plus a per-tile allowance, so a long click is not
        // judged stuck while the player is still honestly walking it.
        static int64_t stuckDeadlineMs(int32_t pathTiles);

        Executor(const Executor &) = delete;
        Executor &operator=(const Executor &) = delete;
        Executor(Executor &&) = delete;
        Executor &operator=(Executor &&) = delete;

        // Block the caller until the run terminates.
        WwStatus run(WwGoal goal);

    private:
        // A same-floor crossing skipped as an open door: its transition and
        // where the player stood when it was skipped.
        struct SkippedCrossing
        {
            uint32_t transitionIndex{0};
            WwTile   from{};
        };

        // What one run knows between steps: the last sampled position, how
        // much of the re-plan budget is spent, and whether global teleports
        // were allowed when last checked, by tile and by combat state (so a
        // false→true flip after a step can be detected).
        struct RunState
        {
            WwTile  position{};
            int32_t replansUsed{0};
            int32_t reroutesUsed{0};
            bool    isTeleAllowedAtLastPlan{false};
            // Transitions ruled out for the rest of this run: one whose loc was
            // missing or that landed off course, with every other transition
            // from that loc and origin. Every (re-)plan excludes them.
            std::vector<uint32_t> excludedTransitions;
            // The same-floor crossing the previous step skipped because its
            // loc was missing, and the tile it was skipped from; only the step
            // right after the skip consults it (see isStalledBehindSkip()).
            std::optional<SkippedCrossing> skippedCrossing;
        };

        // What a step learned beyond its status: a Transition that did not put
        // the player across it (isOffCourse, see hasMissedLanding()), one that
        // failed because the host could not find its loc (isLocMissing), or a
        // same-floor crossing walked on through because its loc was missing
        // (isCrossingSkipped).
        struct StepReport
        {
            bool isOffCourse{false};
            bool isLocMissing{false};
            bool isCrossingSkipped{false};
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
                      std::span<const uint32_t> excludedTransitions,
                      runtime::SearchContext &context, runtime::Plan &outPlan);

        // Read the live value of every varbit in planVarbitIds and every item
        // id any transition requirement references, through the batched host
        // callbacks, into `snapshot`. readCapability cannot surface these — the host does not
        // know which ids matter — and without them every requirement-gated
        // teleport reads as locked and the planner only ever walks.
        void refreshRequirementValues();

        // Emit ReplanStarted, re-invoke the planner from io.position, and
        // refresh the teleport-allowed anchor. The caller has already spent
        // the budget the re-plan belongs to (stuck recovery and the teleport
        // flip share replansUsed; an off-course landing spends reroutesUsed).
        // The caller restarts its step cursor on Restarted; Arrived has
        // already emitted the Arrived event.
        ReplanOutcome replan(const WwGoal &goal, runtime::SearchContext &context,
                             int32_t stepIndex, RunState &io);

        // Whether the plan just made from `at` could seed global teleports:
        // the tile allows them and `snapshot` (the one that plan read) says
        // the player is out of combat.
        bool isTeleAllowedForPlan(const WwTile &at) const;

        // Whether a global teleport could be cast from `at` right now. Reads
        // the combat varbit live, and only when the tile allows teleports at
        // all, since otherwise the answer is no either way.
        bool isTeleAllowedLive(const WwTile &at) const;

        // How a local-origin transition's interact went: the action was
        // issued, the loc was absent on a same-floor crossing (a door already
        // open, walk on through), or it was absent on anything else.
        enum class LocInteract
        {
            Issued,
            SkippedOpenCrossing,
            Missing,
        };

        // Fire interact for tx's loc from its origin tile. The host answers
        // zero when no such loc sits near the origin. That is the normal case
        // for a door that is already open, but for a cave mouth, ladder or
        // long ride there is no other way across: walking on stalls, re-plans
        // onto the same edge and loops until the budget runs out. So only a
        // same-floor crossing may be skipped; anything else is retried a few
        // ticks (a loc can drop out of one scene snapshot) and then reported
        // Missing, which fails the transition.
        LocInteract interactWithLoc(const format::TransitionRecord &tx) const;

        // Emit the terminal Failed event carrying the step (and transition, or
        // -1) it failed on, and return Failed.
        WwStatus failRun(int32_t stepIndex, int32_t transitionIndex) const;

        // Map a re-plan outcome onto the step loop: true when the plan was
        // rebuilt and the cursor restarts at 0; false when the run is over,
        // with its terminal status (Arrived, or Failed with the event emitted)
        // in outStatus.
        bool isRestart(ReplanOutcome outcome, int32_t stepIndex, WwStatus &outStatus) const;

        // Append to ioExcluded every local transition that starts at `missing`'s
        // loc and origin tile. One loc can carry several rows (a map with a
        // destination per row), and none of them works once the loc is gone.
        void excludeTransitionsOfLoc(const format::TransitionRecord &missing,
                                     std::vector<uint32_t> &ioExcluded) const;

        // The transition at `transitionIndex` failed because its loc is
        // missing: exclude it, and every row sharing its loc, for the rest of
        // the run and re-plan from the live position, spending one reroute.
        // True when a new plan exists and the cursor restarts at 0; false when
        // the run is over, with Arrived or Failed (on that transition, event
        // emitted) in outStatus.
        bool rerouteAroundMissingLoc(uint32_t transitionIndex, const WwGoal &goal,
                                     runtime::SearchContext &context, int32_t stepIndex,
                                     RunState &io, WwStatus &outStatus);

        // The transition at `transitionIndex` landed the player away from its
        // destination (a map whose destination was never picked, a refused
        // teleport, a failed agility jump). Spend one reroute, exclude it (and
        // its loc's other rows, when it has a loc) and re-plan from the live
        // position, so a transport that never lands is not chosen again. When
        // nothing else reaches the goal, the exclusion is dropped and the same
        // transition is planned again: a failable shortcut to an island is
        // still worth a retry, and the reroute budget still bounds it.
        // Returns as rerouteAroundMissingLoc does.
        bool rerouteAroundOffCourse(uint32_t transitionIndex, const WwGoal &goal,
                                    runtime::SearchContext &context, int32_t stepIndex,
                                    RunState &io, WwStatus &outStatus);

        // Drive plan.steps[i]: a Walk hop of `hopTiles` path tiles with the
        // handoff its successor demands, or a Transition. Writes the final
        // live position and what the step learned (see StepReport). `view` is
        // the run's context view, which a Transition's landing is judged
        // against.
        WwStatus executeStep(std::size_t i, int32_t hopTiles, const WwGoal &goal,
                             runtime::WorldView &view, WwTile &outPosition,
                             StepReport &outReport);

        // One walk click: the index of the Walk step it targets, and the path
        // tiles from the player to that target through every step it passes
        // over (the Chebyshev hop between consecutive targets stands in for a
        // step's length, which the plan does not carry).
        struct Hop
        {
            std::size_t last{0};
            int32_t     pathTiles{0};
        };

        // The hop to click from `from` when plan.steps[first] is a Walk: the
        // furthest consecutive Walk from `first` on whose path length the
        // stride drawn for it (drawStride) still holds, stopping at a
        // Transition (its approach walk is always clicked itself), at
        // kMaxClickChebyshev of `from`, and, walking out of a tile that
        // allows no teleport, at the first target that does (so the
        // post-step re-plan onto a teleport fires where it did when every
        // step was clicked). Only `first` when any target the hop would pass
        // is near the Wilderness fence (isNearWildernessFence). Always at
        // least `first`.
        Hop chooseHop(std::size_t first, const WwTile &from, const WwGoal &goal);

        // Desired stride of one hop in path tiles, from a mixture that keeps
        // most clicks medium, some long and a few very long.
        int32_t drawStride();

        // Whether the planner fences this run's walks off the Wilderness: a
        // player outside it walking to a goal outside it (the same rule as
        // PathAssembler::decideWilderness, judged at `from`).
        bool isWildernessFenced(const WwTile &from, const WwGoal &goal) const;

        // Whether `at` lies within kWildernessMargin of any Wilderness box on
        // its plane. A click there hands the route to the game's pathfinder,
        // which may cut through the fenced tiles.
        bool isNearWildernessFence(const WwTile &at) const;

        // How a Walk hop hands over to what comes next: the Chebyshev radius
        // at which it counts as done, and the ticks to idle after it.
        struct Handoff
        {
            int32_t radius{0};
            int32_t idleTicks{0};
        };

        // The handoff of the Walk hop ending at step i. When another Walk
        // follows, a random radius in [kHandoffMinChebyshev,
        // kHandoffMaxChebyshev] (the next click fires while the avatar is
        // still moving, at a point that never repeats), or, one hop in
        // kFullStopPercent, a full stop: arrive within kArrivalChebyshev and
        // idle up to kFullStopMaxIdleTicks. Otherwise no idle, and 0 for the
        // final walk of a radius-0 goal (the ARRIVED contract demands the
        // exact tile, not its neighbour), else kArrivalChebyshev.
        Handoff handoffFor(std::size_t i, const WwGoal &goal);

        // The plan drained without an in-loop arrival: the assembler's final
        // step targets the acceptance set, but the walk hands back at its
        // arrival radius, which can be a tile short of the goal test. Judge
        // the live position (after one settling tick) rather than assuming
        // the drain implies arrival.
        WwStatus judgeDrainedRun(const WwGoal &goal, runtime::WorldView &view,
                                 WwTile &ioPosition);

        // The goal tile cannot be stood on (a bartender's spawn behind the
        // counter, an object's footprint), so the planner ended the route on
        // the nearest standable tile instead, and the player is on it. The
        // goal test alone can never pass there: the Rusty Anchor's walk to
        // 3050,3257 reached the stand-in 3050,3256 and was judged FAILED. The
        // same holds for a standable goal in a sealed pocket (East Ardougne's
        // banker tile 2614,3330), which the planner moves across the booth.
        bool isAtGoalStandIn(const WwGoal &goal, runtime::WorldView &view,
                             const WwTile &at) const;

        // Drive one Walk hop to its target. Issues walkTo, then alternates
        // shouldCancel / sleepTicks / readPosition until arrival, cancellation,
        // a stall trip (the player's tile unchanged, not the distance to the
        // target: a winding hop moves away from its target for a while), or
        // the stuck deadline for `pathTiles` (stuckDeadlineMs). Emits
        // StepAdvanced (carrying `interactionHint`) once at entry, before the
        // walkTo, so the host knows the hint when the click arrives; emits
        // Stuck on either failure path.
        // Writes the final sampled position to outPosition so run() can drive
        // re-plan / teleport-allowed checks without a redundant readPosition.
        WwStatus walkOneStep(const runtime::Step &step, int32_t stepIndex,
                             int32_t arrivalRadius, int32_t pathTiles, int32_t interactionHint,
                             WwTile &outPosition);

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
                                       runtime::WorldView &view, WwTile &outPosition,
                                       StepReport &outReport);

        // After an issued local transition, poll a tick at a time until the
        // player lands near tx's destination or stands still elsewhere, within
        // a small budget. An agility obstacle keeps moving the player after
        // the click, and a walk clicked during that move is dropped by the
        // game; the next walk must start from where the player came to rest.
        //
        // Any plain chat page open meanwhile is continued (continueOpenChat).
        //
        // A same-floor crossing whose player has not moved at all after this
        // is clicked once more (retryUnmovedCrossing): a door like Draynor
        // Manor's walks the player through itself, and a walk clicked before
        // that starts cancels it.
        void awaitLanding(const format::TransitionRecord &tx, const WwTile &start,
                          WwTile &ioPosition);

        // A same-floor crossing that left the player on `start`: click it once
        // more, and if they still have not moved, walk to its destination. The
        // walk carries the player through a door that is already open but whose
        // hidden closed loc the host clicked to no effect; a shut one holds
        // them, and the caller's landing judgement refuses the crossing.
        void retryUnmovedCrossing(const format::TransitionRecord &tx, const WwTile &start,
                                  WwTile &ioPosition);

        // Deal with one open conversation page, if any, while the player is at
        // `at`: answer the option list inside a dialog zone, else continue a
        // plain chat page. Spends one of the run's dialog actions; false (and
        // nothing done) when nothing was open or the budget is gone. Called
        // from every walk poll and every landing wait.
        bool handleOpenDialog(const WwTile &at);

        // When the option list (1188) is open and `at` is inside a dialog zone,
        // send the host the zone's next answer to pick (a DialogueAnswer
        // runChainStep). Outside every zone it never picks anything.
        bool answerOptionList(const WwTile &at);

        // The first dialog zone with answers that holds `at`, or nullptr.
        const format::DialogZoneRecord *dialogZoneAt(const WwTile &at) const;

        // Continue the first open plain chat page (npc, player, paired chat,
        // message or item box) with a queued DIALOGUE action through
        // runChainStep; never an option list. True when one was continued.
        // A door can raise such a page before it lets the player through
        // (Draynor Manor), and until it is continued nothing moves.
        bool continueOpenChat() const;

        // True when `at` counts as having crossed tx from `start`: on the
        // destination tile, or moved off `start` to within a tile of it.
        static bool hasLanded(const format::TransitionRecord &tx, const WwTile &start,
                              const WwTile &at);

        static bool isSameTile(const WwTile &a, const WwTile &b);

        // True when `at` is too far from tx's destination (or on another plane)
        // for the transition to have been crossed: the player fell, or the
        // teleport was refused. A skipped open door stays within the slack.
        static bool isOffCourse(const format::TransitionRecord &tx, const WwTile &at);

        // What the baked area grid says about a local transition clicked from
        // `start` that left the player at `at`: Crossed when `at` is in the
        // destination's area, Refused when it is in any other area (the one
        // the click was made from, or a third the rest of the plan does not
        // walk). Unknown when the grid cannot tell: inside a dynamic region,
        // when start and destination share an area, or when one of the three
        // tiles is in no area (the player standing on the loc's own tile
        // mid-climb).
        enum class AreaVerdict
        {
            Crossed,
            Refused,
            Unknown,
        };
        static AreaVerdict judgeLandingArea(const format::TransitionRecord &tx,
                                            const WwTile &start, const WwTile &at,
                                            runtime::WorldView &view);

        // True when a same-floor crossing clicked from `start` left the player
        // at `at` still strictly nearer `start` than the destination: on the
        // near side of the loc. On the loc's own tile (equidistant) the
        // crossing is under way, so that is not the near side.
        static bool isOnNearSide(const format::TransitionRecord &tx, const WwTile &start,
                                 const WwTile &at);

        // After a same-floor crossing was skipped from `from` as an open door
        // and the walk beyond it stalled at `at`: whether the player is still
        // on the side they skipped it from, so the crossing was not open. By
        // area where the baked grid can tell (`at` in `from`'s area and not
        // the destination's), else by distance (isOnNearSide). A player who
        // got through and stalled further on is not held back by it.
        static bool isHeldBackBy(const format::TransitionRecord &tx, const WwTile &from,
                                 const WwTile &at, runtime::WorldView &view);

        // Whether `step`, which failed with the player at `at`, is a walk that
        // stalled right after `skipped` was skipped, held back by it: the
        // crossing was never open and is as missing as a loc the host cannot
        // find. False with no skip, and for a failed Transition.
        bool isStalledBehindSkip(const std::optional<SkippedCrossing> &skipped,
                                 const runtime::Step &step, const WwTile &at,
                                 runtime::WorldView &view) const;

        // Whether a transition that acted, clicked from `start`, did not put
        // the player across it at `at`. Off course by distance always counts
        // (isOffCourse); a global teleport is judged by nothing else. A local
        // transition is also judged by area (judgeLandingArea), because a
        // stile or gate that refuses the player leaves them within the
        // distance slack of a destination two tiles away, and without this
        // the run walked on into the wall until the stuck budget ran out.
        // Where the area grid cannot tell, a same-floor crossing falls back to
        // which side of it the player is on (isOnNearSide). This only ever
        // adds misses to the distance test; it never excuses one.
        static bool hasMissedLanding(const format::TransitionRecord &tx, const WwTile &start,
                                     const WwTile &at, runtime::WorldView &view);

        // Run every chain step of `tx` in order with a cancel poll between
        // steps. Arrived when the whole chain ran; the first non-Arrived
        // step result otherwise.
        WwStatus runChain(const format::TransitionRecord &tx) const;

        // Whether `tx` starts at an NPC rather than a loc: its chain opens
        // with a ClickNpc (a charter ship's crewmember). Such a transition has
        // no loc to interact with; its chain is the whole action, and a chain
        // that fails is its origin gone missing.
        bool hasNpcOrigin(const format::TransitionRecord &tx) const;

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
        // stepIndex / transitionIndex default to -1 to signal "not applicable";
        // interactionHint defaults to 0, "unknown" (see WwEvent).
        void emit(WwEventKind kind,
                  int32_t stepIndex = -1,
                  int32_t transitionIndex = -1,
                  int32_t interactionHint = 0) const;

        // WwEvent::interactionHint for a walk clicked at plan.steps[i]: 1 + the
        // path tiles from its target to the next Transition's approach step
        // (the last Walk before it) or to the plan's end, capped at
        // kMaxInteractionHint.
        int32_t interactionHintFor(std::size_t i) const;

        const format::ArtifactReader *artifact;
        runtime::ContextPool *pool;
        const Callbacks *callbacks;

        // Varbits read on every (re-)plan: the distinct ids any transition
        // requirement references (ArtifactReader::rebuildRequirementIdLists)
        // plus the combat varbit the teleport policy consults, which no
        // requirement names. Owned, because the artifact's list is not the
        // whole set; copying a few dozen ints per run costs nothing next to
        // the pipe round-trip they feed.
        std::vector<int32_t> planVarbitIds;

        // Distinct item ids referenced by any transition requirement, borrowed
        // from the artifact that outlives the Executor.
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

        // Dialog actions left in this run, and which answer of the current
        // zone the next option-list pick sends. Both reset at run() entry.
        int32_t dialogActionsLeft{0};
        std::size_t answerCursor{0};

        // The seed a test pinned, if any, and the generator every stride,
        // handoff and idle is drawn from; reseeded at run() entry.
        std::optional<uint32_t> fixedSeed;
        std::mt19937 rng;

        // Movement categories (format::MoveCategory bits) every plan and
        // re-plan of this run refuses; see ww_executor_run_ex.
        uint32_t disabledMoves{0};
    };
}

#endif  // WORLDWALKER_EXEC_EXECUTOR_H
