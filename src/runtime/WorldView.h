#ifndef WORLDWALKER_RUNTIME_WORLDVIEW_H
#define WORLDWALKER_RUNTIME_WORLDVIEW_H

#include "format/ArtifactReader.h"
#include "format/ClipFlags.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ww::runtime
{
    // Per-query, single-threaded view over the shared artifact's collision clip
    // words and area-id grids, addressed by absolute world tile (x, y, plane).
    //
    // The artifact stores both grids zlib-compressed per map square; this view
    // inflates a square (or area grid) on first touch and caches it, so the
    // tile-level search can read a clip word or area id by world coordinate
    // without re-inflating the same blob. The cache is the mutable per-query
    // working set: a WorldView is therefore NOT thread-safe, each search context
    // owns its own, and the referenced ArtifactReader must outlive the view.
    //
    // Tiles outside any baked map square read as fully blocked (clip CLIP_BLOCKED,
    // area id -1) — unmapped world is treated as solid.
    class WorldView
    {
    public:
        // Clip word returned for tiles the artifact does not bake.
        static constexpr uint32_t kBlockedWord = static_cast<uint32_t>(format::CLIP_BLOCKED);

        explicit WorldView(const format::ArtifactReader &reader);

        WorldView(const WorldView &) = delete;
        WorldView &operator=(const WorldView &) = delete;
        WorldView(WorldView &&) = default;
        WorldView &operator=(WorldView &&) = default;

        // Directional clip word for a world tile, or kBlockedWord when the tile is
        // off-plane, negative, or in a square the artifact does not bake.
        uint32_t clipAt(int x, int y, int plane);

        // True when a unit may occupy the tile (no whole-tile blocker). Wall-edge
        // bits block crossing, not standing, so they are ignored here.
        bool isStandable(int x, int y, int plane)
        {
            return (clipAt(x, y, plane) & format::kClipStandBlockedMask) == 0u;
        }

        // Area id of a world tile, or -1 when the tile is blocked / unreachable /
        // off-plane / in an unmapped square.
        int32_t areaAt(int x, int y, int plane);

        // Drop all cached squares and grids (e.g. when reusing a search context
        // for a new query).
        void clearCache();

    private:
        // Inflate-and-cache one map square's clip words / one (square, plane) area
        // grid. The returned vector is empty when that square/grid is not baked.
        const std::vector<uint32_t> &squareWords(int squareX, int squareY);
        const std::vector<int32_t> &gridIds(int squareX, int squareY, int plane);

        const format::ArtifactReader *artifact;
        std::unordered_map<uint32_t, std::vector<uint32_t>> clipCache;
        std::unordered_map<uint32_t, std::vector<int32_t>> gridCache;
    };
}

#endif  // WORLDWALKER_RUNTIME_WORLDVIEW_H
