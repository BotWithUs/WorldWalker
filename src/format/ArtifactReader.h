#ifndef WORLDWALKER_FORMAT_ARTIFACTREADER_H
#define WORLDWALKER_FORMAT_ARTIFACTREADER_H

#include "format/Artifact.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ww::format
{
    // Header metadata surfaced to the runtime. formatVersion is a hard gate
    // (construction throws on mismatch); cacheRevision/datasetHash let a caller
    // soft-warn that the artifact predates the running cache or datasets.
    struct ArtifactInfo
    {
        uint32_t formatVersion{};
        uint32_t cacheRevision{};
        uint32_t datasetHash{};
    };

    // Loads a baked WorldWalker artifact into memory once, validates its header
    // and section directory, and exposes typed, aligned views of every section.
    //
    // The small POD sections (transitions, area nodes/edges, teleport boxes) are
    // decoded into owned, aligned arrays at construction via memcpy, because the
    // on-disk section offsets are not guaranteed to satisfy a record's natural
    // alignment. The large zlib-compressed grids (per-square collision clips and
    // per-(square,plane) area-id grids) stay compressed and decompress on demand;
    // the small ALT distance tables, needed by every query, are decompressed
    // eagerly. The whole file is retained so the on-demand decompressors can read
    // their blobs without a second open.
    //
    // Construction throws std::runtime_error on I/O failure, a bad magic or
    // format-version mismatch, or a malformed / overflowing section directory.
    // Move-only; immutable after construction and safe to share across threads —
    // the on-demand decompressors allocate fresh buffers and touch no shared
    // mutable state.
    class ArtifactReader
    {
    public:
        explicit ArtifactReader(const std::string &path);

        ArtifactReader(const ArtifactReader &) = delete;
        ArtifactReader &operator=(const ArtifactReader &) = delete;
        ArtifactReader(ArtifactReader &&) = default;
        ArtifactReader &operator=(ArtifactReader &&) = default;

        const ArtifactInfo &info() const
        {
            return metadata;
        }

        bool hasSection(SectionId id) const
        {
            return sectionPresent[static_cast<std::size_t>(id)];
        }

        // ---- Collision ------------------------------------------------------
        bool hasCollision() const
        {
            return hasSection(SectionId::Collision);
        }

        std::span<const CollisionSquareEntry> collisionSquares() const
        {
            return {collisionSquareTable.data(), collisionSquareTable.size()};
        }

        // Decompress one map square's clip words into outWords (resized to
        // kClipWordsPerSquare). Returns false if the square is absent; throws
        // std::runtime_error on a corrupt blob.
        bool decompressSquare(int squareX, int squareY, std::vector<uint32_t> &outWords) const;

        // ---- Transitions ----------------------------------------------------
        bool hasTransitions() const
        {
            return hasSection(SectionId::Transitions);
        }

        std::span<const TransitionRecord> transitions() const
        {
            return {transitionTable.data(), transitionTable.size()};
        }

        std::span<const RequirementRecord> requirements() const
        {
            return {requirementPool.data(), requirementPool.size()};
        }

        std::span<const ChainStepRecord> chainSteps() const
        {
            return {chainStepPool.data(), chainStepPool.size()};
        }

        // ---- Abstraction (area graph) ---------------------------------------
        bool hasAbstraction() const
        {
            return hasSection(SectionId::Abstraction);
        }

        std::span<const AreaNodeRecord> areaNodes() const
        {
            return {areaNodeTable.data(), areaNodeTable.size()};
        }

        std::span<const AreaEdgeRecord> areaEdges() const
        {
            return {areaEdgeTable.data(), areaEdgeTable.size()};
        }

        std::span<const AreaGridEntry> areaGrids() const
        {
            return {areaGridTable.data(), areaGridTable.size()};
        }

        // Decompress one (square, plane) area-id grid into outIds (resized to
        // kClipSize*kClipSize). Returns false if absent; throws on a corrupt blob.
        bool decompressGrid(int squareX, int squareY, int plane, std::vector<int32_t> &outIds) const;

        // ---- ALT landmarks --------------------------------------------------
        bool hasAltLandmarks() const
        {
            return hasSection(SectionId::AltLandmarks);
        }

        uint32_t landmarkCount() const
        {
            return altLandmarkCount;
        }

        uint32_t altAreaCount() const
        {
            return altAreas;
        }

        std::span<const int32_t> landmarkAreas() const
        {
            return {landmarkAreaList.data(), landmarkAreaList.size()};
        }

        // Lower-bound tick cost from landmark `landmarkIndex` to `area`
        // (fromLandmark table) and from `area` to the landmark (toLandmark
        // table). Unreachable pairs are +infinity. Indices are not bounds-checked.
        float distFromLandmark(uint32_t landmarkIndex, uint32_t area) const
        {
            return fromLandmarkData[static_cast<std::size_t>(landmarkIndex) * altAreas + area];
        }

        float distToLandmark(uint32_t area, uint32_t landmarkIndex) const
        {
            return toLandmarkData[static_cast<std::size_t>(landmarkIndex) * altAreas + area];
        }

        // ---- Teleport-allowed -----------------------------------------------
        bool hasTeleportZones() const
        {
            return hasSection(SectionId::TeleportAllowed);
        }

        uint32_t wildernessCutoff() const
        {
            return teleportCutoff;
        }

        std::span<const WildernessRegion> wildernessRegions() const
        {
            return {wildernessList.data(), wildernessList.size()};
        }

        std::span<const NoTeleZone> noTeleZones() const
        {
            return {noTeleList.data(), noTeleList.size()};
        }

    private:
        void parseDirectory();
        void decodeSection(const SectionEntry &entry);
        void decodeCollision(const SectionEntry &entry);
        void decodeTransitions(const SectionEntry &entry);
        void decodeAbstraction(const SectionEntry &entry);
        void decodeAltLandmarks(const SectionEntry &entry);
        void decodeTeleportAllowed(const SectionEntry &entry);
        std::vector<float> decompressFloatTable(const AltTableDescriptor &desc, uint64_t sectionOffset) const;

        std::vector<uint8_t> bytes;
        ArtifactInfo metadata{};
        std::array<bool, 8> sectionPresent{};

        std::vector<CollisionSquareEntry> collisionSquareTable;
        std::unordered_map<uint32_t, std::size_t> collisionIndex;
        uint64_t collisionSectionOffset{};

        std::vector<TransitionRecord> transitionTable;
        std::vector<RequirementRecord> requirementPool;
        std::vector<ChainStepRecord> chainStepPool;

        std::vector<AreaNodeRecord> areaNodeTable;
        std::vector<AreaEdgeRecord> areaEdgeTable;
        std::vector<AreaGridEntry> areaGridTable;
        std::unordered_map<uint32_t, std::size_t> areaGridIndex;
        uint64_t abstractionSectionOffset{};

        std::vector<int32_t> landmarkAreaList;
        std::vector<float> fromLandmarkData;
        std::vector<float> toLandmarkData;
        uint32_t altLandmarkCount{};
        uint32_t altAreas{};

        std::vector<WildernessRegion> wildernessList;
        std::vector<NoTeleZone> noTeleList;
        uint32_t teleportCutoff{};
    };
}

#endif  // WORLDWALKER_FORMAT_ARTIFACTREADER_H
