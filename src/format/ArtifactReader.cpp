#include "format/ArtifactReader.h"

#include "data/Transitions.h"
#include "format/Zlib.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace ww::format
{
    namespace
    {
        std::vector<uint8_t> readWholeFile(const std::string &path)
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream)
            {
                throw std::runtime_error("ArtifactReader: cannot open " + path);
            }
            const std::streamoff size = stream.tellg();
            if (size < 0)
            {
                throw std::runtime_error("ArtifactReader: cannot size " + path);
            }
            std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
            stream.seekg(0);
            stream.read(reinterpret_cast<char *>(bytes.data()), size);
            if (!stream)
            {
                throw std::runtime_error("ArtifactReader: short read on " + path);
            }
            return bytes;
        }

        // Throw unless [offset, offset + length) lies wholly within `size`.
        // The two-part test avoids unsigned overflow when offset is near SIZE_MAX.
        void requireRange(uint64_t offset, uint64_t length, std::size_t size, const char *what)
        {
            if (offset > size || length > size - offset)
            {
                throw std::runtime_error(std::string("ArtifactReader: ") + what + " out of bounds");
            }
        }

        template <typename T>
        T readPod(const std::vector<uint8_t> &bytes, uint64_t offset)
        {
            static_assert(std::is_trivially_copyable_v<T>, "readPod requires a trivially copyable type");
            T value{};
            std::memcpy(&value, bytes.data() + static_cast<std::size_t>(offset), sizeof(T));
            return value;
        }

        template <typename T>
        std::vector<T> readPodArray(const std::vector<uint8_t> &bytes, uint64_t offset, uint32_t count)
        {
            static_assert(std::is_trivially_copyable_v<T>, "readPodArray requires a trivially copyable type");
            std::vector<T> out(count);
            if (count != 0)
            {
                std::memcpy(out.data(), bytes.data() + static_cast<std::size_t>(offset),
                            static_cast<std::size_t>(count) * sizeof(T));
            }
            return out;
        }

        uint32_t squareKey(uint32_t squareX, uint32_t squareY)
        {
            return (squareY << 16) | (squareX & 0xFFFFu);
        }

        uint32_t gridKey(uint32_t squareX, uint32_t squareY, uint32_t plane)
        {
            return (squareY << 16) | ((squareX & 0xFFFu) << 4) | (plane & 0xFu);
        }

        // The northern RS3 landmass reaches squareY ~200, so an 8-bit axis
        // (0..255) covers every populated square without overshoot. Bucket
        // key: (destPlane << 16) | (destSquareY << 8) | destSquareX; bucket
        // table is 256*256*4 = 262144 entries (1 MB of uint32 offsets).
        constexpr int kSquaresPerAxis = 256;
        constexpr int kNearGoalBucketCount =
            kSquaresPerAxis * kSquaresPerAxis * kClipPlanes;

        bool inSquareRange(int v)
        {
            return v >= 0 && v < kSquaresPerAxis;
        }

        uint32_t nearGoalBucketKey(int destPlane, int destSquareX, int destSquareY)
        {
            return (static_cast<uint32_t>(destPlane) << 16)
                 | (static_cast<uint32_t>(destSquareY) << 8)
                 | (static_cast<uint32_t>(destSquareX));
        }
    }

    ArtifactReader::ArtifactReader(const std::string &path)
        : bytes(readWholeFile(path))
    {
        parseDirectory();
        if (hasAltLandmarks() && hasAbstraction() && altAreas != areaNodeTable.size())
        {
            throw std::runtime_error("ArtifactReader: ALT areaCount disagrees with abstraction");
        }
        // After both abstraction + transitions are decoded, build the
        // near-goal edge bucket index. No-op when either section is absent.
        buildNearGoalEdgeBuckets();
    }

    void ArtifactReader::parseDirectory()
    {
        requireRange(0, sizeof(ArtifactHeader), bytes.size(), "header");
        const ArtifactHeader header = readPod<ArtifactHeader>(bytes, 0);
        if (header.magic != kArtifactMagic)
        {
            throw std::runtime_error("ArtifactReader: bad magic");
        }
        if (header.formatVersion != kArtifactFormatVersion)
        {
            throw std::runtime_error("ArtifactReader: format version "
                                     + std::to_string(header.formatVersion) + ", expected "
                                     + std::to_string(kArtifactFormatVersion));
        }
        metadata = {header.formatVersion, header.cacheRevision, header.datasetHash};

        const uint64_t dirBytes = static_cast<uint64_t>(header.sectionCount) * sizeof(SectionEntry);
        requireRange(sizeof(ArtifactHeader), dirBytes, bytes.size(), "section directory");

        uint64_t entryOffset = sizeof(ArtifactHeader);
        for (uint32_t i = 0; i < header.sectionCount; ++i)
        {
            decodeSection(readPod<SectionEntry>(bytes, entryOffset));
            entryOffset += sizeof(SectionEntry);
        }
    }

    void ArtifactReader::decodeSection(const SectionEntry &entry)
    {
        requireRange(entry.offset, entry.length, bytes.size(), "section payload");
        const std::size_t idx = entry.id;
        if (idx >= sectionPresent.size())
        {
            return;  // unknown section id — skip (forward compatible)
        }
        if (sectionPresent[idx])
        {
            throw std::runtime_error("ArtifactReader: duplicate section id " + std::to_string(entry.id));
        }
        sectionPresent[idx] = true;
        switch (static_cast<SectionId>(entry.id))
        {
        case SectionId::Collision:
            decodeCollision(entry);
            break;
        case SectionId::Transitions:
            decodeTransitions(entry);
            break;
        case SectionId::Abstraction:
            decodeAbstraction(entry);
            break;
        case SectionId::AltLandmarks:
            decodeAltLandmarks(entry);
            break;
        case SectionId::TeleportAllowed:
            decodeTeleportAllowed(entry);
            break;
        case SectionId::Provenance:
            decodeProvenance(entry);
            break;
        default:
            break;  // in-range but unmapped id — skip
        }
    }

    void ArtifactReader::decodeCollision(const SectionEntry &entry)
    {
        requireRange(entry.offset, sizeof(CollisionSectionHeader), bytes.size(), "collision header");
        const CollisionSectionHeader header = readPod<CollisionSectionHeader>(bytes, entry.offset);
        const uint64_t tableOffset = entry.offset + sizeof(CollisionSectionHeader);
        const uint64_t tableBytes = static_cast<uint64_t>(header.squareCount) * sizeof(CollisionSquareEntry);
        requireRange(tableOffset, tableBytes, bytes.size(), "collision table");
        collisionSquareTable = readPodArray<CollisionSquareEntry>(bytes, tableOffset, header.squareCount);
        collisionSectionOffset = entry.offset;

        collisionIndex.reserve(collisionSquareTable.size());
        for (std::size_t i = 0; i < collisionSquareTable.size(); ++i)
        {
            const CollisionSquareEntry &e = collisionSquareTable[i];
            collisionIndex.emplace(squareKey(e.squareX, e.squareY), i);
        }
    }

    bool ArtifactReader::decompressSquare(int squareX, int squareY, std::vector<uint32_t> &outWords) const
    {
        const auto it = collisionIndex.find(squareKey(static_cast<uint32_t>(squareX),
                                                      static_cast<uint32_t>(squareY)));
        if (it == collisionIndex.end())
        {
            return false;
        }
        const CollisionSquareEntry &e = collisionSquareTable[it->second];
        const uint32_t expectedRaw = kClipWordsPerSquare * static_cast<uint32_t>(sizeof(uint32_t));
        if (e.rawLength != expectedRaw)
        {
            throw std::runtime_error("ArtifactReader: collision blob raw length mismatch");
        }
        const uint64_t blobOffset = collisionSectionOffset + e.blobOffset;
        requireRange(blobOffset, e.blobLength, bytes.size(), "collision blob");
        outWords.resize(kClipWordsPerSquare);
        zlibDecompressInto(bytes.data() + static_cast<std::size_t>(blobOffset),
                            e.blobLength,
                            reinterpret_cast<uint8_t *>(outWords.data()),
                            e.rawLength);
        return true;
    }

    void ArtifactReader::decodeTransitions(const SectionEntry &entry)
    {
        requireRange(entry.offset, sizeof(TransitionSectionHeader), bytes.size(), "transition header");
        const TransitionSectionHeader header = readPod<TransitionSectionHeader>(bytes, entry.offset);
        uint64_t cursor = entry.offset + sizeof(TransitionSectionHeader);

        const uint64_t recBytes = static_cast<uint64_t>(header.transitionCount) * sizeof(TransitionRecord);
        requireRange(cursor, recBytes, bytes.size(), "transition records");
        transitionTable = readPodArray<TransitionRecord>(bytes, cursor, header.transitionCount);
        cursor += recBytes;

        const uint64_t reqBytes = static_cast<uint64_t>(header.requirementCount) * sizeof(RequirementRecord);
        requireRange(cursor, reqBytes, bytes.size(), "requirement pool");
        requirementPool = readPodArray<RequirementRecord>(bytes, cursor, header.requirementCount);
        cursor += reqBytes;

        const uint64_t chainBytes = static_cast<uint64_t>(header.chainStepCount) * sizeof(ChainStepRecord);
        requireRange(cursor, chainBytes, bytes.size(), "chain pool");
        chainStepPool = readPodArray<ChainStepRecord>(bytes, cursor, header.chainStepCount);

        // Validate every record's pool cross-references once at load, so a
        // corrupt artifact fails loud here instead of reading out of bounds
        // in a consumer that indexes the pools without a per-use range check
        // (the wwcli exec diagnostic does exactly that).
        for (const TransitionRecord &t : transitionTable)
        {
            const uint64_t reqEnd = static_cast<uint64_t>(t.requirementStart) + t.requirementCount;
            const uint64_t chainEnd = static_cast<uint64_t>(t.chainStart) + t.chainCount;
            if (reqEnd > requirementPool.size() || chainEnd > chainStepPool.size())
            {
                throw std::runtime_error(
                    "ArtifactReader: transition requirement/chain range out of bounds");
            }
        }

        // Remember the baked prefix lengths so runtime-appended teleports
        // (appendTransitions) can be dropped again on reload (truncateToBaked).
        bakedTransitionCount = transitionTable.size();
        bakedRequirementCount = requirementPool.size();
        bakedChainCount = chainStepPool.size();
        rebuildGlobalOriginIndex();
        rebuildRequirementIdLists();
    }

    void ArtifactReader::truncateToBaked()
    {
        transitionTable.resize(bakedTransitionCount);
        requirementPool.resize(bakedRequirementCount);
        chainStepPool.resize(bakedChainCount);
        rebuildGlobalOriginIndex();
        rebuildRequirementIdLists();
    }

    void ArtifactReader::appendTransitions(std::span<const TransitionRecord> transitions,
                                           std::span<const RequirementRecord> requirements,
                                           std::span<const ChainStepRecord> chainSteps)
    {
        // Callers (RuntimeTeleports) build the appended records with
        // requirementStart / chainStart already offset by the CURRENT pool
        // sizes, so a straight concatenation keeps every range valid.
        const std::size_t prefix = transitionTable.size();
        transitionTable.insert(transitionTable.end(), transitions.begin(), transitions.end());
        requirementPool.insert(requirementPool.end(), requirements.begin(), requirements.end());
        chainStepPool.insert(chainStepPool.end(), chainSteps.begin(), chainSteps.end());
        // Delta-append the global-origin index for the new tail instead of
        // a full rescan — the baked prefix's entries remain valid.
        for (std::size_t i = 0; i < transitions.size(); ++i)
        {
            if ((transitions[i].flags & kTransitionFlagGlobalOrigin) != 0u)
            {
                globalOriginIndices.push_back(static_cast<uint32_t>(prefix + i));
            }
        }
        // Requirement-id lists are deduped sets, so the cheap thing here is
        // to rescan the (now-extended) pool once instead of merging — the
        // pool is small (a few hundred records on a real artifact) and an
        // append happens at most a handful of times per process.
        rebuildRequirementIdLists();
    }

    void ArtifactReader::rebuildGlobalOriginIndex()
    {
        globalOriginIndices.clear();
        for (std::size_t i = 0; i < transitionTable.size(); ++i)
        {
            if ((transitionTable[i].flags & kTransitionFlagGlobalOrigin) != 0u)
            {
                globalOriginIndices.push_back(static_cast<uint32_t>(i));
            }
        }
    }

    void ArtifactReader::buildNearGoalEdgeBuckets()
    {
        nearGoalBucketFirst.assign(static_cast<std::size_t>(kNearGoalBucketCount) + 1u, 0u);
        nearGoalBucketEdges.clear();
        if (areaEdgeTable.empty() || transitionTable.empty())
        {
            return;  // either section absent — bucket stays empty, scan returns empty span
        }
        // Two-pass CSR build: count per bucket, prefix-sum, fill.
        const auto classify = [&](const AreaEdgeRecord &E,
                                  int &outBucket) -> bool
        {
            if (E.transitionIndex >= transitionTable.size())
            {
                return false;
            }
            const TransitionRecord &T = transitionTable[E.transitionIndex];
            if ((T.flags & kTransitionFlagGlobalOrigin) != 0u)
            {
                return false;  // globals are seeded separately
            }
            const int destPlane = static_cast<int>(T.destPlane);
            if (destPlane < 0 || destPlane >= kClipPlanes)
            {
                return false;
            }
            const int destSquareX = T.destX >> 6;
            const int destSquareY = T.destY >> 6;
            if (!inSquareRange(destSquareX) || !inSquareRange(destSquareY))
            {
                return false;
            }
            outBucket = static_cast<int>(
                nearGoalBucketKey(destPlane, destSquareX, destSquareY));
            return true;
        };

        for (const AreaEdgeRecord &E : areaEdgeTable)
        {
            int bucket = 0;
            if (classify(E, bucket))
            {
                ++nearGoalBucketFirst[static_cast<std::size_t>(bucket) + 1u];
            }
        }
        for (std::size_t i = 1; i < nearGoalBucketFirst.size(); ++i)
        {
            nearGoalBucketFirst[i] += nearGoalBucketFirst[i - 1];
        }
        nearGoalBucketEdges.assign(nearGoalBucketFirst.back(), 0u);
        // Cursor copy of the per-bucket head, advanced as we fill.
        std::vector<uint32_t> cursor = nearGoalBucketFirst;
        for (std::size_t i = 0; i < areaEdgeTable.size(); ++i)
        {
            int bucket = 0;
            if (classify(areaEdgeTable[i], bucket))
            {
                nearGoalBucketEdges[cursor[static_cast<std::size_t>(bucket)]++] =
                    static_cast<uint32_t>(i);
            }
        }
    }

    std::span<const uint32_t> ArtifactReader::nearGoalEdgeBucket(int destPlane,
                                                                  int destSquareX,
                                                                  int destSquareY) const
    {
        if (nearGoalBucketFirst.empty())
        {
            return {};
        }
        if (destPlane < 0 || destPlane >= kClipPlanes)
        {
            return {};
        }
        if (!inSquareRange(destSquareX) || !inSquareRange(destSquareY))
        {
            return {};
        }
        const std::size_t key = static_cast<std::size_t>(
            nearGoalBucketKey(destPlane, destSquareX, destSquareY));
        const std::size_t begin = nearGoalBucketFirst[key];
        const std::size_t end = nearGoalBucketFirst[key + 1u];
        return std::span<const uint32_t>(nearGoalBucketEdges.data() + begin, end - begin);
    }

    void ArtifactReader::rebuildRequirementIdLists()
    {
        // Two small dedup buffers — the requirement pool tops out at a few
        // hundred entries on a real artifact, so the unsorted "scan + linear
        // contains check" is cheaper than dragging in an std::unordered_set
        // (one-shot allocation, no hashing). Order matches first-seen scan
        // order — the Executor borrows the spans verbatim.
        requirementVarbitIdList.clear();
        requirementItemIdList.clear();
        const auto addUnique = [](std::vector<int32_t> &list, int32_t id)
        {
            for (const int32_t existing : list)
            {
                if (existing == id)
                {
                    return;
                }
            }
            list.push_back(id);
        };
        for (const RequirementRecord &r : requirementPool)
        {
            const auto kind = static_cast<ww::data::RequirementKind>(r.kind);
            // Both varbit forms feed the same id list: the executor reads a
            // value per id, and how that value is compared is the gate's
            // business, not the reader's. Omitting VarbitAtLeast here would
            // leave its ids unread, so every such gate would test against the
            // absent-id default of 0 and deny an account that in fact qualifies.
            if (kind == ww::data::RequirementKind::Varbit
                || kind == ww::data::RequirementKind::VarbitAtLeast)
            {
                addUnique(requirementVarbitIdList, r.id);
            }
            else if (kind == ww::data::RequirementKind::Item)
            {
                addUnique(requirementItemIdList, r.id);
            }
        }
    }

    void ArtifactReader::decodeAbstraction(const SectionEntry &entry)
    {
        requireRange(entry.offset, sizeof(AbstractionSectionHeader), bytes.size(), "abstraction header");
        const AbstractionSectionHeader header = readPod<AbstractionSectionHeader>(bytes, entry.offset);
        uint64_t cursor = entry.offset + sizeof(AbstractionSectionHeader);

        const uint64_t nodeBytes = static_cast<uint64_t>(header.areaCount) * sizeof(AreaNodeRecord);
        requireRange(cursor, nodeBytes, bytes.size(), "area nodes");
        areaNodeTable = readPodArray<AreaNodeRecord>(bytes, cursor, header.areaCount);
        cursor += nodeBytes;

        const uint64_t edgeBytes = static_cast<uint64_t>(header.edgeCount) * sizeof(AreaEdgeRecord);
        requireRange(cursor, edgeBytes, bytes.size(), "area edges");
        areaEdgeTable = readPodArray<AreaEdgeRecord>(bytes, cursor, header.edgeCount);
        cursor += edgeBytes;

        const uint64_t gridBytes = static_cast<uint64_t>(header.gridCount) * sizeof(AreaGridEntry);
        requireRange(cursor, gridBytes, bytes.size(), "area grid table");
        areaGridTable = readPodArray<AreaGridEntry>(bytes, cursor, header.gridCount);
        abstractionSectionOffset = entry.offset;

        areaGridIndex.reserve(areaGridTable.size());
        for (std::size_t i = 0; i < areaGridTable.size(); ++i)
        {
            const AreaGridEntry &g = areaGridTable[i];
            areaGridIndex.emplace(gridKey(g.squareX, g.squareY, g.plane), i);
        }
    }

    bool ArtifactReader::decompressGrid(int squareX, int squareY, int plane,
                                        std::vector<int32_t> &outIds) const
    {
        const auto it = areaGridIndex.find(gridKey(static_cast<uint32_t>(squareX),
                                                   static_cast<uint32_t>(squareY),
                                                   static_cast<uint32_t>(plane)));
        if (it == areaGridIndex.end())
        {
            return false;
        }
        const AreaGridEntry &g = areaGridTable[it->second];
        const uint32_t expectedRaw =
            static_cast<uint32_t>(static_cast<std::size_t>(kClipSize) * kClipSize * sizeof(int32_t));
        if (g.rawLength != expectedRaw)
        {
            throw std::runtime_error("ArtifactReader: area grid raw length mismatch");
        }
        const uint64_t blobOffset = abstractionSectionOffset + g.blobOffset;
        requireRange(blobOffset, g.blobLength, bytes.size(), "area grid blob");
        outIds.resize(static_cast<std::size_t>(kClipSize) * kClipSize);
        zlibDecompressInto(bytes.data() + static_cast<std::size_t>(blobOffset),
                            g.blobLength,
                            reinterpret_cast<uint8_t *>(outIds.data()),
                            g.rawLength);
        return true;
    }

    std::vector<float> ArtifactReader::decompressFloatTable(const AltTableDescriptor &desc,
                                                            uint64_t sectionOffset) const
    {
        const uint64_t blobOffset = sectionOffset + desc.blobOffset;
        requireRange(blobOffset, desc.blobLength, bytes.size(), "alt table blob");
        std::vector<float> table(desc.rawLength / sizeof(float));
        zlibDecompressInto(bytes.data() + static_cast<std::size_t>(blobOffset),
                            desc.blobLength,
                            reinterpret_cast<uint8_t *>(table.data()),
                            desc.rawLength);
        return table;
    }

    // Transpose a landmark-major ALT distance table (on-disk layout,
    // [landmark * areaCount + area]) into area-major in-memory storage
    // ([area * landmarkCount + landmark]). The AltHeuristic::estimate hot
    // loop reads every landmark column for one area per call; area-major
    // makes that a contiguous landmark-wide window, which on a small
    // landmark count (e.g. 16) fits in one cache line.
    static std::vector<float> transposeAltTable(const std::vector<float> &src,
                                                uint32_t landmarks, uint32_t areas)
    {
        std::vector<float> dst(static_cast<std::size_t>(landmarks) * areas);
        for (uint32_t l = 0; l < landmarks; ++l)
        {
            const std::size_t rowBase = static_cast<std::size_t>(l) * areas;
            for (uint32_t a = 0; a < areas; ++a)
            {
                dst[static_cast<std::size_t>(a) * landmarks + l] = src[rowBase + a];
            }
        }
        return dst;
    }

    void ArtifactReader::decodeAltLandmarks(const SectionEntry &entry)
    {
        requireRange(entry.offset, sizeof(AltLandmarksSectionHeader), bytes.size(), "alt header");
        const AltLandmarksSectionHeader header = readPod<AltLandmarksSectionHeader>(bytes, entry.offset);
        altLandmarkCount = header.landmarkCount;
        altAreas = header.areaCount;

        uint64_t cursor = entry.offset + sizeof(AltLandmarksSectionHeader);
        const uint64_t idBytes = static_cast<uint64_t>(header.landmarkCount) * sizeof(int32_t);
        requireRange(cursor, idBytes, bytes.size(), "alt landmark ids");
        landmarkAreaList = readPodArray<int32_t>(bytes, cursor, header.landmarkCount);
        cursor += idBytes;

        requireRange(cursor, 2u * sizeof(AltTableDescriptor), bytes.size(), "alt descriptors");
        const AltTableDescriptor fromDesc = readPod<AltTableDescriptor>(bytes, cursor);
        const AltTableDescriptor toDesc =
            readPod<AltTableDescriptor>(bytes, cursor + sizeof(AltTableDescriptor));

        // Validate the descriptors against the expected table shape BEFORE
        // handing them to the zlib decompressor. zlibDecompress allocates a
        // `rawLength`-sized buffer up front; a corrupt artifact with a
        // 0xFFFFFFFF rawLength would otherwise trigger a ~4 GB allocation
        // before any sanity check ran.
        const uint64_t expectedRaw = static_cast<uint64_t>(header.landmarkCount)
                                   * static_cast<uint64_t>(header.areaCount)
                                   * static_cast<uint64_t>(sizeof(float));
        if (fromDesc.rawLength != expectedRaw)
        {
            throw std::runtime_error("ArtifactReader: alt fromLandmark raw length mismatch");
        }
        if (toDesc.rawLength != expectedRaw)
        {
            throw std::runtime_error("ArtifactReader: alt toLandmark raw length mismatch");
        }

        const std::vector<float> fromRaw = decompressFloatTable(fromDesc, entry.offset);
        const std::vector<float> toRaw = decompressFloatTable(toDesc, entry.offset);
        // Transpose to area-major (see distFromLandmark / distToLandmark
        // accessors). Done once at load; estimate() then pays one contiguous
        // landmarkCount-wide read per area instead of landmarkCount strided
        // reads across the whole table.
        fromLandmarkData = transposeAltTable(fromRaw, header.landmarkCount, header.areaCount);
        toLandmarkData = transposeAltTable(toRaw, header.landmarkCount, header.areaCount);

        // Phase 5: substitute +inf sentinels so AltHeuristic::estimate can be
        // branchless. The two bounds are:
        //   bound1 = toL - goalToL    (estimate >= bound1 when both finite)
        //   bound2 = goalFromL - fromL (estimate >= bound2 when both finite)
        // The substitution must produce a value <= 0 whenever EITHER side was
        // originally +inf, so max(0, bound) clamps the unreachable cases to 0
        // (identical contribution to today's branched estimate). For the
        // area-side tables, the substitutions are:
        //   toL    (minuend in bound1)    : +inf -> -1e30f
        //   fromL  (subtrahend in bound2) : +inf -> +1e30f
        // The matching goal-side substitution happens in AltHeuristic::prepare.
        constexpr float kInf = std::numeric_limits<float>::infinity();
        constexpr float kBigPos = 1e30f;
        constexpr float kBigNeg = -1e30f;
        for (float &v : toLandmarkData)
        {
            if (!(v < kInf))
            {
                v = kBigNeg;
            }
        }
        for (float &v : fromLandmarkData)
        {
            if (!(v < kInf))
            {
                v = kBigPos;
            }
        }
    }

    void ArtifactReader::decodeTeleportAllowed(const SectionEntry &entry)
    {
        requireRange(entry.offset, sizeof(TeleportAllowedSectionHeader), bytes.size(), "teleport header");
        const TeleportAllowedSectionHeader header = readPod<TeleportAllowedSectionHeader>(bytes, entry.offset);
        teleportCutoff = header.defaultWildernessCutoff;

        uint64_t cursor = entry.offset + sizeof(TeleportAllowedSectionHeader);
        const uint64_t wildBytes = static_cast<uint64_t>(header.wildernessCount) * sizeof(WildernessRegion);
        requireRange(cursor, wildBytes, bytes.size(), "wilderness regions");
        wildernessList = readPodArray<WildernessRegion>(bytes, cursor, header.wildernessCount);
        cursor += wildBytes;

        const uint64_t zoneBytes = static_cast<uint64_t>(header.noTeleCount) * sizeof(NoTeleZone);
        requireRange(cursor, zoneBytes, bytes.size(), "no-tele zones");
        noTeleList = readPodArray<NoTeleZone>(bytes, cursor, header.noTeleCount);
    }

    void ArtifactReader::decodeProvenance(const SectionEntry &entry)
    {
        requireRange(entry.offset, sizeof(ProvenanceSectionHeader), bytes.size(), "provenance header");
        const ProvenanceSectionHeader header = readPod<ProvenanceSectionHeader>(bytes, entry.offset);
        const uint64_t bodyOffset = entry.offset + sizeof(ProvenanceSectionHeader);
        requireRange(bodyOffset, header.jsonLength, bytes.size(), "provenance body");

        // An unrecognised schema is recorded and the body kept verbatim rather
        // than refused: provenance is descriptive, so a document this build does
        // not understand is still worth handing to whoever asked for it. Nothing
        // in the planner reads either field.
        provenanceDocSchema = header.schema;
        const auto *first = reinterpret_cast<const char *>(bytes.data() + bodyOffset);
        provenanceDoc.assign(first, static_cast<std::size_t>(header.jsonLength));
    }
}
