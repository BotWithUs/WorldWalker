#ifndef WORLDWALKER_BUILD_TERRAINFLOOR_H
#define WORLDWALKER_BUILD_TERRAINFLOOR_H

#include "build/CacheClient.h"
#include "build/CollisionBuilder.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ww::build
{
    // What one map square's terrain file (index 5, file 3) says about each
    // tile: the overlay and underlay painted on it, its render-rule byte (bit
    // 0x2 marks a bridge column) and the raw per-tile stream flags. Arrays are
    // plane-major and x-major like SquareClip::words: [plane * 4096 + x * 64 + y].
    //
    // NXTCacheLibrary decodes the same file for collision (maps::MapSquare,
    // decodeMapTerrain) but keeps only the blocked and bridge bits and the
    // overlay id, so the clip words it hands over cannot tell a floor from the
    // void above open ground: both are all-zero words. This decoder mirrors
    // that stream layout byte for byte and keeps what the clip drops.
    struct SquareTerrain
    {
        std::vector<int32_t> overlayIds;
        std::vector<int32_t> underlayIds;
        std::vector<uint8_t> renderRules;
        std::vector<uint8_t> streamFlags;

        bool isPainted(int plane, int localX, int localY) const;
    };

    inline constexpr uint8_t kRenderRuleBridge = 0x2;

    // Decode a raw terrain file. A truncated stream stops early and leaves the
    // rest of the square unpainted, as the collision decoder does.
    void decodeSquareTerrain(const std::vector<uint8_t> &bytes, SquareTerrain &outTerrain);

    // Read and decode one square's terrain file. Returns false when the square
    // or its terrain file is absent (outTerrain is then all unpainted).
    bool readSquareTerrain(const CacheClient &cache, int squareX, int squareY,
                           SquareTerrain &outTerrain);

    // One bit per tile, 4 planes, for the squares that have had a bit set.
    class TileBits
    {
    public:
        void set(int worldX, int worldY, int plane);

        // False for a tile of a square no bit was ever set in.
        bool test(int worldX, int worldY, int plane) const;

    private:
        static uint32_t squareKey(int squareX, int squareY);

        std::unordered_map<uint32_t, std::vector<uint64_t>> squares;
    };

    // How far a floor reaches past its paint. Walkable ground can run a tile
    // beyond the painted floor (the landings of the rock crossings at
    // 3430,4261 and 3434,4261 on plane 1 are unpainted), so a tile within this
    // Chebyshev distance of paint is never treated as void.
    inline constexpr int kFloorRimTiles = 1;

    // Which tiles have a painted floor, per collision plane: a bridge column's
    // upper planes are shifted down one, as the collision decoder shifts them.
    class FloorIndex
    {
    public:
        void addSquare(int squareX, int squareY, const SquareTerrain &terrain);

        // Painted anywhere within kFloorRimTiles of the tile.
        bool isFloorNear(int worldX, int worldY, int plane) const;

    private:
        TileBits painted;
    };

    // The largest walk-connected stretch of unpainted upper-plane tiles kept
    // walkable. Paint alone does not mark every floor: part of the City of
    // Um's plane-1 street (1150..1165, 1826..1832) is unpainted ground walled
    // in by blocked terrain, and some upper rooms are floored by locs. Those
    // are pockets. The void over open ground is one stretch per region, joined
    // across every map square with no wall to stop it: before this fence one
    // plane-1 area spanned 6016x7232 tiles and walked from Lumbridge's house
    // stairs to Pollnivneach's.
    inline constexpr std::size_t kMaxUnpaintedPocketTiles = 4096;

    // Block, on planes 1..3, every walk-connected stretch of standable tiles
    // with no paint within kFloorRimTiles that holds more than
    // kMaxUnpaintedPocketTiles tiles. Stretches are joined the way the area
    // fill joins tiles (cardinal steps, walls respected, across squares).
    VoidFenceReport fenceUnpaintedVoid(const FloorIndex &floors, CollisionModel &ioModel);
}

#endif  // WORLDWALKER_BUILD_TERRAINFLOOR_H
