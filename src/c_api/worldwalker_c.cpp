#include "worldwalker_c.h"

#include "exec/Callbacks.h"
#include "exec/Executor.h"
#include "format/ArtifactReader.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/ContextPool.h"
#include "runtime/PathAssembler.h"
#include "runtime/RuntimeTeleports.h"
#include "runtime/SearchContext.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>

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
        ww::runtime::loadGlobalTeleportsInto(artifact->reader, std::string(dir));
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
        ww::exec::Executor executor(artifact->reader, pool->pool, *callbacks);
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
        const ww::runtime::CapabilitySnapshot *snapshotPtr =
            (capabilities != nullptr) ? &snapshot : nullptr;

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

}  // extern "C"
