#include "runtime/WorldView.h"

#include "format/Artifact.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ww::runtime
{
    namespace
    {
        // Non-overlapping 16/16 packing — squareX up to 65535, squareY up to
        // 65535, no aliasing.
        uint64_t squareKey(int squareX, int squareY)
        {
            return (static_cast<uint64_t>(static_cast<uint32_t>(squareY)) << 32)
                 |  static_cast<uint64_t>(static_cast<uint32_t>(squareX));
        }

        // Non-overlapping 16/16/8 packing — squareX up to 65535, squareY up to
        // 65535, plane up to 255, no aliasing under any RS3 cache size.
        uint64_t gridKey(int squareX, int squareY, int plane)
        {
            return (static_cast<uint64_t>(static_cast<uint32_t>(squareY)) << 40)
                 | (static_cast<uint64_t>(static_cast<uint32_t>(squareX)) << 8)
                 | (static_cast<uint64_t>(plane) & 0xFFu);
        }
    }

    WorldView::WorldView(const format::ArtifactReader &reader)
        : artifact(&reader)
    {
    }

    const std::vector<uint32_t> &WorldView::squareWords(int squareX, int squareY)
    {
        const uint64_t key = squareKey(squareX, squareY);
        const auto it = clipCache.find(key);
        if (it != clipCache.end())
        {
            return it->second;
        }
        std::vector<uint32_t> words;
        artifact->decompressSquare(squareX, squareY, words);  // leaves words empty when absent
        return clipCache.emplace(key, std::move(words)).first->second;
    }

    const std::vector<int32_t> &WorldView::gridIds(int squareX, int squareY, int plane)
    {
        const uint64_t key = gridKey(squareX, squareY, plane);
        const auto it = gridCache.find(key);
        if (it != gridCache.end())
        {
            return it->second;
        }
        std::vector<int32_t> ids;
        artifact->decompressGrid(squareX, squareY, plane, ids);  // leaves ids empty when absent
        return gridCache.emplace(key, std::move(ids)).first->second;
    }

    std::vector<uint32_t> *WorldView::stampsLookup(int squareX, int squareY, int plane)
    {
        const auto it = visitedStamps.find(gridKey(squareX, squareY, plane));
        if (it == visitedStamps.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    std::vector<uint32_t> &WorldView::stampsEnsure(int squareX, int squareY, int plane)
    {
        const uint64_t key = gridKey(squareX, squareY, plane);
        const auto it = visitedStamps.find(key);
        if (it != visitedStamps.end())
        {
            return it->second;
        }
        // One uint32 per tile on a 64x64 (square, plane). Lazy allocation
        // means an artifact with thousands of squares only pays for the few
        // its searches actually touch.
        std::vector<uint32_t> grid(static_cast<std::size_t>(format::kClipSize) * format::kClipSize, 0u);
        return visitedStamps.emplace(key, std::move(grid)).first->second;
    }

    std::size_t WorldView::cacheBytes() const
    {
        constexpr std::size_t kTilesPerGrid = static_cast<std::size_t>(format::kClipSize) * format::kClipSize;
        const std::size_t clipBytes = clipCache.size() * format::kClipWordsPerSquare * sizeof(uint32_t);
        const std::size_t gridBytes = gridCache.size() * kTilesPerGrid * sizeof(int32_t);
        const std::size_t stampBytes = visitedStamps.size() * kTilesPerGrid * sizeof(uint32_t);
        return clipBytes + gridBytes + stampBytes;
    }

    void WorldView::trimCache()
    {
        if (cacheBytes() > kCacheBudgetBytes)
        {
            clearCache();
        }
    }

    void WorldView::clearCache()
    {
        clipCache.clear();
        gridCache.clear();
        visitedStamps.clear();
        tileEpoch = 0u;
        // Sticky pointers reference the dropped vectors; null them so the next
        // borrower's first query reloads from the (now-empty) maps and refills
        // the sticky slot rather than dereferencing a stale pointer.
        lastClipSquareX = INT_MIN;
        lastClipSquareY = INT_MIN;
        lastClipWords = nullptr;
        lastGridSquareX = INT_MIN;
        lastGridSquareY = INT_MIN;
        lastGridPlane = INT_MIN;
        lastGridIds = nullptr;
        lastStampSquareX = INT_MIN;
        lastStampSquareY = INT_MIN;
        lastStampPlane = INT_MIN;
        lastStampGrid = nullptr;
    }
}
