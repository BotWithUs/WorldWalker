#ifndef WORLDWALKER_RUNTIME_INSTANCEMAP_H
#define WORLDWALKER_RUNTIME_INSTANCEMAP_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ww::runtime
{
    // The client's instance chunk-descriptor grid, as published on the agent's
    // wire at protocol v19 and handed down by the host through the C ABI's
    // WwInstanceChunks.
    //
    // A dynamic region ("instance") — a player-owned house, a Dungeoneering
    // floor, a boss instance — is assembled by the client by copying 8x8 chunks
    // out of the static map into a scratch area of the world, optionally rotated.
    // This class answers the one question the pathfinder needs: which static tile
    // was this instance tile copied from, and by how much was its chunk rotated?
    //
    // WorldWalker's baked artifact describes the static map only, so a plan
    // inside an instance reads collision through this indirection. Note that only
    // the per-tile collision READ is redirected: planning stays in instance
    // coordinates, because an instance is assembled from scattered source chunks
    // and two chunks adjacent in the instance can be mapsquares apart in the
    // static world.
    //
    // Transcribed from the normative byte spec in NXTDebugger/wire/PROTOCOL.md
    // 2.10, and kept behaviourally identical to the two existing implementations
    // of the same algorithm — the Java host's DynamicRegion (the tested
    // reference) and NXTLibrary's probe ResolveDynTile. Divergence from either is
    // a bug here, not a design choice.
    //
    // UNITS TRAP: originMapX/Y are MAPSQUARES (64 tiles); gridW/gridH are CHUNKS
    // (8 tiles). The origin is promoted to chunks before it is subtracted. Every
    // implementation of this algorithm documents that trap because every one of
    // them nearly shipped without it.
    class InstanceMap
    {
    public:
        // sourceOfPacked's "this tile was not copied from anywhere" answer. The
        // packed layout only ever fills the low 36 bits, so -1 cannot collide
        // with a real result.
        static constexpr int64_t kNoSource = -1;

        // A descriptor cell with no source chunk — a hole in the instance.
        static constexpr int32_t kNoChunk = -1;

        // Scene geometry. Planes per scene, tiles per chunk edge, and the shifts
        // that convert between tiles, chunks, and mapsquares.
        static constexpr int32_t kPlaneCount = 4;
        static constexpr int32_t kChunkTiles = 8;
        static constexpr int32_t kChunkShift = 3;             // tile >> this == chunk
        static constexpr int32_t kMapsquareChunkShift = 3;    // mapsquare << this == chunk

        // Numerically equal, deliberately spelled apart: kChunkTileMask is only
        // ever the right operand of an &, kChunkMaxLocal is the coordinate the
        // rotation math reflects around ("mask minus x" is nonsense).
        static constexpr int32_t kChunkTileMask = kChunkTiles - 1;
        static constexpr int32_t kChunkMaxLocal = kChunkTiles - 1;

        // Rotations are 0..3 in 90-degree steps; the mask wraps composition.
        static constexpr int32_t kRotationMask = 0x3;

        // Addressable world extent, in the units each check needs it.
        //
        // These MUST agree with WorldView::kSquaresPerAxis — a static_assert in
        // WorldView.h pins that. The reason is not cosmetic: WorldView bounds
        // its visited-stamp grids by that axis, and those stamps stay in
        // INSTANCE coordinates while clip reads move into source coordinates.
        // If a grid were accepted outside the band, clipAt would answer
        // "walkable" for tiles every neighbour reported "already closed", and
        // the search would fail having expanded nothing — a silent
        // no-route indistinguishable from blocked terrain. Refusing the tile
        // here keeps the two views of the world consistent.
        static constexpr int32_t kMapsquaresPerAxis = 256;
        static constexpr int32_t kChunksPerAxis = kMapsquaresPerAxis * 8;
        static constexpr int32_t kTilesPerAxis = kMapsquaresPerAxis * 64;

        // Packed 26-bit descriptor bit layout (bit 0 is unused by the client).
        static constexpr int32_t kDescPlaneShift = 24;
        static constexpr uint32_t kDescPlaneMask = 0x3u;
        static constexpr int32_t kDescChunkXShift = 14;
        static constexpr uint32_t kDescChunkXMask = 0x3FFu;
        static constexpr int32_t kDescChunkYShift = 3;
        static constexpr uint32_t kDescChunkYMask = 0x7FFu;
        static constexpr int32_t kDescRotationShift = 1;
        static constexpr uint32_t kDescRotationMask = 0x3u;

        // Packed source-tile bit layout (the allocation-free resolver result).
        static constexpr int32_t kSrcTileXShift = 0;
        static constexpr int32_t kSrcTileYShift = 16;
        static constexpr int32_t kSrcPlaneShift = 32;
        static constexpr int32_t kSrcRotationShift = 34;
        static constexpr int64_t kSrcTileMask = 0xFFFF;
        static constexpr int64_t kSrcPlaneMask = 0x3;
        static constexpr int64_t kSrcRotationMask = 0x3;

        InstanceMap() = default;

        // Install a descriptor grid, copying `descriptors`. The copy is
        // deliberate: the host's buffer is only borrowed for the duration of one
        // upcall, while a search context holds this map across a whole run.
        //
        // A null/empty descriptor run, a non-positive grid dimension, or a count
        // short of 4 * gridW * gridH leaves the map inactive — a partial grid
        // resolves to plausible wrong tiles rather than failing, which is the
        // worst outcome for a pathfinder, so it is refused outright.
        void assign(int32_t originMapX, int32_t originMapY,
                    int32_t gridW, int32_t gridH,
                    const int32_t *descriptors, std::size_t descriptorCount);

        // Drop the grid, returning the map to its inactive (static-scene) state.
        void clear();

        // Whether a usable descriptor grid is installed. When false every
        // sourceOfPacked answers kNoSource, which is exactly right for a static
        // scene.
        bool isActive() const
        {
            return !cells.empty();
        }

        int32_t originMapX() const
        {
            return originX;
        }

        int32_t originMapY() const
        {
            return originY;
        }

        int32_t gridW() const
        {
            return width;
        }

        int32_t gridH() const
        {
            return height;
        }

        // Whether the tile falls inside the descriptor grid's footprint. True
        // does not promise a source: the cell may still be a hole. Used to decide
        // whether a query's endpoints are describable at all.
        bool coversTile(int32_t tileX, int32_t tileY, int32_t plane) const
        {
            int32_t gridX = 0;
            int32_t gridY = 0;
            return gridCoords(tileX, tileY, plane, gridX, gridY);
        }

        // The static-map tile this instance tile was copied from, packed with its
        // chunk rotation, or kNoSource when the tile has no source — because the
        // map is inactive, the plane is out of range, the tile is outside the
        // grid, or the instance has a hole there. "No source" is a routine
        // answer, not an error.
        //
        // Hot path: this runs once per tile inside the A* inner loop, so it
        // allocates nothing and stays branch-light.
        int64_t sourceOfPacked(int32_t tileX, int32_t tileY, int32_t plane) const
        {
            const int32_t descriptor = descriptorAt(tileX, tileY, plane);
            if (isHole(descriptor))
            {
                return kNoSource;
            }
            const int32_t rotation = descRotation(descriptor);
            const int32_t localX = tileX & kChunkTileMask;
            const int32_t localY = tileY & kChunkTileMask;
            const int32_t sourceX = (descChunkX(descriptor) << kChunkShift)
                                  + rotateLocalX(localX, localY, rotation);
            const int32_t sourceY = (descChunkY(descriptor) << kChunkShift)
                                  + rotateLocalY(localX, localY, rotation);
            return packSource(sourceX, sourceY, descPlane(descriptor), rotation);
        }

        // The raw packed descriptor covering a world tile, or kNoChunk.
        //
        // This is where the UNITS TRAP is paid off exactly once: the tile is
        // reduced to a chunk (>> 3) and the mapsquare origin is promoted to
        // chunks (<< 3) before they are subtracted.
        int32_t descriptorAt(int32_t tileX, int32_t tileY, int32_t plane) const
        {
            int32_t gridX = 0;
            int32_t gridY = 0;
            if (!gridCoords(tileX, tileY, plane, gridX, gridY))
            {
                return kNoChunk;
            }
            const std::size_t index =
                static_cast<std::size_t>((plane * width) + gridX) * static_cast<std::size_t>(height)
                + static_cast<std::size_t>(gridY);
            // assign() refuses a short grid, so a tile that passed gridCoords
            // always indexes a published cell.
            return cells[index];
        }

        // ---- Descriptor decode --------------------------------------------
        //
        // Every extractor casts to unsigned before shifting, mirroring Java's
        // >>>, so a malformed high-bit value cannot sign-extend into a field.

        // Whether a descriptor means "no source chunk". Tests < 0 rather than
        // == kNoChunk so a malformed high-bit value degrades to "no source"
        // instead of decoding into garbage coordinates.
        static constexpr bool isHole(int32_t descriptor)
        {
            return descriptor < 0;
        }

        static constexpr int32_t descPlane(int32_t descriptor)
        {
            return static_cast<int32_t>((static_cast<uint32_t>(descriptor) >> kDescPlaneShift)
                                        & kDescPlaneMask);
        }

        static constexpr int32_t descChunkX(int32_t descriptor)
        {
            return static_cast<int32_t>((static_cast<uint32_t>(descriptor) >> kDescChunkXShift)
                                        & kDescChunkXMask);
        }

        static constexpr int32_t descChunkY(int32_t descriptor)
        {
            return static_cast<int32_t>((static_cast<uint32_t>(descriptor) >> kDescChunkYShift)
                                        & kDescChunkYMask);
        }

        static constexpr int32_t descRotation(int32_t descriptor)
        {
            return static_cast<int32_t>((static_cast<uint32_t>(descriptor) >> kDescRotationShift)
                                        & kDescRotationMask);
        }

        // ---- Rotation ------------------------------------------------------

        // Source-local X for destination-local (localX, localY) within an 8x8
        // chunk rotated by `rotation`. Transcribed from the client's own rotation
        // switch: r0 (x,y), r1 (y, 7-x), r2 (7-x, 7-y), r3 (7-y, x). Only the low
        // two bits are used, matching the 2-bit wire field.
        static constexpr int32_t rotateLocalX(int32_t localX, int32_t localY, int32_t rotation)
        {
            switch (rotation & kRotationMask)
            {
                case 1:  return localY;
                case 2:  return kChunkMaxLocal - localX;
                case 3:  return kChunkMaxLocal - localY;
                default: return localX;
            }
        }

        // Source-local Y counterpart of rotateLocalX.
        static constexpr int32_t rotateLocalY(int32_t localX, int32_t localY, int32_t rotation)
        {
            switch (rotation & kRotationMask)
            {
                case 1:  return kChunkMaxLocal - localX;
                case 2:  return kChunkMaxLocal - localY;
                case 3:  return localX;
                default: return localY;
            }
        }

        // ---- Packed source-tile codec --------------------------------------

        static constexpr int64_t packSource(int32_t tileX, int32_t tileY,
                                            int32_t plane, int32_t rotation)
        {
            return ((static_cast<int64_t>(tileX) & kSrcTileMask) << kSrcTileXShift)
                 | ((static_cast<int64_t>(tileY) & kSrcTileMask) << kSrcTileYShift)
                 | ((static_cast<int64_t>(plane) & kSrcPlaneMask) << kSrcPlaneShift)
                 | ((static_cast<int64_t>(rotation) & kSrcRotationMask) << kSrcRotationShift);
        }

        static constexpr int32_t srcTileX(int64_t packed)
        {
            return static_cast<int32_t>((packed >> kSrcTileXShift) & kSrcTileMask);
        }

        static constexpr int32_t srcTileY(int64_t packed)
        {
            return static_cast<int32_t>((packed >> kSrcTileYShift) & kSrcTileMask);
        }

        static constexpr int32_t srcPlane(int64_t packed)
        {
            return static_cast<int32_t>((packed >> kSrcPlaneShift) & kSrcPlaneMask);
        }

        static constexpr int32_t srcRotation(int64_t packed)
        {
            return static_cast<int32_t>((packed >> kSrcRotationShift) & kSrcRotationMask);
        }

    private:
        // Grid cell covering a world tile. Returns false — leaving the outputs
        // untouched — when the tile indexes no published descriptor.
        //
        // The single place the UNITS TRAP is paid: the tile is reduced to a
        // chunk (>> 3) and the mapsquare origin promoted to chunks (<< 3) before
        // they are subtracted. It used to be written out twice; one copy is one
        // fewer place for the two to drift, and the hot path stops computing it
        // twice per tile.
        bool gridCoords(int32_t tileX, int32_t tileY, int32_t plane,
                        int32_t &outGridX, int32_t &outGridY) const
        {
            if (cells.empty() || plane < 0 || plane >= kPlaneCount)
            {
                return false;
            }
            // Tiles outside the addressable band are refused rather than
            // resolved: see the kTilesPerAxis comment for why answering here
            // would silently disagree with WorldView's visited stamps.
            if (tileX < 0 || tileY < 0 || tileX >= kTilesPerAxis || tileY >= kTilesPerAxis)
            {
                return false;
            }
            const int32_t gridX = (tileX >> kChunkShift) - (originX << kMapsquareChunkShift);
            const int32_t gridY = (tileY >> kChunkShift) - (originY << kMapsquareChunkShift);
            if (gridX < 0 || gridX >= width || gridY < 0 || gridY >= height)
            {
                return false;
            }
            outGridX = gridX;
            outGridY = gridY;
            return true;
        }

        std::vector<int32_t> cells;   // plane-major: ((plane * width) + gx) * height + gy
        int32_t originX{0};           // MAPSQUARES
        int32_t originY{0};           // MAPSQUARES
        int32_t width{0};             // CHUNKS
        int32_t height{0};            // CHUNKS
    };
}

#endif  // WORLDWALKER_RUNTIME_INSTANCEMAP_H
