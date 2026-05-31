#include "runtime/ContextPool.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>

namespace ww::runtime
{
    ContextPool::ContextPool(const format::ArtifactReader &reader, std::size_t count)
    {
        const std::size_t n = std::max<std::size_t>(count, 1u);
        contexts.reserve(n);
        freeList.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            contexts.push_back(std::make_unique<SearchContext>(reader));
            freeList.push_back(contexts.back().get());
        }
    }

    SearchContext &ContextPool::acquire()
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [this]() { return !freeList.empty(); });
        SearchContext *ctx = freeList.back();
        freeList.pop_back();
        return *ctx;
    }

    bool ContextPool::tryAcquire(SearchContext *&outContext)
    {
        std::unique_lock<std::mutex> lock(m);
        if (freeList.empty())
        {
            return false;
        }
        outContext = freeList.back();
        freeList.pop_back();
        return true;
    }

    // Recycle outside the lock so the (potentially non-trivial) cache clear does
    // not serialize concurrent acquires; the caller is the sole holder of the
    // context until it lands back on the free list.
    void ContextPool::release(SearchContext &context)
    {
        context.recycle();
        {
            std::unique_lock<std::mutex> lock(m);
            freeList.push_back(&context);
        }
        cv.notify_one();
    }

    std::size_t ContextPool::freeCount() const
    {
        std::unique_lock<std::mutex> lock(m);
        return freeList.size();
    }
}
