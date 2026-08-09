#ifndef WORLDWALKER_FORMAT_CLIPFLAGS_H
#define WORLDWALKER_FORMAT_CLIPFLAGS_H

#include <cstdint>

// WorldWalker-side mirror of the directional clip-word contract produced by
// NXTCacheLibrary's nxt_get_mapsquare_clip (maps::ClipFlag). The cache getter
// hands back opaque uint32 words; this enum is the agreed meaning of their bits.
// It is duplicated here on purpose so WorldWalker does not pull in NXTCache's
// internal headers — keep it byte-for-byte identical to maps::ClipFlag, and if
// the producer's bit layout ever changes, change it here in the same logical
// commit (the same producer/consumer discipline the wider stack follows).
//
// Wall edges are stored on the tile they belong to AND reflected onto the
// adjacent tile, except reflections that fall outside a square's 64x64 grid are
// dropped. A move across an edge is therefore blocked when EITHER endpoint
// carries the corresponding wall bit; callers must check both sides per step.
namespace ww::format
{
    enum ClipFlag : uint32_t
    {
        CLIP_OPEN             = 0x0,
        CLIP_WALL_NW          = 0x1,
        CLIP_WALL_N           = 0x2,
        CLIP_WALL_NE          = 0x4,
        CLIP_WALL_E           = 0x8,
        CLIP_WALL_SE          = 0x10,
        CLIP_WALL_S           = 0x20,
        CLIP_WALL_SW          = 0x40,
        CLIP_WALL_W           = 0x80,
        CLIP_OBJECT           = 0x100,
        CLIP_FLOOR_DECORATION = 0x40000,
        CLIP_FLOOR            = 0x200000,
        CLIP_BLOCKED          = 0x1000000,
        CLIP_WATER            = 0x2000000,
        CLIP_DOOR             = 0x4000000,
        CLIP_AGILITY_SHORTCUT = 0x8000000,
        CLIP_PLANE_CHANGE     = 0x10000000,
        CLIP_CLIMBOVER        = 0x20000000,
    };

    // A tile is standable when no whole-tile blocker occupies it. Wall-edge bits
    // do not block standing — only crossing the edge — so they are excluded here.
    inline constexpr uint32_t kClipStandBlockedMask =
        static_cast<uint32_t>(CLIP_BLOCKED) | static_cast<uint32_t>(CLIP_OBJECT);

    // The eight directional wall bits occupy the low byte, in clockwise order
    // starting at NW: NW, N, NE, E, SE, S, SW, W. Every other clip bit describes
    // the whole tile and has no orientation.
    inline constexpr uint32_t kClipWallMask = 0xFFu;

    // Re-express a SOURCE tile's clip word as it appears inside a dynamic region
    // (instance) whose chunk was copied with `rotation` 90-degree steps. Only the
    // directional wall bits move; whole-tile bits (blocked, object, water, door,
    // …) pass through untouched.
    //
    // One 90-degree step is two bit positions, because the bit order above is
    // clockwise in 45-degree increments.
    //
    // DIRECTION — the part worth deriving rather than guessing. The client's
    // chunk rotation maps instance-local (x, y) back to source-local (y, 7 - x)
    // at rotation 1 (see InstanceMap::rotateLocalX / rotateLocalY), so the
    // source's EAST edge surfaces as the instance's NORTH edge: a 90-degree
    // counter-clockwise turn, which is a RIGHT rotate of a clockwise-ordered
    // field. East is bit 3 and north is bit 1, so the shift is >> 2 per step.
    //
    // Getting this backwards yields collision that is wrong only on rotated
    // chunks — invisible in a player-owned house, where every chunk is copied
    // unrotated. So it is pinned rather than trusted: `wwcli instance`
    // (cli/InstanceTests.cpp, checkRotationGeometry) derives the expected result
    // from InstanceMap::rotateLocalX/Y instead of restating the shift, so this
    // function and the tile rotation cannot silently disagree. Note that is a
    // different suite from `wwcli walltest`, which covers loc wall SHAPES and
    // has nothing to say about chunk rotation.
    inline constexpr uint32_t rotateClipWord(uint32_t word, int32_t rotation)
    {
        const int32_t steps = (rotation & 0x3) * 2;
        if (steps == 0)
        {
            return word;
        }
        const uint32_t walls = word & kClipWallMask;
        const uint32_t rotated = ((walls >> steps) | (walls << (8 - steps))) & kClipWallMask;
        return (word & ~kClipWallMask) | rotated;
    }
}

#endif  // WORLDWALKER_FORMAT_CLIPFLAGS_H
