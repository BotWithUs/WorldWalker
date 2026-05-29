#ifndef WORLDWALKER_FORMAT_ARTIFACT_H
#define WORLDWALKER_FORMAT_ARTIFACT_H

#include <cstdint>

// On-disk layout of the baked WorldWalker artifact. POD structs only; every
// integer is little-endian (the sole target is x64 Windows). The writer
// (wwbuild) and the runtime reader (worldwalker) both include this header, so
// any layout change is a single source of truth. A schema-breaking change must
// bump kArtifactFormatVersion so the loader fails loud rather than misreading.
namespace ww::format
{
    // 'W','W','A','L' as a little-endian uint32.
    inline constexpr uint32_t kArtifactMagic = 0x4C415757u;

    // Keep in lockstep with WW_ARTIFACT_FORMAT_VERSION in the C ABI header.
    inline constexpr uint32_t kArtifactFormatVersion = 1u;

    // Section identifiers. Each build phase fills in its own section; the file
    // order is not significant because the directory carries explicit offsets.
    enum class SectionId : uint32_t
    {
        Collision       = 1,   // directional clip words per populated map square
        Transitions     = 2,   // reserved — transitions ingest sub-step
        Abstraction     = 3,   // reserved — abstraction graph sub-step
        AltLandmarks    = 4,   // reserved — ALT landmark tables sub-step
        TeleportAllowed = 5,   // reserved — teleport-allowed map sub-step
    };

    // File header at offset 0, fixed 64 bytes. cacheRevision + datasetHash let
    // the runtime soft-warn on a stale artifact; formatVersion is a hard gate.
    struct ArtifactHeader
    {
        uint32_t magic;          // kArtifactMagic
        uint32_t formatVersion;  // kArtifactFormatVersion
        uint32_t cacheRevision;  // RS build the collision was decoded from (0 = unknown)
        uint32_t datasetHash;    // hash of the transition datasets (0 until ingested)
        uint32_t sectionCount;   // number of SectionEntry records following the header
        uint32_t reserved[11];   // zero-filled padding to 64 bytes
    };

    // One directory entry per present section, packed immediately after the
    // header. offset/length are absolute byte positions in the file.
    struct SectionEntry
    {
        uint32_t id;        // SectionId
        uint32_t reserved;  // alignment / future per-section flags
        uint64_t offset;    // absolute byte offset of the section payload
        uint64_t length;    // byte length of the section payload
    };

    // ---- Collision section ---------------------------------------------------
    //
    // Payload layout (all offsets relative to the section start):
    //   CollisionSectionHeader
    //   CollisionSquareEntry[squareCount]   (sorted ascending by squareY, then squareX)
    //   <blob region>                       (zlib streams, one per square)
    //
    // Each square's uncompressed payload is exactly kClipWordsPerSquare uint32
    // clip words — plane-major, then x (west-east 0..63), then y (south-north
    // 0..63) — i.e. the raw nxt_get_mapsquare_clip output. All-zero planes are
    // included; they cost almost nothing once zlib-compressed, and planeMask is
    // an optimization hint for the runtime rather than a layout change.

    inline constexpr int kClipPlanes = 4;
    inline constexpr int kClipSize   = 64;
    inline constexpr uint32_t kClipWordsPerSquare =
        static_cast<uint32_t>(kClipPlanes) * kClipSize * kClipSize;  // 16384

    struct CollisionSectionHeader
    {
        uint32_t squareCount;
        uint32_t reserved;   // alignment / future flags
    };

    struct CollisionSquareEntry
    {
        uint16_t squareX;     // 0..127
        uint16_t squareY;     // 0..(archiveId >> 7)
        uint8_t  planeMask;   // bit p set if plane p holds any non-zero tile
        uint8_t  pad[3];      // zero-filled
        uint32_t blobOffset;  // byte offset (within the section) of this square's zlib stream
        uint32_t blobLength;  // compressed byte length
        uint32_t rawLength;   // uncompressed byte length (== kClipWordsPerSquare * 4)
    };

    static_assert(sizeof(ArtifactHeader) == 64, "ArtifactHeader must be 64 bytes");
    static_assert(sizeof(SectionEntry) == 24, "SectionEntry must be 24 bytes");
    static_assert(sizeof(CollisionSectionHeader) == 8, "CollisionSectionHeader must be 8 bytes");
    static_assert(sizeof(CollisionSquareEntry) == 20, "CollisionSquareEntry must be 20 bytes");
}

#endif  // WORLDWALKER_FORMAT_ARTIFACT_H
