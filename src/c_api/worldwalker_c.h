/*
 * WorldWalker C ABI — stable extern "C" surface intended for FFI consumers
 * (Java + Project Panama, .NET P/Invoke, Python ctypes, etc.).
 *
 * Conventions (mirroring NXTCacheLibrary's nxtcache_c.h)
 * -----------------------------------------------------
 *  - Lifetimes: opaque handles created by ww_artifact_open / ww_context_pool_create
 *    must be released with their matching _close / _destroy. Output buffers from
 *    getters are allocated by the library and released with ww_free.
 *  - Strings: char* outputs are NUL-terminated UTF-8.
 *  - Errors: functions returning ww_result return WW_OK (0) on success, non-zero
 *    on failure. Call ww_last_error() (thread-local) for a human-readable message.
 *    Functions returning a handle return NULL on failure (also sets last error).
 *  - Threading: the artifact is immutable and safe to share across threads. A
 *    single search context (borrowed from a pool) is NOT safe for concurrent use;
 *    the pool hands out one per query. ww_last_error()'s buffer is thread-local.
 *
 * The query and executor surfaces (ww_query, ww_executor_run, the callback
 * vtable and POD structs) land in Phase 5 — see docs/adr/0008, 0010 and the
 * implementation plan. This header currently covers lifecycle, versioning,
 * error reporting, and memory only.
 */

#ifndef WORLDWALKER_C_H
#define WORLDWALKER_C_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
  #if defined(WORLDWALKER_BUILDING)
    #define WW_API __declspec(dllexport)
  #else
    #define WW_API __declspec(dllimport)
  #endif
#else
  #define WW_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Baked-artifact format version. ww_artifact_open refuses a mapping whose
   header version does not match this — a schema-breaking change bumps it so
   the host fails loud rather than misreading bytes. */
#define WW_ARTIFACT_FORMAT_VERSION 1u

typedef struct ww_artifact      ww_artifact;
typedef struct ww_context_pool  ww_context_pool;
typedef int                     ww_result;

#define WW_OK            0
#define WW_ERR_INVALID   1   /* invalid argument or handle */
#define WW_ERR_NOT_FOUND 2   /* requested entry doesn't exist */
#define WW_ERR_VERSION   3   /* artifact format version mismatch */
#define WW_ERR_IO        4   /* file / mapping I/O failure */
#define WW_ERR_INTERNAL  5   /* unexpected internal failure */

/* ---- Error + memory ----------------------------------------------------- */

/* Last error message produced on this thread, or "" if none. Owned by the
   library — do not free. */
WW_API const char *ww_last_error(void);

/* Free a buffer returned by any getter. Safe to pass NULL. */
WW_API void ww_free(void *ptr);

/* ---- Artifact lifecycle ------------------------------------------------- */

/* Open (memory-map) a baked artifact from disk. Returns NULL on failure
   (call ww_last_error). The handle is immutable and may be shared across
   threads; release it with ww_artifact_close once no query is using it. */
WW_API ww_artifact *ww_artifact_open(const char *path);

/* Release an artifact handle. Safe to pass NULL. */
WW_API void ww_artifact_close(ww_artifact *artifact);

/* ---- Search-context pool ------------------------------------------------ */

/* Create a bounded pool of reusable search contexts over an artifact
   (typically sized ~= hardware concurrency). Each query/executor run borrows
   one context for its duration. Returns NULL on failure. */
WW_API ww_context_pool *ww_context_pool_create(ww_artifact *artifact, size_t count);

/* Destroy a context pool. Safe to pass NULL. Must outlive every in-flight
   query that borrowed from it. */
WW_API void ww_context_pool_destroy(ww_context_pool *pool);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* WORLDWALKER_C_H */
