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

        uint32_t squareKey(int squareX, int squareY)
        {
            return (static_cast<uint32_t>(squareY) << 16) | (static_cast<uint32_t>(squareX) & 0xFFFFu);
        }

        uint32_t gridKey(int squareX, int squareY, int plane)
        {
            return (static_cast<uint32_t>(squareY) << 16)
                 | ((static_cast<uint32_t>(squareX) & 0xFFFu) << 4)
                 | (static_cast<uint32_t>(plane) & 0xFu);
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
        const uint32_t key = squareKey(squareX, squareY);
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
        const uint32_t key = gridKey(squareX, squareY, plane);
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
        const std::vector<uint32_t> &words = squareWords(x >> kSquareShift, y >> kSquareShift);
        if (words.empty())
        {
            return kBlockedWord;
        }
        return words[clipIndex(x, y, plane)];
    }

    int32_t WorldView::areaAt(int x, int y, int plane)
    {
        if (offWorld(x, y, plane))
        {
            return -1;
        }
        const std::vector<int32_t> &ids = gridIds(x >> kSquareShift, y >> kSquareShift, plane);
        if (ids.empty())
        {
            return -1;
        }
        return ids[gridIndex(x, y)];
    }

    void WorldView::clearCache()
    {
        clipCache.clear();
        gridCache.clear();
    }
}
