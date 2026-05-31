#include "worldwalker_c.h"

#include "exec/Callbacks.h"
#include "exec/Executor.h"
#include "format/ArtifactReader.h"
#include "runtime/ContextPool.h"

#include <cstdlib>
#include <exception>
#include <string>
#include <utility>

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

}  // extern "C"
