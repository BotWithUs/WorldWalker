#include "worldwalker_c.h"

#include <cstdlib>
#include <string>
#include <utility>

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
    setLastError(std::string("ww_artifact_open: artifact loading not yet implemented (Phase 3); path=")
                 + (path != nullptr ? path : "(null)"));
    return nullptr;
}

void ww_artifact_close(ww_artifact *artifact)
{
    (void)artifact;
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
