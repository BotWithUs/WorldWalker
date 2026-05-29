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

    bool CollisionLookup::isWalkable(int worldX, int worldY, int plane) const
    {
        if (plane < 0 || plane >= format::kClipPlanes)
        {
            return false;
        }
        const auto it = squareIndex.find(squareKey(worldX >> 6, worldY >> 6));
        if (it == squareIndex.end())
        {
            return false;
        }
        const SquareClip &sq = model.squares[it->second];
        const std::size_t planeBase =
            static_cast<std::size_t>(plane) * format::kClipSize * format::kClipSize;
        const std::size_t local = static_cast<std::size_t>(worldX & 63) * format::kClipSize
                                + static_cast<std::size_t>(worldY & 63);
        const std::size_t idx = planeBase + local;
        if (idx >= sq.words.size())
        {
            return false;
        }
        return (sq.words[idx] & format::kClipStandBlockedMask) == 0u;
    }
}
