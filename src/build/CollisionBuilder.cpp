#include "build/CollisionBuilder.h"

#include <algorithm>
#include <utility>

namespace ww::build
{
    namespace
    {
        constexpr int kMapIndex = 5;
    }

    CollisionModel buildCollisionModel(const CacheClient &cache, int *outSkipped)
    {
        CollisionModel model;
        int skipped = 0;

        std::vector<int> ids = cache.archiveIds(kMapIndex);
        model.squares.reserve(ids.size());
        for (int id : ids)
        {
            // Map archive id encodes its square: x = low 7 bits, y = the rest.
            const int squareX = id & 0x7F;
            const int squareY = id >> 7;
            SquareClip clip;
            if (!cache.mapSquareClip(squareX, squareY, clip))
            {
                ++skipped;
                continue;
            }
            model.squares.push_back(std::move(clip));
        }

        std::sort(model.squares.begin(), model.squares.end(),
                  [](const SquareClip &a, const SquareClip &b)
                  {
                      if (a.squareY != b.squareY)
                      {
                          return a.squareY < b.squareY;
                      }
                      return a.squareX < b.squareX;
                  });

        if (outSkipped != nullptr)
        {
            *outSkipped = skipped;
        }
        return model;
    }
}
