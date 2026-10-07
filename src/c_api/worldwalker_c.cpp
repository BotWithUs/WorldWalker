#include "worldwalker_c.h"

#include "data/Transitions.h"
#include "exec/Callbacks.h"
#include "exec/Executor.h"
#include "format/ArtifactReader.h"
#include "format/MoveCategory.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/ContextPool.h"
#include "runtime/PathAssembler.h"
#include "runtime/RuntimeTeleports.h"
#include "runtime/SearchContext.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

// WwStep on the wire must be byte-for-byte identical to ww::runtime::Step so
// the query path can memcpy across the FFI boundary without re-packing. The
// enum-class StepKind underlying type is uint8_t (matches WwStep::kind on the
// wire) and the C ABI sentinels are pinned to the C++ enumerators below.
static_assert(sizeof(WwStep) == sizeof(ww::runtime::Step),
              "WwStep and ww::runtime::Step must have identical size");
static_assert(sizeof(WwStep) == 16, "WwStep must be 16 bytes (wire layout)");
static_assert(static_cast<uint8_t>(ww::runtime::StepKind::Walk)       == WW_STEP_KIND_WALK,
              "StepKind::Walk must match WW_STEP_KIND_WALK");
static_assert(static_cast<uint8_t>(ww::runtime::StepKind::Transition) == WW_STEP_KIND_TRANSITION,
              "StepKind::Transition must match WW_STEP_KIND_TRANSITION");

// The WW_MOVE_* bit numbers are format::MoveCategory's values.
static_assert(WW_MOVE_DOORS         == static_cast<uint32_t>(ww::format::MoveCategory::Doors));
static_assert(WW_MOVE_SHORTCUTS     == static_cast<uint32_t>(ww::format::MoveCategory::Shortcuts));
static_assert(WW_MOVE_PLANE         == static_cast<uint32_t>(ww::format::MoveCategory::Plane));
static_assert(WW_MOVE_CLIMBOVERS    == static_cast<uint32_t>(ww::format::MoveCategory::ClimbOvers));
static_assert(WW_MOVE_TRANSPORTS    == static_cast<uint32_t>(ww::format::MoveCategory::Transports));
static_assert(WW_MOVE_TELEPORTS     == static_cast<uint32_t>(ww::format::MoveCategory::Teleports));
static_assert(WW_MOVE_LODESTONES    == static_cast<uint32_t>(ww::format::MoveCategory::Lodestones));
static_assert(WW_MOVE_FAIRY_RINGS   == static_cast<uint32_t>(ww::format::MoveCategory::FairyRings));
static_assert(WW_MOVE_SPIRIT_TREES  == static_cast<uint32_t>(ww::format::MoveCategory::SpiritTrees));
static_assert(WW_MOVE_GLIDERS       == static_cast<uint32_t>(ww::format::MoveCategory::Gliders));
static_assert(WW_MOVE_CHARTERS      == static_cast<uint32_t>(ww::format::MoveCategory::Charters));
static_assert(WW_MOVE_MAGIC_CARPETS == static_cast<uint32_t>(ww::format::MoveCategory::MagicCarpets));
static_assert(WW_MOVE_OTHER_CHAINS  == static_cast<uint32_t>(ww::format::MoveCategory::OtherChains));
static_assert(WW_MOVE_COUNT == ww::format::kMoveCategoryCount);

// ww_transition_describe hands out the internal enums' values as they are.
static_assert(WW_TRANSITION_KIND_TRANSPORT      == static_cast<int>(ww::data::TransitionKind::Transport));
static_assert(WW_TRANSITION_KIND_FAIRY_RING     == static_cast<int>(ww::data::TransitionKind::FairyRing));
static_assert(WW_TRANSITION_KIND_TELEPORT_CHAIN == static_cast<int>(ww::data::TransitionKind::TeleportChain));
static_assert(WW_TRANSITION_KIND_SPELL          == static_cast<int>(ww::data::TransitionKind::Spell));
static_assert(WW_TRANSITION_KIND_LODESTONE      == static_cast<int>(ww::data::TransitionKind::Lodestone));
static_assert(WW_TRANSITION_KIND_ITEM_TELEPORT  == static_cast<int>(ww::data::TransitionKind::ItemTeleport));
static_assert(WW_REQUIREMENT_SKILL           == static_cast<int>(ww::data::RequirementKind::Skill));
static_assert(WW_REQUIREMENT_ITEM            == static_cast<int>(ww::data::RequirementKind::Item));
static_assert(WW_REQUIREMENT_VARBIT          == static_cast<int>(ww::data::RequirementKind::Varbit));
static_assert(WW_REQUIREMENT_VARP            == static_cast<int>(ww::data::RequirementKind::Varp));
static_assert(WW_REQUIREMENT_VARBIT_AT_LEAST == static_cast<int>(ww::data::RequirementKind::VarbitAtLeast));
static_assert(WW_REQUIREMENT_VARP_AT_LEAST   == static_cast<int>(ww::data::RequirementKind::VarpAtLeast));
static_assert(WW_REQUIREMENT_VARP_BIT        == static_cast<int>(ww::data::RequirementKind::VarpBit));
static_assert(WW_CHAIN_STEP_CLICK           == static_cast<int>(ww::data::ChainStepKind::Click));
static_assert(WW_CHAIN_STEP_WAIT            == static_cast<int>(ww::data::ChainStepKind::Wait));
static_assert(WW_CHAIN_STEP_WAIT_INTERFACE  == static_cast<int>(ww::data::ChainStepKind::WaitInterface));
static_assert(WW_CHAIN_STEP_DIALOGUE_SELECT == static_cast<int>(ww::data::ChainStepKind::DialogueSelect));
static_assert(WW_CHAIN_STEP_CLICK_ITEM      == static_cast<int>(ww::data::ChainStepKind::ClickItem));
static_assert(WW_CHAIN_STEP_DIALOGUE_ANSWER == static_cast<int>(ww::data::ChainStepKind::DialogueAnswer));
static_assert(WW_CHAIN_STEP_CLICK_NPC       == static_cast<int>(ww::data::ChainStepKind::ClickNpc));

// Backs the opaque ww_artifact handle with the loaded, validated artifact.
//
// The reader is immutable except for the runtime-teleport pools, which
// ww_artifact_load_teleports rewrites in place (and every in-flight query or
// executor run reads through spans into). The library protects that itself
// rather than trusting each host to: readers (ww_query, ww_executor_run) hold
// `lifecycle` shared for their whole call, the reload holds it exclusive. A
// reload therefore waits for running walks to finish, which is the only
// moment the spans they borrowed can safely be re-pointed.
struct ww_artifact
{
    explicit ww_artifact(const char *path) : reader(std::string(path))
    {
    }

    ww::format::ArtifactReader reader;
    mutable std::shared_mutex lifecycle;
};

// Backs the opaque ww_context_pool handle with the bounded search-context pool.
// The pool borrows the artifact's reader, so the caller is responsible for
// destroying the pool before closing its underlying artifact.
struct ww_context_pool
{
    ww_context_pool(const ww::format::ArtifactReader &reader, std::size_t count)
        : pool(reader, count)
    {
    }

    ww::runtime::ContextPool pool;
};

namespace
{
    thread_local std::string g_lastError;

    void setLastError(std::string message)
    {
        g_lastError = std::move(message);
    }

    // The artifacts whose lifecycle lock this thread holds shared for a
    // running ww_executor_run, innermost first: a list threaded through the
    // runs' own stack frames, so a callback that starts a nested walk needs no
    // allocation and the list unwinds in order. ww_transition_describe reads
    // it to tell "called from inside a run's callback" (lock already held on
    // this thread; taking it again would be a recursive shared acquire, which
    // deadlocks behind a queued reload) from every other call.
    struct HeldRun
    {
        const ww_artifact *artifact;
        const HeldRun     *outer;
    };

    thread_local const HeldRun *g_heldRuns = nullptr;

    class HeldRunScope
    {
    public:
        explicit HeldRunScope(const ww_artifact *artifact) : node{artifact, g_heldRuns}
        {
            g_heldRuns = &node;
        }

        ~HeldRunScope()
        {
            g_heldRuns = node.outer;
        }

        HeldRunScope(const HeldRunScope &) = delete;
        HeldRunScope &operator=(const HeldRunScope &) = delete;

    private:
        HeldRun node;
    };

    bool isHeldByThisThread(const ww_artifact *artifact)
    {
        for (const HeldRun *run = g_heldRuns; run != nullptr; run = run->outer)
        {
            if (run->artifact == artifact)
            {
                return true;
            }
        }
        return false;
    }

    bool opensWithNpc(const ww::format::TransitionRecord &tx,
                      std::span<const ww::format::ChainStepRecord> chain)
    {
        return tx.chainCount > 0
            && chain[tx.chainStart].kind
                   == static_cast<uint8_t>(ww::data::ChainStepKind::ClickNpc);
    }

    // The origin fields. Everything not set here keeps the -1 / zero that
    // describeInto put there for "does not apply".
    void describeOrigin(const ww::format::TransitionRecord &tx,
                        std::span<const ww::format::ChainStepRecord> chain,
                        WwTransitionInfo &outInfo)
    {
        if ((tx.flags & ww::format::kTransitionFlagGlobalOrigin) != 0u)
        {
            outInfo.origin = WW_TRANSITION_ORIGIN_GLOBAL;
            return;
        }
        outInfo.originTile = WwTile{tx.originX, tx.originY, static_cast<int32_t>(tx.originPlane)};
        if (opensWithNpc(tx, chain))
        {
            // ClickNpc: a=option, b..d=search centre, e=radius, f..g=type range.
            const ww::format::ChainStepRecord &npc = chain[tx.chainStart];
            outInfo.origin          = WW_TRANSITION_ORIGIN_NPC;
            outInfo.npcTypeMin      = npc.f;
            outInfo.npcTypeMax      = npc.g;
            outInfo.npcSearchRadius = npc.e;
            outInfo.optionIndex     = npc.a;
            return;
        }
        outInfo.origin      = WW_TRANSITION_ORIGIN_LOC;
        outInfo.objectId    = tx.objectId;
        outInfo.optionIndex = static_cast<int32_t>(tx.optionIndex);
        outInfo.shape       = tx.shape;
        outInfo.rotation    = tx.rotation;
    }

    // Requirements and chain, copied up to capacity. Entries past the copied
    // ones stay as describeInto zeroed them.
    void describeArrays(const ww::format::TransitionRecord &tx,
                        std::span<const ww::format::RequirementRecord> reqs,
                        std::span<const ww::format::ChainStepRecord> chain,
                        WwTransitionInfo &outInfo)
    {
        outInfo.requirementCount = tx.requirementCount;
        outInfo.chainStepCount   = tx.chainCount;
        const uint32_t reqCopy = std::min<uint32_t>(tx.requirementCount, WW_TRANSITION_MAX_REQUIREMENTS);
        for (uint32_t i = 0; i < reqCopy; ++i)
        {
            const ww::format::RequirementRecord &r = reqs[tx.requirementStart + i];
            outInfo.requirements[i] = WwRequirement{static_cast<int32_t>(r.kind), r.id, r.amount};
        }
        const uint32_t chainCopy = std::min<uint32_t>(tx.chainCount, WW_TRANSITION_MAX_CHAIN_STEPS);
        for (uint32_t i = 0; i < chainCopy; ++i)
        {
            const ww::format::ChainStepRecord &c = chain[tx.chainStart + i];
            outInfo.chain[i] = WwChainStep{static_cast<int32_t>(c.kind),
                                           c.a, c.b, c.c, c.d, c.e, c.f, c.g, c.h, c.i};
        }
    }

    // Caller holds the lifecycle lock shared (or is a run that does) and has
    // zeroed outInfo.
    ww_result describeInto(const ww::format::ArtifactReader &reader, uint32_t transitionIndex,
                           WwTransitionInfo &outInfo)
    {
        const auto txs   = reader.transitions();
        const auto reqs  = reader.requirements();
        const auto chain = reader.chainSteps();
        const auto moves = reader.moveCategories();
        if (transitionIndex >= txs.size())
        {
            setLastError("ww_transition_describe: transition index out of range");
            return WW_ERR_NOT_FOUND;
        }
        const ww::format::TransitionRecord &tx = txs[transitionIndex];
        const bool isInBounds =
            std::size_t{tx.requirementStart} + tx.requirementCount <= reqs.size()
            && std::size_t{tx.chainStart} + tx.chainCount <= chain.size()
            && transitionIndex < moves.size();
        if (!isInBounds)
        {
            setLastError("ww_transition_describe: record points outside its pools");
            return WW_ERR_INTERNAL;
        }

        outInfo.kind            = static_cast<int32_t>(tx.kind);
        outInfo.moveCategory    = static_cast<int32_t>(moves[transitionIndex]);
        outInfo.isRuntimeLoaded = transitionIndex >= reader.bakedTransitions() ? 1 : 0;
        outInfo.destTile        = WwTile{tx.destX, tx.destY, static_cast<int32_t>(tx.destPlane)};
        outInfo.objectId        = -1;
        outInfo.npcTypeMin      = -1;
        outInfo.npcTypeMax      = -1;
        outInfo.npcSearchRadius = -1;
        outInfo.optionIndex     = -1;
        std::memcpy(outInfo.code, tx.code, sizeof(outInfo.code));
        outInfo.cost = tx.cost;
        describeOrigin(tx, chain, outInfo);
        describeArrays(tx, reqs, chain, outInfo);
        return WW_OK;
    }
}

extern "C"
{

const char *ww_last_error(void)
{
    return g_lastError.c_str();
}

void ww_free(void *ptr)
{
    std::free(ptr);
}

ww_artifact *ww_artifact_open(const char *path)
{
    if (path == nullptr)
    {
        setLastError("ww_artifact_open: path is null");
        return nullptr;
    }
    try
    {
        return new ww_artifact(path);
    }
    catch (const std::exception &e)
    {
        setLastError(std::string("ww_artifact_open: ") + e.what());
        return nullptr;
    }
}

void ww_artifact_close(ww_artifact *artifact)
{
    delete artifact;
}

ww_result ww_artifact_load_teleports(ww_artifact *artifact, const char *dir)
{
    if (artifact == nullptr || dir == nullptr)
    {
        setLastError("ww_artifact_load_teleports: null artifact or dir");
        return WW_ERR_INVALID;
    }
    try
    {
        // Exclusive: blocks until every in-flight query / run has released
        // its shared hold, and keeps new ones out until the pools are stable.
        const std::unique_lock<std::shared_mutex> exclusive(artifact->lifecycle);
        std::vector<ww::data::ExcludedRow> excluded;
        ww::runtime::loadGlobalTeleportsInto(artifact->reader, std::string(dir), &excluded);
        // Set on every WW_OK, empty included: an earlier failure's text left
        // on this thread would otherwise read as rows this load excluded.
        const std::string report = ww::runtime::describeExcludedRows(excluded);
        setLastError(report.empty() ? std::string()
                                    : "ww_artifact_load_teleports: " + report);
        return WW_OK;
    }
    catch (const std::exception &e)
    {
        setLastError(std::string("ww_artifact_load_teleports: ") + e.what());
        return WW_ERR_INVALID;
    }
}

ww_context_pool *ww_context_pool_create(ww_artifact *artifact, size_t count)
{
    if (artifact == nullptr)
    {
        setLastError("ww_context_pool_create: artifact is null");
        return nullptr;
    }
    if (count == 0)
    {
        setLastError("ww_context_pool_create: count must be > 0");
        return nullptr;
    }
    try
    {
        return new ww_context_pool(artifact->reader, count);
    }
    catch (const std::exception &e)
    {
        setLastError(std::string("ww_context_pool_create: ") + e.what());
        return nullptr;
    }
}

void ww_context_pool_destroy(ww_context_pool *pool)
{
    delete pool;
}

int32_t ww_executor_run(ww_artifact      *artifact,
                         ww_context_pool *pool,
                         WwGoal           goal,
                         const WwCallbacks *callbacks)
{
    return ww_executor_run_ex(artifact, pool, goal, callbacks, 0u);
}

int32_t ww_executor_run_ex(ww_artifact       *artifact,
                            ww_context_pool   *pool,
                            WwGoal             goal,
                            const WwCallbacks *callbacks,
                            uint32_t           disabledMoves)
{
    if (artifact == nullptr)
    {
        setLastError("ww_executor_run: artifact is null");
        return WW_STATUS_FAILED;
    }
    if (pool == nullptr)
    {
        setLastError("ww_executor_run: pool is null");
        return WW_STATUS_FAILED;
    }
    if (callbacks == nullptr)
    {
        setLastError("ww_executor_run: callbacks is null");
        return WW_STATUS_FAILED;
    }
    // Every function pointer the executor actually calls must be provided;
    // onEvent is the lone optional. Keep this list in lock-step with what the
    // Executor dereferences: a pointer it calls but this guard skips is a
    // null-call crash mid-walk instead of a clean status (isItemWorn /
    // readItemCount fire on ClickItem chains). readInstance joined the required
    // set when it claimed the formerly-reserved readVarbit slot — unlike its
    // predecessor it has a call site, at every (re-)plan.
    if (callbacks->readPosition    == nullptr
     || callbacks->readCapability  == nullptr
     || callbacks->readInstance    == nullptr
     || callbacks->readVarbits     == nullptr
     || callbacks->readItemCounts  == nullptr
     || callbacks->readItemCount   == nullptr
     || callbacks->isItemWorn      == nullptr
     || callbacks->isInterfaceOpen == nullptr
     || callbacks->walkTo          == nullptr
     || callbacks->interact        == nullptr
     || callbacks->runChainStep    == nullptr
     || callbacks->sleepTicks      == nullptr
     || callbacks->shouldCancel    == nullptr)
    {
        setLastError("ww_executor_run: callbacks vtable missing a required function pointer");
        return WW_STATUS_FAILED;
    }
    try
    {
        // Shared for the whole walk: the Executor borrows requirement-id spans
        // and transition records from the reader for its entire run.
        const std::shared_lock<std::shared_mutex> shared(artifact->lifecycle);
        // Declared after the lock so it unwinds first: the thread is marked as
        // holding the lock only while it actually does.
        const HeldRunScope held(artifact);
        ww::exec::Executor executor(artifact->reader, pool->pool, *callbacks, std::nullopt,
                                    disabledMoves);
        return static_cast<int32_t>(executor.run(goal));
    }
    catch (const std::exception &e)
    {
        setLastError(std::string("ww_executor_run: ") + e.what());
        return WW_STATUS_FAILED;
    }
}

ww_result ww_query(ww_artifact                *artifact,
                    ww_context_pool            *pool,
                    WwTile                      start,
                    WwGoal                      goal,
                    const WwCapabilitySnapshot *capabilities,
                    WwPath                     *outPath)
{
    return ww_query_ex(artifact, pool, start, goal, capabilities, nullptr, outPath);
}

ww_result ww_query_ex(ww_artifact                *artifact,
                       ww_context_pool            *pool,
                       WwTile                      start,
                       WwGoal                      goal,
                       const WwCapabilitySnapshot *capabilities,
                       const WwInstanceChunks     *instance,
                       WwPath                     *outPath)
{
    return ww_query_moves(artifact, pool, start, goal, capabilities, instance, outPath, 0u);
}

ww_result ww_query_moves(ww_artifact                *artifact,
                          ww_context_pool            *pool,
                          WwTile                      start,
                          WwGoal                      goal,
                          const WwCapabilitySnapshot *capabilities,
                          const WwInstanceChunks     *instance,
                          WwPath                     *outPath,
                          uint32_t                    disabledMoves)
{
    if (outPath == nullptr)
    {
        setLastError("ww_query: outPath is null");
        return WW_ERR_INVALID;
    }
    // Always leave outPath in a clean post-call state on any error so a
    // caller that forgets to check the result code still sees a zero-step
    // path rather than uninitialised pointer bytes.
    *outPath = WwPath{};
    if (artifact == nullptr)
    {
        setLastError("ww_query: artifact is null");
        return WW_ERR_INVALID;
    }
    if (pool == nullptr)
    {
        setLastError("ww_query: pool is null");
        return WW_ERR_INVALID;
    }
    try
    {
        const std::shared_lock<std::shared_mutex> shared(artifact->lifecycle);
        ww::runtime::CapabilitySnapshot snapshot;
        if (capabilities != nullptr)
        {
            ww::exec::copyCapabilities(*capabilities, snapshot);
        }
        // No capabilities admits every gate. With moves disabled the planner
        // still needs a snapshot to carry the mask, so it gets one that
        // admits every gate the way a null one would; with none disabled the
        // call is exactly the old one.
        if (capabilities == nullptr && disabledMoves != 0u)
        {
            snapshot.admitEveryRequirement();
        }
        snapshot.disableMoves(disabledMoves, artifact->reader.moveCategories());
        const bool hasSnapshot = capabilities != nullptr || disabledMoves != 0u;
        const ww::runtime::CapabilitySnapshot *snapshotPtr = hasSnapshot ? &snapshot : nullptr;

        // RAII lease — released on scope exit even when assemble() throws. The
        // release path calls SearchContext::recycle(), which drops the instance
        // map, so the grid installed just below cannot leak into whatever query
        // borrows this context next.
        ww::runtime::ContextLease lease = pool->pool.acquire();
        ww::exec::installInstance(lease->instance, instance);
        ww::runtime::Plan plan;
        const bool ok = lease->assembler.assemble(start.x, start.y, start.plane,
                                                  goal.x, goal.y, goal.plane,
                                                  snapshotPtr, plan);

        if (!ok)
        {
            setLastError("ww_query: no route from start to goal");
            return WW_ERR_NOT_FOUND;
        }

        const size_t stepCount = plan.steps.size();
        WwStep *buffer = nullptr;
        if (stepCount > 0)
        {
            // memcpy is sound because static_asserts above pin WwStep and
            // ww::runtime::Step to identical size + layout.
            buffer = static_cast<WwStep *>(std::malloc(stepCount * sizeof(WwStep)));
            if (buffer == nullptr)
            {
                setLastError("ww_query: out of memory allocating path steps");
                return WW_ERR_INTERNAL;
            }
            std::memcpy(buffer, plan.steps.data(), stepCount * sizeof(WwStep));
        }
        outPath->steps     = buffer;
        outPath->stepCount = stepCount;
        outPath->cost      = plan.cost;
        outPath->pad       = 0;
        return WW_OK;
    }
    catch (const std::exception &e)
    {
        setLastError(std::string("ww_query: ") + e.what());
        return WW_ERR_INTERNAL;
    }
}

void ww_path_free(WwPath *path)
{
    if (path == nullptr)
    {
        return;
    }
    std::free(path->steps);
    *path = WwPath{};
}

ww_result ww_transition_describe(const ww_artifact *artifact,
                                 uint32_t           transitionIndex,
                                 WwTransitionInfo  *outInfo)
{
    if (outInfo == nullptr)
    {
        setLastError("ww_transition_describe: outInfo is null");
        return WW_ERR_INVALID;
    }
    // Zeroed up front: every error leaves it zero, and on success every array
    // entry past the copied ones stays zero (a documented guarantee).
    *outInfo = WwTransitionInfo{};
    if (artifact == nullptr)
    {
        setLastError("ww_transition_describe: artifact is null");
        return WW_ERR_INVALID;
    }
    try
    {
        ww_result result = WW_OK;
        if (isHeldByThisThread(artifact))
        {
            // Inside a run's callback: the run holds the lock shared on this
            // thread, so no reload can touch the pools until it returns.
            result = describeInto(artifact->reader, transitionIndex, *outInfo);
        }
        else
        {
            const std::shared_lock<std::shared_mutex> shared(artifact->lifecycle);
            result = describeInto(artifact->reader, transitionIndex, *outInfo);
        }
        if (result != WW_OK)
        {
            *outInfo = WwTransitionInfo{};
        }
        return result;
    }
    catch (const std::exception &e)
    {
        *outInfo = WwTransitionInfo{};
        setLastError(std::string("ww_transition_describe: ") + e.what());
        return WW_ERR_INTERNAL;
    }
}

}  // extern "C"
