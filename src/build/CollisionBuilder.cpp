#include "build/CollisionBuilder.h"

#include "build/TerrainFloor.h"

#include <algorithm>
#include <utility>

namespace ww::build
{
    namespace
    {
        constexpr int kMapIndex = 5;
    }

    CollisionBuildResult buildCollisionModel(const CacheClient &cache)
    {
        CollisionBuildResult result;
        CollisionModel &model = result.model;

        std::vector<int> ids = cache.archiveIds(kMapIndex);
        model.squares.reserve(ids.size());
        std::vector<Crossing> square;
        SquareTerrain terrain;
        FloorIndex floors;
        for (int id : ids)
        {
            int squareX = 0;
            int squareY = 0;
            if (!CacheClient::mapSquareOf(id, squareX, squareY))
            {
                ++result.skippedArchives;
                continue;
            }
            SquareClip clip;
            if (!cache.mapSquareClipAndCrossings(squareX, squareY, clip, square))
            {
                ++result.skippedArchives;
                continue;
            }
            readSquareTerrain(cache, squareX, squareY, terrain);
            floors.addSquare(squareX, squareY, terrain);
            model.squares.push_back(std::move(clip));
            result.crossings.insert(result.crossings.end(), square.begin(), square.end());
        }

        // After every square is in: a stretch of void runs across squares,
        // and a tile's floor can be painted in the square next to it.
        result.voidFence = fenceUnpaintedVoid(floors, model);

        // Squares are sorted for the serialized square table; the crossings
        // keep archive-enumeration order, which is what the transition
        // derivers (and the artifact's transition order) are built on.
        std::sort(model.squares.begin(), model.squares.end(),
                  [](const SquareClip &a, const SquareClip &b)
                  {
                      if (a.squareY != b.squareY)
                      {
                          return a.squareY < b.squareY;
                      }
                      return a.squareX < b.squareX;
                  });

        return result;
    }
}
