#include "format/ArtifactReader.h"

#include "format/Zlib.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
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
    }

    ArtifactReader::ArtifactReader(const std::string &path)
        : bytes(readWholeFile(path))
    {
        parseDirectory();
        if (hasAltLandmarks() && hasAbstraction() && altAreas != areaNodeTable.size())
        {
            throw std::runtime_error("ArtifactReader: ALT areaCount disagrees with abstraction");
        }
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
        const std::vector<uint8_t> raw =
            zlibDecompress(bytes.data() + static_cast<std::size_t>(blobOffset), e.blobLength, e.rawLength);
        outWords.resize(kClipWordsPerSquare);
        std::memcpy(outWords.data(), raw.data(), e.rawLength);
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

        // Remember the baked prefix lengths so runtime-appended teleports
        // (appendTransitions) can be dropped again on reload (truncateToBaked).
        bakedTransitionCount = transitionTable.size();
        bakedRequirementCount = requirementPool.size();
        bakedChainCount = chainStepPool.size();
    }

    void ArtifactReader::truncateToBaked()
    {
        transitionTable.resize(bakedTransitionCount);
        requirementPool.resize(bakedRequirementCount);
        chainStepPool.resize(bakedChainCount);
    }

    void ArtifactReader::appendTransitions(std::span<const TransitionRecord> transitions,
                                           std::span<const RequirementRecord> requirements,
                                           std::span<const ChainStepRecord> chainSteps)
    {
        // Callers (RuntimeTeleports) build the appended records with
        // requirementStart / chainStart already offset by the CURRENT pool
        // sizes, so a straight concatenation keeps every range valid.
        transitionTable.insert(transitionTable.end(), transitions.begin(), transitions.end());
        requirementPool.insert(requirementPool.end(), requirements.begin(), requirements.end());
        chainStepPool.insert(chainStepPool.end(), chainSteps.begin(), chainSteps.end());
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
        const std::vector<uint8_t> raw =
            zlibDecompress(bytes.data() + static_cast<std::size_t>(blobOffset), g.blobLength, g.rawLength);
        outIds.resize(static_cast<std::size_t>(kClipSize) * kClipSize);
        std::memcpy(outIds.data(), raw.data(), g.rawLength);
        return true;
    }

    std::vector<float> ArtifactReader::decompressFloatTable(const AltTableDescriptor &desc,
                                                            uint64_t sectionOffset) const
    {
        const uint64_t blobOffset = sectionOffset + desc.blobOffset;
        requireRange(blobOffset, desc.blobLength, bytes.size(), "alt table blob");
        const std::vector<uint8_t> raw =
            zlibDecompress(bytes.data() + static_cast<std::size_t>(blobOffset), desc.blobLength, desc.rawLength);
        std::vector<float> table(desc.rawLength / sizeof(float));
        std::memcpy(table.data(), raw.data(), desc.rawLength);
        return table;
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

        fromLandmarkData = decompressFloatTable(fromDesc, entry.offset);
        toLandmarkData = decompressFloatTable(toDesc, entry.offset);
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
}
