#include "runtime/ContextPool.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>

namespace ww::runtime
{
    void ContextLease::reset() noexcept
    {
        if (pool != nullptr && context != nullptr)
        {
            pool->release(context);
        }
        pool = nullptr;
        context = nullptr;
    }

    ContextPool::ContextPool(const format::ArtifactReader &reader, std::size_t count)
    {
        const std::size_t n = std::max<std::size_t>(count, 1u);
        contexts.reserve(n);
        freeList.reserve(n);
        outstanding.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            contexts.push_back(std::make_unique<SearchContext>(reader));
            freeList.push_back(contexts.back().get());
        }
    }

    ContextLease ContextPool::acquire()
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [this]() { return !freeList.empty(); });
        SearchContext *ctx = freeList.back();
        freeList.pop_back();
        outstanding.insert(ctx);
        return ContextLease(this, ctx);
    }

    ContextLease ContextPool::tryAcquire()
    {
        std::unique_lock<std::mutex> lock(m);
        if (freeList.empty())
        {
            return ContextLease();
        }
        SearchContext *ctx = freeList.back();
        freeList.pop_back();
        outstanding.insert(ctx);
        return ContextLease(this, ctx);
    }

    // Called from the ContextLease destructor (or move-assignment). Two
    // defenses ride on the outstanding set:
    //   1. A context not in `outstanding` was never acquired through us (or has
    //      already been released): drop silently rather than double-listing.
    //   2. recycle() runs OUTSIDE the lock so the (potentially expensive)
    //      cache clear does not serialize concurrent acquires. If it throws
    //      (e.g., allocator failure during the unordered_map clear), swallow
    //      the exception and re-list anyway — the next borrower will reset
    //      whatever scratch remains. Leaking the context would deadlock the
    //      pool once enough leaks accumulate.
    void ContextPool::release(SearchContext *context) noexcept
    {
        if (context == nullptr)
        {
            return;
        }
        {
            std::unique_lock<std::mutex> lock(m);
            const auto it = outstanding.find(context);
            if (it == outstanding.end())
            {
                return;  // stale / double-release
            }
            outstanding.erase(it);
        }
        try
        {
            context->recycle();
        }
        catch (...)
        {
            // Best-effort: re-list anyway so the slot is not lost.
        }
        {
            std::unique_lock<std::mutex> lock(m);
            freeList.push_back(context);
        }
        cv.notify_one();
    }

    std::size_t ContextPool::freeCount() const
    {
        std::unique_lock<std::mutex> lock(m);
        return freeList.size();
    }
}
