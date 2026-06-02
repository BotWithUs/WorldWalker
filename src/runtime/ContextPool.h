#ifndef WORLDWALKER_RUNTIME_CONTEXTPOOL_H
#define WORLDWALKER_RUNTIME_CONTEXTPOOL_H

#include "format/ArtifactReader.h"
#include "runtime/SearchContext.h"

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace ww::runtime
{
    class ContextPool;

    // Move-only RAII handle around one borrowed SearchContext. Releases the
    // borrow on destruction, so any throw between acquire and the use of the
    // context still returns it to the pool — closing the deadlock-on-leak
    // window the old "raw reference + explicit release()" API had. Default-
    // constructed leases hold no context and are no-ops on destruction.
    //
    // Not thread-safe; intended to be held by one stack frame for the
    // duration of a query / executor run, exactly like a unique_ptr.
    class ContextLease
    {
    public:
        ContextLease() noexcept : pool(nullptr), context(nullptr) {}
        ContextLease(ContextPool *pool, SearchContext *context) noexcept
            : pool(pool), context(context)
        {
        }
        ~ContextLease()
        {
            reset();
        }

        ContextLease(const ContextLease &) = delete;
        ContextLease &operator=(const ContextLease &) = delete;

        ContextLease(ContextLease &&other) noexcept
            : pool(other.pool), context(other.context)
        {
            other.pool = nullptr;
            other.context = nullptr;
        }

        ContextLease &operator=(ContextLease &&other) noexcept
        {
            if (this != &other)
            {
                reset();
                pool = other.pool;
                context = other.context;
                other.pool = nullptr;
                other.context = nullptr;
            }
            return *this;
        }

        // True while a context is held; false after reset / move-out.
        explicit operator bool() const noexcept
        {
            return context != nullptr;
        }

        SearchContext &operator*() const noexcept
        {
            return *context;
        }

        SearchContext *operator->() const noexcept
        {
            return context;
        }

        SearchContext *get() const noexcept
        {
            return context;
        }

        // Eagerly release back to the pool. Subsequent operator*/operator->
        // are undefined; explicit operator bool reports false.
        void reset() noexcept;

    private:
        ContextPool *pool;
        SearchContext *context;
    };

    // Bounded pool of reusable SearchContexts over a shared, immutable artifact
    // (ADR 0007). A query or executor run borrows one context for its duration
    // via acquire() and returns it implicitly when the returned ContextLease
    // is destroyed. When every context is in use, acquire() blocks on a
    // condition variable until one is returned — a virtual thread therefore
    // only blocks when CPU parallelism is already saturated, so scratch memory
    // stays flat as clients/scripts scale.
    //
    // The artifact is borrowed; it must outlive the pool, which in turn must
    // outlive every context borrowed from it. The contexts are heap-allocated
    // and never relocated, so a borrower may hold the returned lease for the
    // duration of its query without re-acquiring through the pool.
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
        // lease releases the borrow on destruction; move it to transfer
        // ownership.
        ContextLease acquire();

        // Non-blocking variant: when a context is immediately available, the
        // returned lease holds it; otherwise the returned lease is empty
        // (evaluates to false).
        ContextLease tryAcquire();

        // Total context count (does not change after construction).
        std::size_t size() const
        {
            return contexts.size();
        }

        // Currently-free context count. Mostly useful for self-checks; production
        // callers should rely on acquire() blocking rather than polling this.
        std::size_t freeCount() const;

    private:
        // The ContextLease destructor calls back into the pool to return its
        // borrowed context. Kept private so callers cannot construct a release
        // path that bypasses the outstanding-set bookkeeping.
        friend class ContextLease;
        void release(SearchContext *context) noexcept;

        std::vector<std::unique_ptr<SearchContext>> contexts;
        std::vector<SearchContext *> freeList;
        // Tracks contexts currently leased out. A release() that targets a
        // pointer not in this set is silently dropped — a double-release would
        // otherwise corrupt the freeList by listing the same context twice and
        // hand it out to two callers, trampling each other's scratch.
        std::unordered_set<SearchContext *> outstanding;
        mutable std::mutex m;
        std::condition_variable cv;
    };
}

#endif  // WORLDWALKER_RUNTIME_CONTEXTPOOL_H
