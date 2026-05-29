#include "worldwalker_c.h"

#include "format/ArtifactReader.h"

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
    (void)artifact;
    (void)count;
    setLastError("ww_context_pool_create: not yet implemented (Phase 3)");
    return nullptr;
}

void ww_context_pool_destroy(ww_context_pool *pool)
{
    (void)pool;
}

}  // extern "C"
