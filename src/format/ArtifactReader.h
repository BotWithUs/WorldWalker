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

        // Pre-indexed list of every Global-origin transition (spell /
        // lodestone / item-teleport). PathAssembler walks this on every
        // query to seed the global-teleport frontier — the full transitions
        // span is 15k+ on a real artifact, and the prior linear scan paid
        // an O(transitions) cost per query when typically only a handful
        // are global-origin. The index is rebuilt whenever the transition
        // table changes (decodeTransitions / appendTransitions /
        // truncateToBaked) so it always reflects the live table.
        std::span<const uint32_t> globalOriginTransitions() const
        {
            return {globalOriginIndices.data(), globalOriginIndices.size()};
        }

        // Distinct ids referenced by any RequirementRecord of any transition.
        // The executor reads the live varbit value and item count for each id
        // on every (re-)plan so requirement-gated teleports can be admitted on
        // mid-walk state changes (an item picked up, a lodestone newly
        // unlocked). The lists are rebuilt in lockstep with the requirement
        // pool whenever the transition table changes (decodeTransitions /
        // appendTransitions / truncateToBaked), so the Executor can borrow
        // these spans without rescanning the pool on every ww_executor_run.
        std::span<const int32_t> requirementVarbitIds() const
        {
            return {requirementVarbitIdList.data(), requirementVarbitIdList.size()};
        }

        std::span<const int32_t> requirementItemIds() const
        {
            return {requirementItemIdList.data(), requirementItemIdList.size()};
        }

        // ---- Runtime teleports (appended after bake) ------------------------
        // Global teleports (spell + lodestone) are loaded from editable JSON at
        // runtime and appended onto the baked transition / requirement / chain
        // pools. Globals never appear in baked AreaEdgeRecords (those reference
        // only the baked prefix), so appending past the baked count is safe and
        // is picked up automatically by the planner's frontier seeding and by
        // the executor (both index the full transitions() / chainSteps() spans).
        //
        // NOT thread-safe with concurrent query / execution on their own; the
        // C ABI (ww_artifact_load_teleports) serialises them against ww_query /
        // ww_executor_run with the artifact handle's lifecycle lock, so C++
        // callers that bypass the ABI must do the same.

        // Append POD records onto the owned pools. Each appended TransitionRecord
        // must already carry requirementStart / chainStart offsets relative to
        // the pools' CURRENT sizes (i.e. the post-truncate state).
        void appendTransitions(std::span<const TransitionRecord> transitions,
                               std::span<const RequirementRecord> requirements,
                               std::span<const ChainStepRecord> chainSteps);

        // Drop everything appended since load, restoring the baked prefix. Makes
        // a reload (truncate + re-append) idempotent.
        void truncateToBaked();

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

        // Indices into areaEdges() of every baked area edge whose underlying
        // TransitionRecord's destination tile falls inside the (squareX,
        // squareY) map square on `destPlane`, and whose transition is NOT
        // global-origin. PathAssembler's near-goal-exit scan enumerates the
        // 3x3 squares around the goal and concatenates these buckets, turning
        // a per-query O(areaEdges) sweep (~14k entries) into O(matching ~tens).
        //
        // Stable for the artifact's life: only baked edges reference baked
        // transitions, and runtime-appended teleports (appendTransitions) are
        // global-origin and therefore excluded by construction.
        std::span<const uint32_t> nearGoalEdgeBucket(int destPlane,
                                                     int destSquareX,
                                                     int destSquareY) const;

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
        //
        // Storage is area-major: the on-disk format is landmark-major
        // (table[landmark * areaCount + area]) but decodeAltLandmarks
        // transposes it in place so a per-area read of every landmark column
        // — the AltHeuristic::estimate hot loop — touches one contiguous
        // landmark-wide window instead of striding across the whole table.
        float distFromLandmark(uint32_t landmarkIndex, uint32_t area) const
        {
            return fromLandmarkData[static_cast<std::size_t>(area) * altLandmarkCount + landmarkIndex];
        }

        float distToLandmark(uint32_t area, uint32_t landmarkIndex) const
        {
            return toLandmarkData[static_cast<std::size_t>(area) * altLandmarkCount + landmarkIndex];
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
        // Rescan transitionTable and rebuild globalOriginIndices. Called
        // after any change to the transition pool (decodeTransitions,
        // appendTransitions, truncateToBaked).
        void rebuildGlobalOriginIndex();

        // Rescan requirementPool and rebuild the distinct varbit / item id
        // lists. Called after any change to the requirement pool — same
        // entry points as rebuildGlobalOriginIndex.
        void rebuildRequirementIdLists();

        // Build the near-goal edge bucket index from the baked area edges +
        // baked transition table. Called once at end of construction, after
        // both abstraction and transitions sections have been decoded; the
        // bucket is then immutable for the artifact's life (runtime appends
        // never affect it — see nearGoalEdgeBucket()).
        void buildNearGoalEdgeBuckets();

        std::vector<uint8_t> bytes;
        ArtifactInfo metadata{};
        std::array<bool, 8> sectionPresent{};

        std::vector<CollisionSquareEntry> collisionSquareTable;
        std::unordered_map<uint32_t, std::size_t> collisionIndex;
        uint64_t collisionSectionOffset{};

        std::vector<TransitionRecord> transitionTable;
        std::vector<RequirementRecord> requirementPool;
        std::vector<ChainStepRecord> chainStepPool;
        // Indices into transitionTable for every global-origin record. Kept
        // in lockstep with transitionTable via rebuildGlobalOriginIndex
        // (called after decode and any append/truncate).
        std::vector<uint32_t> globalOriginIndices;
        // Distinct varbit / item ids referenced by any requirement record in
        // the pool. Kept in lockstep with requirementPool via
        // rebuildRequirementIdLists. The Executor reads each per re-plan, so
        // a per-run rescan would otherwise cost an O(requirements) sweep on
        // every ww_executor_run call.
        std::vector<int32_t> requirementVarbitIdList;
        std::vector<int32_t> requirementItemIdList;
        // Baked prefix lengths, captured after decodeTransitions; runtime
        // teleport appends sit past these and truncateToBaked() rewinds to them.
        std::size_t bakedTransitionCount{};
        std::size_t bakedRequirementCount{};
        std::size_t bakedChainCount{};

        std::vector<AreaNodeRecord> areaNodeTable;
        std::vector<AreaEdgeRecord> areaEdgeTable;
        std::vector<AreaGridEntry> areaGridTable;
        std::unordered_map<uint32_t, std::size_t> areaGridIndex;
        uint64_t abstractionSectionOffset{};

        // CSR-style index of area edges grouped by their underlying
        // transition's destination (destPlane, destSquareX, destSquareY).
        // nearGoalBucketFirst has size (kClipPlanes * kSquaresPerAxis^2) + 1
        // (the +1 is a sentinel; bucket k's edges live in [first[k], first[k+1])).
        // nearGoalBucketEdges holds those edge indices in bucket order.
        // Built once by buildNearGoalEdgeBuckets() at end of construction.
        std::vector<uint32_t> nearGoalBucketFirst;
        std::vector<uint32_t> nearGoalBucketEdges;

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
