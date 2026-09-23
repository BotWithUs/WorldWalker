#ifndef WORLDWALKER_FORMAT_ARTIFACT_H
#define WORLDWALKER_FORMAT_ARTIFACT_H

#include <bit>
#include <cstdint>

// On-disk layout of the baked WorldWalker artifact. POD structs only; every
// integer is little-endian (the sole target is x64 Windows). The writer
// (wwbuild) and the runtime reader (worldwalker) both include this header, so
// any layout change is a single source of truth. A schema-breaking change must
// bump kArtifactFormatVersion so the loader fails loud rather than misreading.
//
// The byte order is intentionally not portable: the writer and reader memcpy
// PODs verbatim, so an attempt to build or load on a big-endian host would
// produce silent corruption. Fail loud at compile time instead.
static_assert(std::endian::native == std::endian::little,
              "WorldWalker artifact format is little-endian only "
              "(see writer/reader memcpy paths in ArtifactWriter / ArtifactReader)");

namespace ww::format
{
    // 'W','W','A','L' as a little-endian uint32.
    inline constexpr uint32_t kArtifactMagic = 0x4C415757u;

    // Keep in lockstep with WW_ARTIFACT_FORMAT_VERSION in the C ABI header.
    // v2: ChainStepRecord gained `d` (chain steps are now generic queued
    // actions: actionId + param1..3 — see ChainStepKind).
    // v3: ChainStepRecord gained `e..i` (nine generic slots) for the richer
    // chain kinds WaitInterface / DialogueSelect / ClickItem.
    inline constexpr uint32_t kArtifactFormatVersion = 3u;

    // Section identifiers. Each build phase fills in its own section; the file
    // order is not significant because the directory carries explicit offsets.
    enum class SectionId : uint32_t
    {
        Collision       = 1,   // directional clip words per populated map square
        Transitions     = 2,   // reserved — transitions ingest sub-step
        Abstraction     = 3,   // reserved — abstraction graph sub-step
        AltLandmarks    = 4,   // ALT landmark distance tables over the area graph
        TeleportAllowed = 5,   // wilderness regions + curated no-teleport zones
        Provenance      = 6,   // what this artifact was baked from (JSON; see below)
        DialogZones     = 7,   // where walking raises a question, and its answers
    };

    // File header at offset 0, fixed 64 bytes. formatVersion is a hard gate.
    //
    // datasetHash is trustworthy: FNV-1a over the bytes of the dataset files that
    // were actually loaded (see ww::data::loadDatasets).
    //
    // cacheRevision is NOT trustworthy and must not be used to decide anything.
    // wwbuild derives it from the mtimes of `main_file_cache.dat2` / `.js5`, two
    // files that do not exist in an RS3 NXT cache directory (the NXT cache is a
    // set of `js5-<N>.jcache` SQLite databases), so both stamps read 0 and the
    // value collapses to a hash of the cache directory's own mtime — it moves
    // when the client merely runs, and can sit still when the map data changes.
    // The field is kept as-is so nothing downstream shifts behaviour, but the
    // honest, machine-independent fingerprint of the baked map data is
    // `collisionFingerprint` in the Provenance section. ADR 0005's soft-warn
    // wants that one.
    struct ArtifactHeader
    {
        uint32_t magic;          // kArtifactMagic
        uint32_t formatVersion;  // kArtifactFormatVersion
        uint32_t cacheRevision;  // legacy cache-dir mtime hash — see above, do not trust
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

    // ---- Transitions section -------------------------------------------------
    //
    // Payload layout (all offsets relative to the section start):
    //   TransitionSectionHeader
    //   TransitionRecord[transitionCount]
    //   RequirementRecord[requirementCount]   (one shared pool)
    //   ChainStepRecord[chainStepCount]       (one shared pool)
    //
    // Each TransitionRecord owns a contiguous run in the requirement pool
    // ([requirementStart, requirementStart + requirementCount)) and another in the
    // chain pool, so records stay fixed-size and the reader can index the section
    // directly without offset chasing. The section is stored uncompressed (small).

    struct TransitionSectionHeader
    {
        uint32_t transitionCount;
        uint32_t requirementCount;  // total entries in the requirement pool
        uint32_t chainStepCount;    // total entries in the chain-step pool
        uint32_t reserved;          // alignment / future flags
    };

    // bit0 of TransitionRecord.flags: origin is global (works from anywhere; the
    // origin* fields are unused for such a record).
    inline constexpr uint8_t kTransitionFlagGlobalOrigin = 0x1u;

    // Mirrors ww::data::Transition. kind is ww::data::TransitionKind; code is a
    // fairy-ring code, null-padded, all-zero when not applicable. For local-origin
    // kinds (flag bit0 clear) origin* is the interactable object's tile — reach an
    // adjacent walkable tile, then interact; the tile itself may be blocked.
    struct TransitionRecord
    {
        uint8_t  kind;
        uint8_t  flags;
        uint8_t  originPlane;
        uint8_t  destPlane;
        int32_t  originX;
        int32_t  originY;
        int32_t  destX;
        int32_t  destY;
        int32_t  objectId;
        uint8_t  shape;
        uint8_t  rotation;
        uint8_t  optionIndex;
        uint8_t  pad0;
        char     code[4];
        float    cost;
        float    costQuick;
        uint32_t requirementStart;
        uint32_t requirementCount;
        uint32_t chainStart;
        uint32_t chainCount;
    };

    struct RequirementRecord
    {
        uint8_t  kind;     // ww::data::RequirementKind
        uint8_t  pad[3];
        int32_t  id;
        int32_t  amount;   // skill level / item count / varbit|varp value
    };

    struct ChainStepRecord
    {
        uint8_t  kind;     // ww::data::ChainStepKind
        uint8_t  pad[3];
        // Nine generic slots; the meaning of each is keyed on `kind`
        // (see ww::data::ChainStepKind). Unused slots are zero.
        //   Click:          a=actionId, b..d=param1..3
        //   Wait:           a=ticks
        //   WaitInterface:  a=interfaceId
        //   DialogueSelect: a=interface, b=index, c=per_page, d=next_comp, e=wait_ticks
        //   ClickItem:      a..d=worn(iface,comp,opt,sub),
        //                   e..h=backpack(iface,comp,opt,sub), i=backpack_special
        int32_t  a;
        int32_t  b;
        int32_t  c;
        int32_t  d;
        int32_t  e;
        int32_t  f;
        int32_t  g;
        int32_t  h;
        int32_t  i;
    };

    // ---- Abstraction section -------------------------------------------------
    //
    // A flood-filled area graph over the collision grid. An "area" is a maximal
    // set of tiles on one plane reachable from one another by cardinal walking
    // (wall edges respected); within an area any tile reaches any other by
    // walking, so the planner only needs tile-level search inside a single area.
    // Areas on different planes are always distinct. The area id is the index of
    // its AreaNodeRecord (0..areaCount-1).
    //
    // Payload layout (all offsets relative to the section start):
    //   AbstractionSectionHeader
    //   AreaNodeRecord[areaCount]            (indexed by area id)
    //   AreaEdgeRecord[edgeCount]            (sorted by fromArea, then toArea)
    //   AreaGridEntry[gridCount]             (sorted by squareY, squareX, plane)
    //   <blob region>                        (zlib streams, one per grid entry)
    //
    // Edges are derived from the Transitions section: a local-origin transition
    // links the area(s) touching its origin tile to the area at its dest tile;
    // transitionIndex points back into TransitionRecord[]. Global-origin
    // transitions (teleports/spells) are not edges here — the planner seeds them
    // at the search frontier. Each grid blob is an int32 area id per tile of one
    // (square, plane), x-major then y (id -1 for blocked / unreachable tiles).

    struct AbstractionSectionHeader
    {
        uint32_t areaCount;
        uint32_t edgeCount;
        uint32_t gridCount;   // number of (square, plane) area-id grids
        uint32_t reserved;    // alignment / future flags
    };

    struct AreaNodeRecord
    {
        uint8_t  plane;
        uint8_t  pad[3];
        uint32_t tileCount;
        int32_t  centroidX;   // mean tile x (may itself be blocked)
        int32_t  centroidY;
        int32_t  minX;        // bounding box, inclusive
        int32_t  minY;
        int32_t  maxX;
        int32_t  maxY;
    };

    struct AreaEdgeRecord
    {
        int32_t  fromArea;
        int32_t  toArea;
        uint32_t transitionIndex;  // index into the Transitions section's TransitionRecord[]
        float    cost;             // transition tick cost (intra-area walk cost is estimated by the planner)
    };

    struct AreaGridEntry
    {
        uint16_t squareX;
        uint16_t squareY;
        uint8_t  plane;
        uint8_t  pad[3];
        uint32_t blobOffset;  // byte offset (within the section) of this grid's zlib stream
        uint32_t blobLength;  // compressed byte length
        uint32_t rawLength;   // uncompressed byte length (== kClipSize * kClipSize * 4)
    };

    // ---- ALT landmarks section -----------------------------------------------
    //
    // Landmark distance tables for the ALT (A*, Landmarks, Triangle-inequality)
    // heuristic over the area graph (Abstraction section). A small set of
    // landmark areas is chosen; Dijkstra from each precomputes, for every area,
    // the cost to and from that landmark over the baked AreaEdge graph
    // (point-to-point transitions; intra-area walk cost is omitted, so the
    // distances are lower bounds — exactly what an admissible heuristic needs).
    // Global teleports are not in this graph; the planner seeds them at the
    // frontier. The graph is directed, so two tables are stored: fromLandmark
    // (landmark -> area) and toLandmark (area -> landmark). Unreachable entries
    // are stored as +infinity.
    //
    // Payload layout (all offsets relative to the section start):
    //   AltLandmarksSectionHeader
    //   int32 landmarkArea[landmarkCount]    (area id of each landmark)
    //   AltTableDescriptor fromLandmark
    //   AltTableDescriptor toLandmark
    //   <blob region>                        (fromLandmark zlib stream, then toLandmark)
    //
    // Each decompressed table holds landmarkCount * areaCount float32 values in
    // landmark-major order: table[landmark * areaCount + area].

    struct AltLandmarksSectionHeader
    {
        uint32_t landmarkCount;
        uint32_t areaCount;     // must equal the Abstraction section's areaCount
        uint32_t reserved[2];
    };

    struct AltTableDescriptor
    {
        uint32_t blobOffset;  // byte offset (within the section) of the zlib stream
        uint32_t blobLength;  // compressed byte length
        uint32_t rawLength;   // uncompressed byte length (== landmarkCount * areaCount * 4)
        uint32_t reserved;
    };

    // ---- Teleport-allowed section --------------------------------------------
    //
    // Whether a Global teleport may be initiated from a Tile. Two inputs, both
    // axis-aligned tile boxes with inclusive bounds on planes [planeMin, planeMax]:
    //   * Wilderness regions — the wilderness level rises with y as
    //       level = ((y - baseY) / stepY) + baseLevel
    //     inside the box. A teleport is blocked where that level exceeds the
    //     teleport's wilderness cutoff. defaultWildernessCutoff (20 for the
    //     standard spellbook) is the single cutoff the runtime applies until a
    //     per-teleport cutoff Requirement exists.
    //   * No-teleport zones — teleporting is blocked unconditionally inside them,
    //     independent of wilderness level. A tuning list; may be empty.
    // Every other Tile is teleport-allowed. Stored uncompressed (a few boxes).
    //
    // Payload layout (all offsets relative to the section start):
    //   TeleportAllowedSectionHeader
    //   WildernessRegion[wildernessCount]
    //   NoTeleZone[noTeleCount]

    struct TeleportAllowedSectionHeader
    {
        uint32_t wildernessCount;
        uint32_t noTeleCount;
        uint32_t defaultWildernessCutoff;  // wilderness level at/below which standard teleports work
        uint32_t reserved;
    };

    struct WildernessRegion
    {
        int32_t  minX;       // bounding box, inclusive tile coordinates
        int32_t  minY;
        int32_t  maxX;
        int32_t  maxY;
        int32_t  baseY;      // y at which the level equals baseLevel
        int32_t  baseLevel;  // wilderness level at baseY (typically 1)
        int32_t  stepY;      // tiles of y per wilderness level (typically 8)
        uint8_t  planeMin;
        uint8_t  planeMax;
        uint8_t  pad[2];     // zero-filled
    };

    struct NoTeleZone
    {
        int32_t  minX;       // bounding box, inclusive tile coordinates
        int32_t  minY;
        int32_t  maxX;
        int32_t  maxY;
        uint8_t  planeMin;
        uint8_t  planeMax;
        uint8_t  pad[2];     // zero-filled
    };

    // ---- Provenance section --------------------------------------------------
    //
    // What this artifact was baked from, so "where did this file come from" has an
    // answer that does not depend on anyone's memory. Purely descriptive: no
    // runtime behaviour keys off it, and a reader that ignores it is correct.
    //
    // Payload layout (all offsets relative to the section start):
    //   ProvenanceSectionHeader
    //   char json[jsonLength]     (UTF-8, not null-terminated)
    //
    // The body is JSON rather than a POD struct on purpose: it is variable-length,
    // human-read far more often than machine-read, and expected to gain fields.
    // Freezing it into a versioned struct would buy nothing and cost a format bump
    // every time a field is added; `schema` covers the compatibility that matters.
    //
    // Adding this section needs NO kArtifactFormatVersion bump, and must not get
    // one: ArtifactReader has skipped unknown section ids since its first commit
    // (in-range-but-unmapped ids take the `default` arm, out-of-range ids return
    // early), so every reader ever shipped loads a provenanced artifact unchanged.
    // A format bump, by contrast, is a hard refuse that would strand every client
    // already in the field (ADR 0005). Verified by experiment, not just by reading:
    // injecting this section into the shipping artifact left the full wwcli harness
    // byte-identical. Note the reader bounds-checks the section entry BEFORE
    // skipping it, so the offset/length must still be written correctly.
    struct ProvenanceSectionHeader
    {
        uint32_t schema;      // provenance document schema (kProvenanceSchema)
        uint32_t jsonLength;  // byte length of the UTF-8 JSON body that follows
    };

    // Bumped when the provenance JSON's meaning changes incompatibly. Readers
    // should present an unrecognised schema as opaque rather than refuse the file.
    inline constexpr uint32_t kProvenanceSchema = 1u;

    // ---- DialogZones section --------------------------------------------------
    //
    // Boxes of tiles where walking can raise a conversation that asks a
    // question, each with the replies the executor may pick there, in order
    // (ww::data::DialogZone). Outside every zone the executor never picks an
    // option. Like Provenance, this section needs no format bump: a reader that
    // predates it skips the id, and an artifact without it has no zones.
    //
    // Payload layout (all offsets relative to the section start):
    //   DialogZonesSectionHeader
    //   DialogZoneRecord[zoneCount]
    //   DialogAnswerRecord[answerCount]   (the zones' answers, in zone order)

    struct DialogZonesSectionHeader
    {
        uint32_t zoneCount;
        uint32_t answerCount;
    };

    struct DialogZoneRecord
    {
        int32_t  minX;         // bounding box, inclusive tile coordinates
        int32_t  minY;
        int32_t  maxX;
        int32_t  maxY;
        uint32_t answerStart;  // first of this zone's DialogAnswerRecords
        uint16_t answerCount;
        uint8_t  planeMin;
        uint8_t  planeMax;
    };

    // One reply, UTF-8, zero-padded to 36 bytes: exactly the nine int32 slots
    // of the runChainStep call that hands it to the host.
    struct DialogAnswerRecord
    {
        char text[36];
    };

    static_assert(sizeof(ArtifactHeader) == 64, "ArtifactHeader must be 64 bytes");
    static_assert(sizeof(DialogZonesSectionHeader) == 8, "DialogZonesSectionHeader must be 8 bytes");
    static_assert(sizeof(DialogZoneRecord) == 24, "DialogZoneRecord must be 24 bytes");
    static_assert(sizeof(DialogAnswerRecord) == 36, "DialogAnswerRecord must be 36 bytes");
    static_assert(sizeof(ProvenanceSectionHeader) == 8, "ProvenanceSectionHeader must be 8 bytes");
    static_assert(sizeof(SectionEntry) == 24, "SectionEntry must be 24 bytes");
    static_assert(sizeof(CollisionSectionHeader) == 8, "CollisionSectionHeader must be 8 bytes");
    static_assert(sizeof(CollisionSquareEntry) == 20, "CollisionSquareEntry must be 20 bytes");
    static_assert(sizeof(TransitionSectionHeader) == 16, "TransitionSectionHeader must be 16 bytes");
    static_assert(sizeof(TransitionRecord) == 56, "TransitionRecord must be 56 bytes");
    static_assert(sizeof(RequirementRecord) == 12, "RequirementRecord must be 12 bytes");
    static_assert(sizeof(ChainStepRecord) == 40, "ChainStepRecord must be 40 bytes");
    static_assert(sizeof(AbstractionSectionHeader) == 16, "AbstractionSectionHeader must be 16 bytes");
    static_assert(sizeof(AreaNodeRecord) == 32, "AreaNodeRecord must be 32 bytes");
    static_assert(sizeof(AreaEdgeRecord) == 16, "AreaEdgeRecord must be 16 bytes");
    static_assert(sizeof(AreaGridEntry) == 20, "AreaGridEntry must be 20 bytes");
    static_assert(sizeof(AltLandmarksSectionHeader) == 16, "AltLandmarksSectionHeader must be 16 bytes");
    static_assert(sizeof(AltTableDescriptor) == 16, "AltTableDescriptor must be 16 bytes");
    static_assert(sizeof(TeleportAllowedSectionHeader) == 16, "TeleportAllowedSectionHeader must be 16 bytes");
    static_assert(sizeof(WildernessRegion) == 32, "WildernessRegion must be 32 bytes");
    static_assert(sizeof(NoTeleZone) == 20, "NoTeleZone must be 20 bytes");
}

#endif  // WORLDWALKER_FORMAT_ARTIFACT_H
