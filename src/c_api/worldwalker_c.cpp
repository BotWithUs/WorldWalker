#include "worldwalker_c.h"

#include "exec/Callbacks.h"
#include "exec/Executor.h"
#include "format/ArtifactReader.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/ContextPool.h"
#include "runtime/PathAssembler.h"
#include "runtime/SearchContext.h"

#include <cstdlib>
#include <cstring>
#include <exception>
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
struct ww_artifact
{
    explicit ww_artifact(const char *path) : reader(std::string(path))
    {
    }

    ww::format::ArtifactReader reader;
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

    // Apply each (id, value) entry of one capability run onto a runtime
    // CapabilitySnapshot via `apply`. A null/zero-length run is a no-op; a
    // null `entries` with non-zero count is the caller's bug and is treated
    // as empty here so a malformed snapshot can't dereference past null.
    template <typename ApplyFn>
    void applyCapabilityRun(const WwCapabilityEntry *entries, size_t count, ApplyFn apply)
    {
        if (entries == nullptr || count == 0)
        {
            return;
        }
        for (size_t i = 0; i < count; ++i)
        {
            apply(entries[i].id, entries[i].value);
        }
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
    // Every non-null function pointer documented as required must be provided;
    // onEvent is the lone optional. Detecting a missing entry here means the
    // crash inside the executor's tight loop is replaced with a clean status.
    if (callbacks->readPosition    == nullptr
     || callbacks->readCapability  == nullptr
     || callbacks->readVarbit      == nullptr
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
        ww::runtime::CapabilitySnapshot snapshot;
        if (capabilities != nullptr)
        {
            applyCapabilityRun(capabilities->skills, capabilities->skillCount,
                               [&](int32_t id, int32_t v) { snapshot.setSkillLevel(id, v); });
            applyCapabilityRun(capabilities->items, capabilities->itemCount,
                               [&](int32_t id, int32_t v) { snapshot.setItemCount(id, v); });
            applyCapabilityRun(capabilities->varbits, capabilities->varbitCount,
                               [&](int32_t id, int32_t v) { snapshot.setVarbit(id, v); });
            applyCapabilityRun(capabilities->varps, capabilities->varpCount,
                               [&](int32_t id, int32_t v) { snapshot.setVarp(id, v); });
        }
        const ww::runtime::CapabilitySnapshot *snapshotPtr =
            (capabilities != nullptr) ? &snapshot : nullptr;

        ww::runtime::SearchContext &context = pool->pool.acquire();
        ww::runtime::Plan plan;
        bool ok = false;
        try
        {
            ok = context.assembler.assemble(start.x, start.y, start.plane,
                                            goal.x, goal.y, goal.plane,
                                            snapshotPtr, plan);
        }
        catch (...)
        {
            pool->pool.release(context);
            throw;
        }
        pool->pool.release(context);

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
