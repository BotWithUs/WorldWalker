#ifndef WORLDWALKER_RUNTIME_WORLDVIEW_H
#define WORLDWALKER_RUNTIME_WORLDVIEW_H

#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "format/ClipFlags.h"
#include "runtime/InstanceMap.h"

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
    // Dynamic regions (instances): when an InstanceMap is installed, clipAt
    // resolves each tile to the static tile its 8x8 chunk was copied from and
    // returns that tile's baked word, rotated. Coordinates in and out of this
    // class stay INSTANCE coordinates — only the internal lookup moves into
    // source space — because an instance is assembled from scattered chunks and
    // planning in source space would route the avatar across the real world
    // between chunks that are neighbours in the instance. areaAt stays static-only
    // (see its comment).
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
        // populated square without overshoot. Off-axis tiles act as blocked.
        // The caches below are hash maps sized to the working set, so this
        // bound costs no memory up front.
        static constexpr std::size_t kSquaresPerAxis = 256;

        // Per-context cache ceiling (see trimCache). 64 MB is ~1000 inflated
        // squares - far more than any one walk touches, small enough that a
        // core-count pool stays under a gigabyte in the worst case.
        static constexpr std::size_t kCacheBudgetBytes = std::size_t{64} << 20;

        // The instance resolver refuses tiles outside the same band, so a
        // dynamic region can never answer "walkable" for a tile this class's
        // visited stamps treat as permanently closed. Drift between the two
        // would surface as a search that expands nothing and reports no route.
        static_assert(static_cast<std::size_t>(InstanceMap::kMapsquaresPerAxis) == kSquaresPerAxis,
                      "InstanceMap and WorldView must agree on the addressable world extent");

        explicit WorldView(const format::ArtifactReader &reader);

        WorldView(const WorldView &) = delete;
        WorldView &operator=(const WorldView &) = delete;
        // Moving carries the borrowed `instance` pointer to the new object,
        // which is only sound while the pointee outlives both. That holds today
        // because SearchContext owns the InstanceMap, is itself non-movable, and
        // is heap-pinned by the pool's unique_ptr — so no move ever happens.
        // Re-check this if a WorldView is ever moved between contexts.
        WorldView(WorldView &&) = default;
        WorldView &operator=(WorldView &&) = default;

        // Install (or clear, with nullptr) the dynamic-region descriptor grid
        // this view resolves collision through. Borrowed, not owned: the map
        // must outlive the view's use of it. SearchContext owns one per context
        // and clears it on recycle, so a later static query cannot inherit a
        // stale instance.
        void setInstance(const InstanceMap *map)
        {
            instance = map;
        }

        // The installed dynamic-region map, or nullptr in a static scene.
        const InstanceMap *instanceMap() const
        {
            return instance;
        }

        // Whether this view is currently resolving through an instance.
        bool isInstanced() const
        {
            return instance != nullptr && instance->isActive();
        }

        // Directional clip word for a world tile, or kBlockedWord when the tile is
        // off-plane, negative, or in a square the artifact does not bake.
        //
        // Inside a dynamic region the tile is first resolved to the static tile
        // its chunk was copied from, and the answer is the SOURCE tile's baked
        // word with its directional bits rotated by the chunk rotation. Resolving
        // before the square lookup is deliberate: it leaves clipCache and the
        // sticky slot keyed on SOURCE squares in both modes, so entering or
        // leaving an instance needs no cache invalidation. Keying them on
        // instance coordinates would quietly serve one instance's collision to
        // the next.
        uint32_t clipAt(int x, int y, int plane)
        {
            if (instance != nullptr && instance->isActive())
            {
                return instancedClipAt(x, y, plane);
            }
            return bakedClipAt(x, y, plane);
        }

    private:
        // The two halves of clipAt. Private — every caller outside this class
        // wants clipAt, which picks between them; instancedClipAt in particular
        // dereferences `instance` unconditionally and is only safe once clipAt
        // has established the map is installed and active. They sit here rather
        // than with the other private members so they stay adjacent to the
        // dispatcher that is the whole reason they are split.

        // Baked clip word addressed by STATIC world tile, ignoring any installed
        // instance. In instance mode the caller has already resolved the tile to
        // its source, so this always sees static-world coordinates.
        //
        // Header-inlined: the sticky one-slot fast path is the dominant inner-
        // loop step of tile A* (TileSearch::tryStep + expand). Inlining lets the
        // sticky compare + array index fold into the caller; the slow path
        // (squareWords) stays out-of-line so the inlined body is small.
        uint32_t bakedClipAt(int x, int y, int plane)
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

        // Clip word for a tile inside a dynamic region: resolve the source tile,
        // read its baked word, rotate the directional bits.
        //
        // A tile the descriptor grid does not source — outside the grid, or a
        // hole inside it — reads as fully blocked. A hole in an instance is
        // genuinely solid, and the static map underneath an instance describes
        // unrelated terrain, so falling back to it would be worse than failing.
        uint32_t instancedClipAt(int x, int y, int plane)
        {
            const int64_t source = instance->sourceOfPacked(x, y, plane);
            if (source == InstanceMap::kNoSource)
            {
                return kBlockedWord;
            }
            const uint32_t word = bakedClipAt(InstanceMap::srcTileX(source),
                                              InstanceMap::srcTileY(source),
                                              InstanceMap::srcPlane(source));
            return format::rotateClipWord(word, InstanceMap::srcRotation(source));
        }

    public:
        // True when a unit may occupy the tile (no whole-tile blocker). Wall-edge
        // bits block crossing, not standing, so they are ignored here.
        bool isStandable(int x, int y, int plane)
        {
            return (clipAt(x, y, plane) & format::kClipStandBlockedMask) == 0u;
        }

        // True when (x, y) lies in a mapsquare the artifact bakes, on a legal
        // plane. Static coordinates, ignoring any installed instance: it tells
        // a start the walker has a map for from one it has never seen (an
        // instance, a region the bake does not cover).
        bool isBakedTile(int x, int y, int plane)
        {
            if (offWorld(x, y, plane))
            {
                return false;
            }
            return !squareWords(x >> kSquareShift, y >> kSquareShift).empty();
        }

        // Area id of a world tile, or -1 when the tile is blocked / unreachable /
        // off-plane / in an unmapped square.
        //
        // Deliberately NOT instance-aware, and it must stay that way. The baked
        // area graph is a connectivity model of the static world; the source
        // chunks an instance is assembled from belong to unrelated areas, so
        // remapping through the instance would stitch a graph whose edges do not
        // exist. Inside a dynamic region this answers -1, which is the honest
        // answer, and PathAssembler takes a tile-level branch that never asks.
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

        // Approximate bytes held by the three caches (inflated clip words, area
        // grids, visited stamps). They grow with every square a search touches
        // and nothing else ever shrinks them, so a long-lived context that has
        // roamed the world holds most of the artifact inflated - times the pool
        // size. Cheap to compute; used by trimCache.
        std::size_t cacheBytes() const;

        // Bound the cache: wipe everything (clearCache) once cacheBytes exceeds
        // kCacheBudgetBytes. Called between borrows (SearchContext::recycle),
        // never mid-search - the visited stamps ARE the in-flight closed set
        // and the sticky pointers alias the cached vectors. A wipe costs the
        // next borrower a re-inflate of the squares it touches (tens of
        // microseconds each); an unbounded cache costs the process the whole
        // world per context.
        void trimCache();

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
        // survives across queries on the same context. The stamps are keyed on
        // INSTANCE coordinates — they track the search, not the map — so inside
        // a dynamic region they and the clip cache sit in different coordinate
        // spaces and that locality no longer holds. Correctness is unaffected;
        // only the cache-friendliness argument is.
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
        // Borrowed dynamic-region descriptor grid, or nullptr in a static scene.
        // Owned by the SearchContext; see setInstance.
        const InstanceMap *instance{nullptr};
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
