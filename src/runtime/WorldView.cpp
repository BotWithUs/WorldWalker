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
        constexpr int kSquareShift = 6;                  // 64 tiles per square edge
        constexpr int kLocalMask = format::kClipSize - 1;

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

        // Clip words are plane-major, then x (west-east), then y (south-north).
        std::size_t clipIndex(int x, int y, int plane)
        {
            const std::size_t lx = static_cast<std::size_t>(x & kLocalMask);
            const std::size_t ly = static_cast<std::size_t>(y & kLocalMask);
            return (static_cast<std::size_t>(plane) * format::kClipSize + lx) * format::kClipSize + ly;
        }

        // Area-id grids are x-major, then y, over a single (square, plane).
        std::size_t gridIndex(int x, int y)
        {
            const std::size_t lx = static_cast<std::size_t>(x & kLocalMask);
            const std::size_t ly = static_cast<std::size_t>(y & kLocalMask);
            return lx * static_cast<std::size_t>(format::kClipSize) + ly;
        }

        bool offWorld(int x, int y, int plane)
        {
            return x < 0 || y < 0 || plane < 0 || plane >= format::kClipPlanes;
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

    uint32_t WorldView::clipAt(int x, int y, int plane)
    {
        if (offWorld(x, y, plane))
        {
            return kBlockedWord;
        }
        const int sqX = x >> kSquareShift;
        const int sqY = y >> kSquareShift;
        // Sticky one-slot fast path — spatially-coherent A* almost always asks
        // for the same square the previous call did, so the equality compare
        // skips the unordered_map::find. Pointer doubles as the valid flag.
        const std::vector<uint32_t> *words = lastClipWords;
        if (sqX != lastClipSquareX || sqY != lastClipSquareY || words == nullptr)
        {
            words = &squareWords(sqX, sqY);
            lastClipSquareX = sqX;
            lastClipSquareY = sqY;
            lastClipWords = words;
        }
        if (words->empty())
        {
            return kBlockedWord;
        }
        return (*words)[clipIndex(x, y, plane)];
    }

    int32_t WorldView::areaAt(int x, int y, int plane)
    {
        if (offWorld(x, y, plane))
        {
            return -1;
        }
        const int sqX = x >> kSquareShift;
        const int sqY = y >> kSquareShift;
        // Sticky one-slot fast path. Plane is part of the grid key because
        // each (square, plane) is a distinct inflated buffer.
        const std::vector<int32_t> *ids = lastGridIds;
        if (sqX != lastGridSquareX || sqY != lastGridSquareY || plane != lastGridPlane
            || ids == nullptr)
        {
            ids = &gridIds(sqX, sqY, plane);
            lastGridSquareX = sqX;
            lastGridSquareY = sqY;
            lastGridPlane = plane;
            lastGridIds = ids;
        }
        if (ids->empty())
        {
            return -1;
        }
        return (*ids)[gridIndex(x, y)];
    }

    void WorldView::clearCache()
    {
        clipCache.clear();
        gridCache.clear();
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
    }
}
