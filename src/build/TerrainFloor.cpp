#include "build/TerrainFloor.h"

#include "format/Artifact.h"
#include "format/ClipFlags.h"

#include <algorithm>

namespace ww::build
{
    namespace
    {
        constexpr int kMapIndex = 5;
        constexpr int kTerrainFile = 3;
        constexpr int kSquareTiles = format::kClipSize * format::kClipSize;
        constexpr std::size_t kTerrainTiles =
            static_cast<std::size_t>(format::kClipPlanes) * kSquareTiles;
        // The terrain stream opens with a 5-byte header the decoders skip.
        constexpr std::size_t kHeaderBytes = 5;

        // Per-tile stream flags, as maps::MapSquare decodeMapTerrain reads them.
        constexpr uint8_t kStreamOverlay = 0x1;     // shape byte + overlay id smart
        constexpr uint8_t kStreamRenderRule = 0x2;  // render-rule byte
        constexpr uint8_t kStreamUnderlay = 0x4;    // underlay id smart
        constexpr uint8_t kStreamWide = 0x8;        // two further bytes

        uint32_t squareKeyOf(int squareX, int squareY)
        {
            return (static_cast<uint32_t>(squareX & 0xFFFF) << 16)
                 | static_cast<uint32_t>(squareY & 0xFFFF);
        }

        std::size_t tileIndex(int plane, int localX, int localY)
        {
            return static_cast<std::size_t>(plane) * kSquareTiles
                 + static_cast<std::size_t>(localX) * format::kClipSize
                 + static_cast<std::size_t>(localY);
        }

        // Byte cursor over the raw stream. Every read reports whether the
        // stream still held the bytes, so a truncated file stops the decode.
        struct StreamCursor
        {
            const std::vector<uint8_t> &bytes;
            std::size_t pos;

            bool readByte(int &outValue)
            {
                if (pos >= bytes.size())
                {
                    return false;
                }
                outValue = bytes[pos++];
                return true;
            }

            // RS "smart": one byte below 128, else a 16-bit value biased by
            // 32768. Mirrors readSmart in maps::MapSquare, including reading
            // nothing when the second byte is missing.
            bool readSmart(int &outValue)
            {
                if (pos >= bytes.size())
                {
                    return false;
                }
                const int peek = bytes[pos];
                if (peek < 128)
                {
                    ++pos;
                    outValue = peek;
                    return true;
                }
                if (pos + 1 >= bytes.size())
                {
                    return false;
                }
                outValue = ((bytes[pos] << 8) | bytes[pos + 1]) - 32768;
                pos += 2;
                return true;
            }

            bool skip(std::size_t count)
            {
                if (pos + count > bytes.size())
                {
                    return false;
                }
                pos += count;
                return true;
            }
        };

        // Decode one tile's entry. Returns false when the stream ran out.
        bool decodeTile(StreamCursor &cursor, std::size_t tile, SquareTerrain &ioTerrain)
        {
            int flags = 0;
            if (!cursor.readByte(flags))
            {
                return false;
            }
            ioTerrain.streamFlags[tile] = static_cast<uint8_t>(flags);
            if ((flags & kStreamOverlay) != 0)
            {
                int overlayId = 0;
                if (!cursor.skip(1) || !cursor.readSmart(overlayId))
                {
                    return false;
                }
                ioTerrain.overlayIds[tile] = overlayId;
            }
            if ((flags & kStreamRenderRule) != 0)
            {
                int rule = 0;
                if (!cursor.readByte(rule))
                {
                    return false;
                }
                ioTerrain.renderRules[tile] = static_cast<uint8_t>(rule);
            }
            if ((flags & kStreamUnderlay) != 0)
            {
                int underlayId = 0;
                if (!cursor.readSmart(underlayId))
                {
                    return false;
                }
                ioTerrain.underlayIds[tile] = underlayId;
            }
            if ((flags & kStreamWide) != 0 && !cursor.skip(2))
            {
                return false;
            }
            return true;
        }

        // Mutable, cached view of the model's clip words by world tile.
        class ClipGrid
        {
        public:
            explicit ClipGrid(CollisionModel &model)
            {
                index.reserve(model.squares.size() * 2 + 1);
                for (SquareClip &sq : model.squares)
                {
                    index.emplace(squareKeyOf(sq.squareX, sq.squareY), &sq.words);
                }
            }

            // nullptr for a tile of an absent square (which reads as blocked).
            uint32_t *word(int worldX, int worldY, int plane)
            {
                if (worldX < 0 || worldY < 0)
                {
                    return nullptr;
                }
                const uint32_t key = squareKeyOf(worldX >> 6, worldY >> 6);
                if (key != lastKey)
                {
                    const auto it = index.find(key);
                    lastWords = it == index.end() ? nullptr : it->second;
                    lastKey = key;
                }
                if (lastWords == nullptr)
                {
                    return nullptr;
                }
                return &(*lastWords)[tileIndex(plane, worldX & 63, worldY & 63)];
            }

        private:
            std::unordered_map<uint32_t, std::vector<uint32_t> *> index;
            uint32_t lastKey{0xFFFFFFFFu};
            std::vector<uint32_t> *lastWords{nullptr};
        };

        // The area fill's step rule (AreaGraph canStep): a wall bit on either
        // side of the shared edge blocks a cardinal step.
        bool isWallBetween(uint32_t src, uint32_t dst, int dx, int dy)
        {
            uint32_t srcWall = format::CLIP_WALL_W;
            uint32_t dstWall = format::CLIP_WALL_E;
            if (dy == 1)
            {
                srcWall = format::CLIP_WALL_N;
                dstWall = format::CLIP_WALL_S;
            }
            else if (dy == -1)
            {
                srcWall = format::CLIP_WALL_S;
                dstWall = format::CLIP_WALL_N;
            }
            else if (dx == 1)
            {
                srcWall = format::CLIP_WALL_E;
                dstWall = format::CLIP_WALL_W;
            }
            return (src & srcWall) != 0u || (dst & dstWall) != 0u;
        }

        uint64_t packTile(int x, int y)
        {
            return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32)
                 | static_cast<uint64_t>(static_cast<uint32_t>(y));
        }

        // Floods one stretch of unpainted standable tiles on one plane.
        class VoidFill
        {
        public:
            VoidFill(const FloorIndex &floorIndex, ClipGrid &clipGrid, int fillPlane)
                : floors(floorIndex), grid(clipGrid), plane(fillPlane)
            {
            }

            bool isCandidate(int x, int y)
            {
                const uint32_t *w = grid.word(x, y, plane);
                return w != nullptr && (*w & format::kClipStandBlockedMask) == 0u
                    && !floors.isFloorNear(x, y, plane);
            }

            // Visit the stretch holding the seed, marking it in ioVisited.
            // Returns its tile count.
            std::size_t measure(int seedX, int seedY, TileBits &ioVisited)
            {
                return flood(seedX, seedY, [&](int x, int y)
                {
                    if (ioVisited.test(x, y, plane))
                    {
                        return false;
                    }
                    ioVisited.set(x, y, plane);
                    return true;
                });
            }

            // Block every tile of the stretch holding the seed. Returns the
            // number blocked.
            std::size_t block(int seedX, int seedY)
            {
                return flood(seedX, seedY, [&](int x, int y)
                {
                    *grid.word(x, y, plane) |= static_cast<uint32_t>(format::CLIP_BLOCKED);
                    return true;
                });
            }

        private:
            // claim(x, y) takes a candidate tile and reports whether it was
            // still free. Blocking is its own mark: a blocked tile is no
            // longer a candidate, so the block pass never revisits one.
            template <typename Claim>
            std::size_t flood(int seedX, int seedY, Claim claim)
            {
                static constexpr int kDx[4] = {0, 0, 1, -1};
                static constexpr int kDy[4] = {1, -1, 0, 0};
                std::size_t count = 0;
                stack.clear();
                claim(seedX, seedY);
                stack.push_back(packTile(seedX, seedY));
                while (!stack.empty())
                {
                    const uint64_t cur = stack.back();
                    stack.pop_back();
                    ++count;
                    const int x = static_cast<int>(static_cast<uint32_t>(cur >> 32));
                    const int y = static_cast<int>(static_cast<uint32_t>(cur & 0xFFFFFFFFu));
                    const uint32_t src = *grid.word(x, y, plane);
                    for (int d = 0; d < 4; ++d)
                    {
                        const int nx = x + kDx[d];
                        const int ny = y + kDy[d];
                        if (!isCandidate(nx, ny)
                            || isWallBetween(src, *grid.word(nx, ny, plane), kDx[d], kDy[d])
                            || !claim(nx, ny))
                        {
                            continue;
                        }
                        stack.push_back(packTile(nx, ny));
                    }
                }
                return count;
            }

            const FloorIndex &floors;
            ClipGrid &grid;
            int plane;
            std::vector<uint64_t> stack;
        };
    }

    bool SquareTerrain::isPainted(int plane, int localX, int localY) const
    {
        const std::size_t tile = tileIndex(plane, localX, localY);
        return tile < overlayIds.size() && (overlayIds[tile] > 0 || underlayIds[tile] > 0);
    }

    void decodeSquareTerrain(const std::vector<uint8_t> &bytes, SquareTerrain &outTerrain)
    {
        outTerrain.overlayIds.assign(kTerrainTiles, 0);
        outTerrain.underlayIds.assign(kTerrainTiles, 0);
        outTerrain.renderRules.assign(kTerrainTiles, 0);
        outTerrain.streamFlags.assign(kTerrainTiles, 0);
        StreamCursor cursor{bytes, kHeaderBytes};
        for (int plane = 0; plane < format::kClipPlanes; ++plane)
        {
            for (int x = 0; x < format::kClipSize; ++x)
            {
                for (int y = 0; y < format::kClipSize; ++y)
                {
                    if (!decodeTile(cursor, tileIndex(plane, x, y), outTerrain))
                    {
                        return;
                    }
                }
            }
        }
    }

    bool readSquareTerrain(const CacheClient &cache, int squareX, int squareY,
                           SquareTerrain &outTerrain)
    {
        std::vector<uint8_t> bytes;
        const int archiveId = (squareX & 0x7F) | (squareY << 7);
        const bool isPresent = cache.readFile(kMapIndex, archiveId, kTerrainFile, bytes);
        if (!isPresent)
        {
            bytes.clear();
        }
        decodeSquareTerrain(bytes, outTerrain);
        return isPresent;
    }

    uint32_t TileBits::squareKey(int squareX, int squareY)
    {
        return squareKeyOf(squareX, squareY);
    }

    void TileBits::set(int worldX, int worldY, int plane)
    {
        if (worldX < 0 || worldY < 0 || plane < 0 || plane >= format::kClipPlanes)
        {
            return;
        }
        std::vector<uint64_t> &bits = squares[squareKey(worldX >> 6, worldY >> 6)];
        if (bits.empty())
        {
            bits.assign(kTerrainTiles / 64u, 0u);
        }
        const std::size_t tile = tileIndex(plane, worldX & 63, worldY & 63);
        bits[tile / 64u] |= uint64_t{1} << (tile % 64u);
    }

    bool TileBits::test(int worldX, int worldY, int plane) const
    {
        if (worldX < 0 || worldY < 0 || plane < 0 || plane >= format::kClipPlanes)
        {
            return false;
        }
        const auto it = squares.find(squareKey(worldX >> 6, worldY >> 6));
        if (it == squares.end())
        {
            return false;
        }
        const std::size_t tile = tileIndex(plane, worldX & 63, worldY & 63);
        return ((it->second[tile / 64u] >> (tile % 64u)) & 1u) != 0u;
    }

    // Stores the paint already widened by kFloorRimTiles, so the fill asks one
    // lookup per tile.
    void FloorIndex::addSquare(int squareX, int squareY, const SquareTerrain &terrain)
    {
        const int baseX = squareX * format::kClipSize;
        const int baseY = squareY * format::kClipSize;
        for (int plane = 1; plane < format::kClipPlanes; ++plane)
        {
            for (int x = 0; x < format::kClipSize; ++x)
            {
                for (int y = 0; y < format::kClipSize; ++y)
                {
                    const bool isBridge =
                        (terrain.renderRules[tileIndex(1, x, y)] & kRenderRuleBridge) != 0;
                    const int sourcePlane = isBridge ? plane + 1 : plane;
                    if (sourcePlane >= format::kClipPlanes || !terrain.isPainted(sourcePlane, x, y))
                    {
                        continue;
                    }
                    for (int dx = -kFloorRimTiles; dx <= kFloorRimTiles; ++dx)
                    {
                        for (int dy = -kFloorRimTiles; dy <= kFloorRimTiles; ++dy)
                        {
                            painted.set(baseX + x + dx, baseY + y + dy, plane);
                        }
                    }
                }
            }
        }
    }

    bool FloorIndex::isFloorNear(int worldX, int worldY, int plane) const
    {
        return painted.test(worldX, worldY, plane);
    }

    VoidFenceReport fenceUnpaintedVoid(const FloorIndex &floors, CollisionModel &ioModel)
    {
        VoidFenceReport report;
        ClipGrid grid(ioModel);
        TileBits visited;
        for (int plane = 1; plane < format::kClipPlanes; ++plane)
        {
            VoidFill fill(floors, grid, plane);
            for (const SquareClip &sq : ioModel.squares)
            {
                const int baseX = sq.squareX * format::kClipSize;
                const int baseY = sq.squareY * format::kClipSize;
                for (int x = 0; x < format::kClipSize; ++x)
                {
                    for (int y = 0; y < format::kClipSize; ++y)
                    {
                        const int wx = baseX + x;
                        const int wy = baseY + y;
                        if (visited.test(wx, wy, plane) || !fill.isCandidate(wx, wy))
                        {
                            continue;
                        }
                        const std::size_t tiles = fill.measure(wx, wy, visited);
                        if (tiles <= kMaxUnpaintedPocketTiles)
                        {
                            ++report.pockets;
                            report.largestPocket = std::max(report.largestPocket, tiles);
                            continue;
                        }
                        ++report.voids;
                        report.tilesBlocked += fill.block(wx, wy);
                    }
                }
            }
        }
        return report;
    }
}
