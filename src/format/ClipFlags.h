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
}

#endif  // WORLDWALKER_FORMAT_CLIPFLAGS_H
