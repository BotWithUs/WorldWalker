#ifndef WORLDWALKER_RUNTIME_WORLDVIEW_H
#define WORLDWALKER_RUNTIME_WORLDVIEW_H

#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "format/ClipFlags.h"

#include <algorithm>
#include <climits>
#include <cstddef>
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
    //
    // Cache lifetime: a WorldView's clip + grid caches survive across queries on
    // the same context. The cached squares are an immutable function of the
    // borrowed ArtifactReader, so re-using them across walks/queries is sound
    // and lets the next query land on a warm working set instead of re-inflating
    // every touched square. Phase 1 dropped the per-recycle wipe; call
    // clearCache() explicitly when the borrowed reader itself has been replaced
    // (e.g. an artifact reload).
    class WorldView
    {
    public:
        // Clip word returned for tiles the artifact does not bake.
        static constexpr uint32_t kBlockedWord = static_cast<uint32_t>(format::CLIP_BLOCKED);

        // RS3 mapsquare axis width — the engine addresses squares with a
        // 16-bit field, but baked content reaches squareY ~200 (the
        // northern landmass extends past row 128). 256 covers every
        // populated square without overshoot. Cache memory at 256: 65536
        // clip slots (~1.5MB of vector headers) + 262144 grid/stamp slots
        // (~12MB of vector headers, vectors empty until touched). Off-axis
        // tiles act as blocked.
        static constexpr std::size_t kSquaresPerAxis = 256;

        explicit WorldView(const format::ArtifactReader &reader);

        WorldView(const WorldView &) = delete;
        WorldView &operator=(const WorldView &) = delete;
        WorldView(WorldView &&) = default;
        WorldView &operator=(WorldView &&) = default;

        // Directional clip word for a world tile, or kBlockedWord when the tile is
        // off-plane, negative, or in a square the artifact does not bake.
        //
        // Header-inlined: the sticky one-slot fast path is the dominant inner-
        // loop step of tile A* (TileSearch::tryStep + expand). Inlining lets the
        // sticky compare + array index fold into the caller; the slow path
        // (squareWords) stays out-of-line so the inlined body is small.
        uint32_t clipAt(int x, int y, int plane)
        {
            if (offWorld(x, y, plane))
            {
                return kBlockedWord;
            }
            const int sqX = x >> kSquareShift;
            const int sqY = y >> kSquareShift;
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

        // True when a unit may occupy the tile (no whole-tile blocker). Wall-edge
        // bits block crossing, not standing, so they are ignored here.
        bool isStandable(int x, int y, int plane)
        {
            return (clipAt(x, y, plane) & format::kClipStandBlockedMask) == 0u;
        }

        // Area id of a world tile, or -1 when the tile is blocked / unreachable /
        // off-plane / in an unmapped square.
        //
        // Header-inlined for the same reason as clipAt.
        int32_t areaAt(int x, int y, int plane)
        {
            if (offWorld(x, y, plane))
            {
                return -1;
            }
            const int sqX = x >> kSquareShift;
            const int sqY = y >> kSquareShift;
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

        // Drop all cached squares, grids, and tile-search stamps. Phase 1
        // stopped doing this on context recycle (the cache survives across
        // queries on the same context); call this only when the borrowed
        // artifact itself has changed in a way that invalidates the bytes —
        // e.g. a forced reload.
        void clearCache();

        // ---- Tile-search visited stamps (Phase 2) ---------------------------
        //
        // Per-(square, plane) generation-counter grids backing TileSearch's
        // closed set. A tile is "closed in this search" when its stamp equals
        // the current tileEpoch. TileSearch bumps the epoch via
        // beginTileSearch() at the top of each findPath so the prior search's
        // marks read as stale without wiping anything; wraparound zeroes the
        // allocated stamps and restarts at 1, mirroring AreaSearch.
        //
        // Storage lives on WorldView so it shares spatial locality with the
        // clip cache the tile search already touches per neighbor, and
        // survives across queries on the same context.
        uint32_t beginTileSearch()
        {
            ++tileEpoch;
            if (tileEpoch == 0u)
            {
                for (auto &kv : visitedStamps)
                {
                    std::fill(kv.second.begin(), kv.second.end(), 0u);
                }
                tileEpoch = 1u;
            }
            return tileEpoch;
        }

        bool isTileClosed(int x, int y, int plane, uint32_t epoch)
        {
            if (offWorld(x, y, plane))
            {
                return true;  // outside the world acts as closed (never expandable)
            }
            const int sqX = x >> kSquareShift;
            const int sqY = y >> kSquareShift;
            std::vector<uint32_t> *stamps = lastStampGrid;
            if (sqX != lastStampSquareX || sqY != lastStampSquareY
                || plane != lastStampPlane || stamps == nullptr)
            {
                stamps = stampsLookup(sqX, sqY, plane);
                lastStampSquareX = sqX;
                lastStampSquareY = sqY;
                lastStampPlane = plane;
                lastStampGrid = stamps;
            }
            return stamps != nullptr && (*stamps)[stampIndex(x, y)] == epoch;
        }

        // Mark (x, y, plane) closed in the current epoch. Lazily allocates the
        // (square, plane) stamp grid on first touch.
        void markTileClosed(int x, int y, int plane, uint32_t epoch)
        {
            if (offWorld(x, y, plane))
            {
                return;
            }
            const int sqX = x >> kSquareShift;
            const int sqY = y >> kSquareShift;
            std::vector<uint32_t> *stamps = lastStampGrid;
            if (sqX != lastStampSquareX || sqY != lastStampSquareY
                || plane != lastStampPlane || stamps == nullptr)
            {
                stamps = &stampsEnsure(sqX, sqY, plane);
                lastStampSquareX = sqX;
                lastStampSquareY = sqY;
                lastStampPlane = plane;
                lastStampGrid = stamps;
            }
            else if (stamps->empty())
            {
                stamps = &stampsEnsure(sqX, sqY, plane);
                lastStampGrid = stamps;
            }
            (*stamps)[stampIndex(x, y)] = epoch;
        }

    private:
        static constexpr int kSquareShift = 6;                  // 64 tiles per square edge
        static constexpr int kLocalMask = format::kClipSize - 1;

        static bool offWorld(int x, int y, int plane)
        {
            // The upper bound enforces the kSquaresPerAxis promise ("off-axis
            // tiles act as blocked"): without it, an absurd-but-valid int32
            // coordinate wraps the reader's 32-bit square keys into a real
            // baked square (returning walkable data for a garbage tile) and
            // overflows this class's 64-bit grid keys (poisoning a cached
            // slot for the context's lifetime).
            constexpr int kAxisTiles = static_cast<int>(kSquaresPerAxis) << kSquareShift;
            return x < 0 || y < 0 || x >= kAxisTiles || y >= kAxisTiles
                || plane < 0 || plane >= format::kClipPlanes;
        }

        // Clip words are plane-major, then x (west-east), then y (south-north).
        static std::size_t clipIndex(int x, int y, int plane)
        {
            const std::size_t lx = static_cast<std::size_t>(x & kLocalMask);
            const std::size_t ly = static_cast<std::size_t>(y & kLocalMask);
            return (static_cast<std::size_t>(plane) * format::kClipSize + lx)
                 * format::kClipSize + ly;
        }

        // Area-id grids and stamp grids are x-major, then y, over a single
        // (square, plane). Stamps deliberately mirror gridIndex so a
        // hypothetical future layout change can move them in lockstep.
        static std::size_t gridIndex(int x, int y)
        {
            const std::size_t lx = static_cast<std::size_t>(x & kLocalMask);
            const std::size_t ly = static_cast<std::size_t>(y & kLocalMask);
            return lx * static_cast<std::size_t>(format::kClipSize) + ly;
        }

        static std::size_t stampIndex(int x, int y)
        {
            return gridIndex(x, y);
        }

        // Inflate-and-cache one map square's clip words / one (square, plane) area
        // grid. The returned vector is empty when that square/grid is not baked.
        // Out-of-line so the inlined clipAt/areaAt bodies stay small.
        const std::vector<uint32_t> &squareWords(int squareX, int squareY);
        const std::vector<int32_t> &gridIds(int squareX, int squareY, int plane);

        // Read-only / read-write accessors for the per-(square, plane) stamp
        // grid. stampsLookup returns nullptr when nothing is allocated for that
        // key — there is nothing to be closed there. stampsEnsure inserts an
        // all-zero grid on first touch so the caller can write into it. Both
        // hand back a mutable pointer/reference (the sticky cache reuses one
        // for both reads and writes; isTileClosed is the only "read" caller).
        // Out-of-line so the inlined isTileClosed/markTileClosed bodies stay
        // small.
        std::vector<uint32_t> *stampsLookup(int squareX, int squareY, int plane);
        std::vector<uint32_t> &stampsEnsure(int squareX, int squareY, int plane);

        const format::ArtifactReader *artifact;
        // 64-bit non-overlapping keys (squareY << 32 | squareX, and
        // squareY << 40 | squareX << 8 | plane). The unordered_map sized
        // itself to the working set (a few tens of squares), which on this
        // bench beats a fully-allocated 256x256 direct-indexed table — the
        // sticky one-slot caches below cover the hot path either way, and
        // the larger structure's empty-header memory cost surfaced as a
        // small same-area regression in early Phase 6 measurements.
        std::unordered_map<uint64_t, std::vector<uint32_t>> clipCache;
        std::unordered_map<uint64_t, std::vector<int32_t>> gridCache;
        // Per-(square, plane) tile-search stamp grids. Keyed identically to
        // gridCache. Lazily inserted by stampsEnsure; live iff
        // stamp[i] == tileEpoch.
        std::unordered_map<uint64_t, std::vector<uint32_t>> visitedStamps;
        uint32_t tileEpoch{0};

        // Sticky one-slot caches for the most recently resolved (square, plane).
        // A* refinement and area expansion both touch tiles in a tight spatial
        // window, so the next clipAt/areaAt call is almost always in the same
        // square as the previous one — a single equality compare skips the
        // unordered_map::find that otherwise dominates the tile-search hot loop.
        // INT_MIN keys are an out-of-range sentinel meaning "no sticky hit";
        // clearCache() resets them so a recycled context starts cold.
        int lastClipSquareX{INT_MIN};
        int lastClipSquareY{INT_MIN};
        const std::vector<uint32_t> *lastClipWords{nullptr};
        int lastGridSquareX{INT_MIN};
        int lastGridSquareY{INT_MIN};
        int lastGridPlane{INT_MIN};
        const std::vector<int32_t> *lastGridIds{nullptr};
        int lastStampSquareX{INT_MIN};
        int lastStampSquareY{INT_MIN};
        int lastStampPlane{INT_MIN};
        std::vector<uint32_t> *lastStampGrid{nullptr};
    };
}

#endif  // WORLDWALKER_RUNTIME_WORLDVIEW_H
