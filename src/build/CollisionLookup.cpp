#include "build/CollisionLookup.h"

#include "format/Artifact.h"
#include "format/ClipFlags.h"

namespace ww::build
{
    namespace
    {
        uint32_t squareKey(int squareX, int squareY)
        {
            return (static_cast<uint32_t>(squareX & 0xFFFF) << 16)
                 | static_cast<uint32_t>(squareY & 0xFFFF);
        }
    }

    CollisionLookup::CollisionLookup(const CollisionModel &collisionModel)
        : model(collisionModel)
    {
        squareIndex.reserve(model.squares.size() * 2 + 1);
        for (std::size_t i = 0; i < model.squares.size(); ++i)
        {
            const SquareClip &sq = model.squares[i];
            squareIndex.emplace(squareKey(sq.squareX, sq.squareY), i);
        }
    }

    const std::vector<uint32_t> &CollisionLookup::squareWords(int squareX, int squareY) const
    {
        if (lastWords != nullptr && squareX == lastSquareX && squareY == lastSquareY)
        {
            return *lastWords;
        }
        const auto it = squareIndex.find(squareKey(squareX, squareY));
        const std::vector<uint32_t> *words =
            (it == squareIndex.end()) ? &absentWords : &model.squares[it->second].words;
        lastSquareX = squareX;
        lastSquareY = squareY;
        lastWords = words;
        return *words;
    }

    uint32_t CollisionLookup::clipAt(int worldX, int worldY, int plane) const
    {
        if (plane < 0 || plane >= format::kClipPlanes)
        {
            return static_cast<uint32_t>(format::CLIP_BLOCKED);
        }
        const std::vector<uint32_t> &words = squareWords(worldX >> 6, worldY >> 6);
        const std::size_t planeBase =
            static_cast<std::size_t>(plane) * format::kClipSize * format::kClipSize;
        const std::size_t local = static_cast<std::size_t>(worldX & 63) * format::kClipSize
                                + static_cast<std::size_t>(worldY & 63);
        const std::size_t idx = planeBase + local;
        // An absent square holds no words at all, so this also covers the miss.
        if (idx >= words.size())
        {
            return static_cast<uint32_t>(format::CLIP_BLOCKED);
        }
        return words[idx];
    }

    bool CollisionLookup::isWalkable(int worldX, int worldY, int plane) const
    {
        return (clipAt(worldX, worldY, plane) & format::kClipStandBlockedMask) == 0u;
    }
}
