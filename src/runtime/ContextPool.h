#ifndef WORLDWALKER_RUNTIME_CONTEXTPOOL_H
#define WORLDWALKER_RUNTIME_CONTEXTPOOL_H

#include "format/ArtifactReader.h"
#include "runtime/SearchContext.h"

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace ww::runtime
{
    // Bounded pool of reusable SearchContexts over a shared, immutable artifact
    // (ADR 0007). A query or executor run borrows one context for its duration
    // via acquire() and returns it via release(). When every context is in use,
    // acquire() blocks on a condition variable until one is returned — a virtual
    // thread therefore only blocks when CPU parallelism is already saturated, so
    // scratch memory stays flat as clients/scripts scale.
    //
    // The artifact is borrowed; it must outlive the pool, which in turn must
    // outlive every context borrowed from it. The contexts are heap-allocated
    // and never relocated, so a borrower may hold the returned reference for
    // the duration of its query without re-acquiring through the pool.
    class ContextPool
    {
    public:
        // Build `count` SearchContexts over the borrowed reader. A count of 0
        // is silently clamped to 1 so the pool always has at least one slot.
        ContextPool(const format::ArtifactReader &reader, std::size_t count);
        ~ContextPool() = default;

        ContextPool(const ContextPool &) = delete;
        ContextPool &operator=(const ContextPool &) = delete;
        ContextPool(ContextPool &&) = delete;
        ContextPool &operator=(ContextPool &&) = delete;

        // Borrow a SearchContext, blocking until one is available. The returned
        // reference must be returned exactly once via release(); double-release
        // or releasing a context from a different pool is undefined behavior.
        SearchContext &acquire();

        // Non-blocking variant: when a context is immediately available, writes
        // it to outContext and returns true; otherwise leaves outContext
        // unchanged and returns false.
        bool tryAcquire(SearchContext *&outContext);

        // Return a previously acquired context to the pool, waking one waiter.
        // The pool recycles the context's per-query cache before re-listing it.
        void release(SearchContext &context);

        // Total context count (does not change after construction).
        std::size_t size() const
        {
            return contexts.size();
        }

        // Currently-free context count. Mostly useful for self-checks; production
        // callers should rely on acquire() blocking rather than polling this.
        std::size_t freeCount() const;

    private:
        std::vector<std::unique_ptr<SearchContext>> contexts;
        std::vector<SearchContext *> freeList;
        mutable std::mutex m;
        std::condition_variable cv;
    };
}

#endif  // WORLDWALKER_RUNTIME_CONTEXTPOOL_H
