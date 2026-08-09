#include "cli/InstanceTests.h"

#include "format/ClipFlags.h"
#include "runtime/InstanceMap.h"

#include <cstdint>
#include <cstdio>
#include <vector>

// Dynamic-region (instance) unit tests — see InstanceTests.h for why this layer
// exists and what it is guarding against.
namespace
{
    using ww::format::rotateClipWord;
    using ww::runtime::InstanceMap;

    // The eight wall-edge flags in the order their bits appear (clockwise from
    // NW), paired with the tile offset each one faces. This table is the only
    // place the geometry is stated; the rotation test derives everything else
    // from it plus InstanceMap's own rotateLocalX/Y.
    struct WallDir
    {
        int32_t dx;
        int32_t dy;
        uint32_t flag;
    };

    constexpr WallDir kWallDirs[] = {
        { -1,  1, ww::format::CLIP_WALL_NW },
        {  0,  1, ww::format::CLIP_WALL_N  },
        {  1,  1, ww::format::CLIP_WALL_NE },
        {  1,  0, ww::format::CLIP_WALL_E  },
        {  1, -1, ww::format::CLIP_WALL_SE },
        {  0, -1, ww::format::CLIP_WALL_S  },
        { -1, -1, ww::format::CLIP_WALL_SW },
        { -1,  0, ww::format::CLIP_WALL_W  },
    };

    constexpr int kWallDirCount = 8;

    uint32_t flagForOffset(int32_t dx, int32_t dy)
    {
        for (const WallDir &d : kWallDirs)
        {
            if (d.dx == dx && d.dy == dy)
            {
                return d.flag;
            }
        }
        return 0u;
    }

    int fail(const char *what)
    {
        std::printf("  FAIL: %s\n", what);
        return 1;
    }

    // Build the raw 26-bit descriptor the client publishes.
    int32_t packDescriptor(int32_t plane, int32_t chunkX, int32_t chunkY, int32_t rotation)
    {
        return (plane << 24) | (chunkX << 14) | (chunkY << 3) | (rotation << 1);
    }

    std::vector<int32_t> makeGrid(int32_t gridW, int32_t gridH)
    {
        return std::vector<int32_t>(
            static_cast<std::size_t>(4 * gridW * gridH), InstanceMap::kNoChunk);
    }

    void setCell(std::vector<int32_t> &outGrid, int32_t gridW, int32_t gridH,
                 int32_t plane, int32_t gx, int32_t gy, int32_t descriptor)
    {
        outGrid[static_cast<std::size_t>(((plane * gridW) + gx) * gridH + gy)] = descriptor;
    }

    // ---- Rotation ------------------------------------------------------

    // Cross-validate rotateClipWord against InstanceMap's tile rotation, which
    // is the only other statement of the same geometry.
    //
    // Take two adjacent tiles in the instance, P and its neighbour in direction
    // d. Rotate both back to their source-local positions. The edge between them
    // faces some direction s in source space. A wall stored on the source tile's
    // s edge is therefore the same physical wall as one on the instance tile's d
    // edge — so rotating the source's s flag must produce the instance's d flag.
    //
    // Nothing here restates "shift by two bits": if rotateClipWord and
    // rotateLocalX/Y ever disagree about which way a chunk turns, this fires.
    int checkRotationGeometry()
    {
        int failures = 0;
        for (int32_t rotation = 0; rotation <= 3; ++rotation)
        {
            // (3, 3) keeps every 8-neighbour inside the 0..7 chunk.
            constexpr int32_t px = 3;
            constexpr int32_t py = 3;
            const int32_t sx = InstanceMap::rotateLocalX(px, py, rotation);
            const int32_t sy = InstanceMap::rotateLocalY(px, py, rotation);
            for (int i = 0; i < kWallDirCount; ++i)
            {
                const WallDir &d = kWallDirs[i];
                const int32_t qx = InstanceMap::rotateLocalX(px + d.dx, py + d.dy, rotation);
                const int32_t qy = InstanceMap::rotateLocalY(px + d.dx, py + d.dy, rotation);
                const uint32_t sourceFlag = flagForOffset(qx - sx, qy - sy);
                if (sourceFlag == 0u)
                {
                    failures += fail("rotation produced a non-adjacent source offset");
                    continue;
                }
                if (rotateClipWord(sourceFlag, rotation) != d.flag)
                {
                    std::printf("  FAIL: rot=%d source flag 0x%X should rotate to 0x%X, got 0x%X\n",
                                rotation, sourceFlag, d.flag,
                                rotateClipWord(sourceFlag, rotation));
                    ++failures;
                }
            }
        }
        return failures;
    }

    // Rotation 0 is the identity, non-directional bits never move, and four
    // 90-degree steps return to the start.
    int checkRotationInvariants()
    {
        int failures = 0;
        const uint32_t wholeTile = ww::format::CLIP_BLOCKED | ww::format::CLIP_WATER
                                 | ww::format::CLIP_DOOR | ww::format::CLIP_OBJECT
                                 | ww::format::CLIP_PLANE_CHANGE;
        const uint32_t mixed = wholeTile | ww::format::CLIP_WALL_N | ww::format::CLIP_WALL_SE;

        if (rotateClipWord(mixed, 0) != mixed)
        {
            failures += fail("rotation 0 is not the identity");
        }
        for (int32_t rotation = 0; rotation <= 3; ++rotation)
        {
            if ((rotateClipWord(mixed, rotation) & ~ww::format::kClipWallMask)
                != (mixed & ~ww::format::kClipWallMask))
            {
                failures += fail("rotation disturbed a non-directional clip bit");
            }
            if (rotateClipWord(wholeTile, rotation) != wholeTile)
            {
                failures += fail("rotation changed a word with no wall bits");
            }
        }
        // Only the low two bits of rotation are significant, matching the 2-bit
        // wire field, so a full turn is a no-op.
        if (rotateClipWord(mixed, 4) != mixed)
        {
            failures += fail("rotation 4 did not wrap to the identity");
        }
        return failures;
    }

    // ---- Descriptor resolve --------------------------------------------

    // Unrotated 1:1 resolve with a zero origin — the player-owned-house shape.
    int checkIdentityResolve()
    {
        int failures = 0;
        constexpr int32_t gridW = 4;
        constexpr int32_t gridH = 4;
        std::vector<int32_t> grid = makeGrid(gridW, gridH);
        // Instance chunk (1, 2) on plane 0 was copied from source chunk (100, 200).
        setCell(grid, gridW, gridH, 0, 1, 2, packDescriptor(0, 100, 200, 0));

        InstanceMap map;
        map.assign(0, 0, gridW, gridH, grid.data(), grid.size());
        if (!map.isActive())
        {
            return fail("a well-formed grid did not activate the map");
        }
        // Tile (1*8 + 5, 2*8 + 3) sits at local (5, 3) of that chunk.
        const int64_t packed = map.sourceOfPacked(8 + 5, 16 + 3, 0);
        if (packed == InstanceMap::kNoSource)
        {
            return fail("identity resolve returned no source");
        }
        if (InstanceMap::srcTileX(packed) != 100 * 8 + 5
            || InstanceMap::srcTileY(packed) != 200 * 8 + 3
            || InstanceMap::srcPlane(packed) != 0
            || InstanceMap::srcRotation(packed) != 0)
        {
            std::printf("  FAIL: identity resolve gave (%d,%d,p%d,r%d), expected (%d,%d,p0,r0)\n",
                        InstanceMap::srcTileX(packed), InstanceMap::srcTileY(packed),
                        InstanceMap::srcPlane(packed), InstanceMap::srcRotation(packed),
                        100 * 8 + 5, 200 * 8 + 3);
            ++failures;
        }
        return failures;
    }

    // A rotated chunk must resolve through the client's own rotation switch:
    // local (x, y) reads source-local (y, 7 - x) at rotation 1.
    int checkRotatedResolve()
    {
        constexpr int32_t gridW = 2;
        constexpr int32_t gridH = 2;
        std::vector<int32_t> grid = makeGrid(gridW, gridH);
        setCell(grid, gridW, gridH, 0, 0, 0, packDescriptor(0, 50, 60, 1));

        InstanceMap map;
        map.assign(0, 0, gridW, gridH, grid.data(), grid.size());
        const int64_t packed = map.sourceOfPacked(5, 3, 0);   // local (5, 3)
        if (packed == InstanceMap::kNoSource)
        {
            return fail("rotated resolve returned no source");
        }
        const int32_t expectX = 50 * 8 + 3;        // rotateLocalX(5, 3, 1) == 3
        const int32_t expectY = 60 * 8 + (7 - 5);  // rotateLocalY(5, 3, 1) == 2
        if (InstanceMap::srcTileX(packed) != expectX
            || InstanceMap::srcTileY(packed) != expectY
            || InstanceMap::srcRotation(packed) != 1)
        {
            std::printf("  FAIL: rotated resolve gave (%d,%d,r%d), expected (%d,%d,r1)\n",
                        InstanceMap::srcTileX(packed), InstanceMap::srcTileY(packed),
                        InstanceMap::srcRotation(packed), expectX, expectY);
            return 1;
        }
        return 0;
    }

    // THE UNITS TRAP. originMap* are mapsquares, gridW/gridH are chunks, so the
    // origin must be promoted by << 3 before it is subtracted from a chunk
    // index. Drop that shift and the tile below lands far outside the grid and
    // resolves to nothing — which is exactly what this asserts against.
    int checkUnitsTrap()
    {
        constexpr int32_t originMapX = 40;   // MAPSQUARES -> chunk 320 -> tile 2560
        constexpr int32_t originMapY = 50;   // MAPSQUARES -> chunk 400 -> tile 3200
        constexpr int32_t gridW = 8;
        constexpr int32_t gridH = 8;
        std::vector<int32_t> grid = makeGrid(gridW, gridH);
        setCell(grid, gridW, gridH, 0, 0, 0, packDescriptor(0, 10, 20, 0));

        InstanceMap map;
        map.assign(originMapX, originMapY, gridW, gridH, grid.data(), grid.size());
        const int64_t packed = map.sourceOfPacked(2560 + 1, 3200 + 2, 0);
        if (packed == InstanceMap::kNoSource)
        {
            return fail("units trap: origin was not promoted from mapsquares to chunks");
        }
        if (InstanceMap::srcTileX(packed) != 10 * 8 + 1
            || InstanceMap::srcTileY(packed) != 20 * 8 + 2)
        {
            return fail("units trap: promoted origin resolved to the wrong source tile");
        }
        // One chunk short of the origin is outside the grid, not wrapped into it.
        if (map.sourceOfPacked(2560 - 1, 3200 + 2, 0) != InstanceMap::kNoSource)
        {
            return fail("units trap: a tile before the origin resolved to a source");
        }
        return 0;
    }

    // Holes, out-of-range planes and out-of-grid tiles are all routine "no
    // source" answers rather than errors or garbage coordinates.
    int checkHolesAndBounds()
    {
        int failures = 0;
        constexpr int32_t gridW = 2;
        constexpr int32_t gridH = 2;
        std::vector<int32_t> grid = makeGrid(gridW, gridH);   // every cell a hole
        setCell(grid, gridW, gridH, 1, 0, 0, packDescriptor(1, 7, 8, 0));

        InstanceMap map;
        map.assign(0, 0, gridW, gridH, grid.data(), grid.size());

        if (map.sourceOfPacked(0, 0, 0) != InstanceMap::kNoSource)
        {
            failures += fail("a hole descriptor resolved to a source");
        }
        if (map.sourceOfPacked(0, 0, 4) != InstanceMap::kNoSource
            || map.sourceOfPacked(0, 0, -1) != InstanceMap::kNoSource)
        {
            failures += fail("an out-of-range plane resolved to a source");
        }
        if (map.sourceOfPacked(16, 0, 1) != InstanceMap::kNoSource)
        {
            failures += fail("a tile past the grid width resolved to a source");
        }
        if (map.coversTile(16, 0, 1) || !map.coversTile(0, 0, 1))
        {
            failures += fail("coversTile disagreed with the grid footprint");
        }
        // Plane-major indexing: the same (gx, gy) on a different plane is a
        // different cell. Plane 1 is populated, plane 0 is not.
        if (map.sourceOfPacked(0, 0, 1) == InstanceMap::kNoSource)
        {
            failures += fail("plane-major indexing missed a populated cell");
        }
        return failures;
    }

    // Entries past the required 4 * gridW * gridH must never be consulted.
    // Append a distinctive descriptor beyond the end and confirm no tile in the
    // whole footprint reaches it.
    int checkStaleTailUnread(const std::vector<int32_t> &grid, int32_t gridW, int32_t gridH)
    {
        std::vector<int32_t> padded = grid;
        padded.push_back(packDescriptor(3, 1023, 2047, 3));
        InstanceMap map;
        map.assign(0, 0, gridW, gridH, padded.data(), padded.size());
        for (int32_t plane = 0; plane < 4; ++plane)
        {
            for (int32_t tileY = 0; tileY < gridH * 8; ++tileY)
            {
                for (int32_t tileX = 0; tileX < gridW * 8; ++tileX)
                {
                    const int64_t packed = map.sourceOfPacked(tileX, tileY, plane);
                    if (packed != InstanceMap::kNoSource
                        && InstanceMap::srcTileX(packed) == 1023 * 8)
                    {
                        return fail("a tile resolved through a stale tail entry");
                    }
                }
            }
        }
        return 0;
    }

    // The agent leaves the tail of its published chunk array stale rather than
    // clearing it every tick, so a grid shorter than 4 * gridW * gridH must be
    // refused outright — installing it partially would resolve tiles through the
    // PREVIOUS instance's descriptors.
    int checkCountDiscipline()
    {
        int failures = 0;
        constexpr int32_t gridW = 4;
        constexpr int32_t gridH = 4;
        const std::size_t required = static_cast<std::size_t>(4 * gridW * gridH);

        std::vector<int32_t> grid = makeGrid(gridW, gridH);
        setCell(grid, gridW, gridH, 0, 0, 0, packDescriptor(0, 1, 1, 0));

        InstanceMap shortMap;
        shortMap.assign(0, 0, gridW, gridH, grid.data(), required - 1);
        if (shortMap.isActive())
        {
            failures += fail("a short descriptor run was installed instead of refused");
        }

        failures += checkStaleTailUnread(grid, gridW, gridH);

        // An origin outside the addressable band is refused, not wrapped. A raw
        // `originMapX << 3` on a value this large truncates to a small chunk
        // index, which would resolve tiles near the wrapped position to
        // real-looking source chunks.
        InstanceMap wrappedMap;
        wrappedMap.assign(1 << 28, 0, gridW, gridH, grid.data(), grid.size());
        if (wrappedMap.isActive())
        {
            failures += fail("an origin past the addressable band was installed");
        }
        // Likewise a grid whose footprint would run off the end of the world:
        // WorldView's visited stamps stop at the same band, so accepting it
        // would make clipAt and the closed-set disagree. The last mapsquare
        // (255) starts at chunk 2040 and the band ends at 2048, so a grid wider
        // than 8 chunks there overhangs — while a narrow one at the same origin
        // is perfectly legal and must still be accepted.
        constexpr int32_t lastMapsquare = InstanceMap::kMapsquaresPerAxis - 1;
        constexpr int32_t overhangW = 16;
        std::vector<int32_t> wideGrid = makeGrid(overhangW, gridH);
        InstanceMap overhangMap;
        overhangMap.assign(lastMapsquare, 0, overhangW, gridH,
                           wideGrid.data(), wideGrid.size());
        if (overhangMap.isActive())
        {
            failures += fail("a grid overhanging the addressable band was installed");
        }
        InstanceMap edgeMap;
        edgeMap.assign(lastMapsquare, 0, gridW, gridH, grid.data(), grid.size());
        if (!edgeMap.isActive())
        {
            failures += fail("a legal grid in the last mapsquare was refused");
        }

        InstanceMap nullMap;
        nullMap.assign(0, 0, gridW, gridH, nullptr, 0);
        if (nullMap.isActive())
        {
            failures += fail("a null descriptor pointer produced an active map");
        }
        // clear() returns an installed map to the static-scene state — the
        // property SearchContext::recycle relies on so a recycled context cannot
        // inherit the previous borrower's instance.
        InstanceMap installed;
        installed.assign(0, 0, gridW, gridH, grid.data(), grid.size());
        if (!installed.isActive())
        {
            failures += fail("a well-formed grid failed to install");
        }
        installed.clear();
        if (installed.isActive()
            || installed.sourceOfPacked(0, 0, 0) != InstanceMap::kNoSource)
        {
            failures += fail("clear() left the map active");
        }
        return failures;
    }
}

int runInstanceTests()
{
    std::printf("instance: dynamic-region resolve + wall rotation\n");
    int failures = 0;
    failures += checkRotationGeometry();
    failures += checkRotationInvariants();
    failures += checkIdentityResolve();
    failures += checkRotatedResolve();
    failures += checkUnitsTrap();
    failures += checkHolesAndBounds();
    failures += checkCountDiscipline();

    if (failures == 0)
    {
        std::printf("instance: all checks passed\n");
        return 0;
    }
    std::printf("instance: %d check(s) FAILED\n", failures);
    return 1;
}
