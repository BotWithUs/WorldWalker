#include "build/ArtifactWriter.h"

#include "format/Artifact.h"
#include "format/Zlib.h"

#include <cstdint>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <vector>

namespace ww::build
{
    namespace
    {
        template <typename T>
        void appendPod(std::vector<uint8_t> &out, const T &value)
        {
            const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
            out.insert(out.end(), bytes, bytes + sizeof(T));
        }

        // Collision section payload: header + square table + per-square zlib blobs.
        std::vector<uint8_t> buildCollisionSection(const CollisionModel &model)
        {
            using namespace ww::format;
            const uint32_t squareCount = static_cast<uint32_t>(model.squares.size());

            std::vector<std::vector<uint8_t>> blobs;
            blobs.reserve(model.squares.size());
            for (const SquareClip &sq : model.squares)
            {
                const auto *raw = reinterpret_cast<const uint8_t *>(sq.words.data());
                blobs.push_back(zlibCompress(raw, sq.words.size() * sizeof(uint32_t)));
            }

            const uint32_t entriesBytes = squareCount * static_cast<uint32_t>(sizeof(CollisionSquareEntry));
            const uint32_t blobBase = static_cast<uint32_t>(sizeof(CollisionSectionHeader)) + entriesBytes;

            std::vector<uint8_t> section;
            CollisionSectionHeader sectionHeader{};
            sectionHeader.squareCount = squareCount;
            appendPod(section, sectionHeader);

            uint32_t blobCursor = blobBase;
            for (uint32_t i = 0; i < squareCount; ++i)
            {
                const SquareClip &sq = model.squares[i];
                CollisionSquareEntry entry{};
                entry.squareX = static_cast<uint16_t>(sq.squareX);
                entry.squareY = static_cast<uint16_t>(sq.squareY);
                entry.planeMask = sq.planeMask;
                entry.blobOffset = blobCursor;
                entry.blobLength = static_cast<uint32_t>(blobs[i].size());
                entry.rawLength = static_cast<uint32_t>(sq.words.size() * sizeof(uint32_t));
                appendPod(section, entry);
                blobCursor += static_cast<uint32_t>(blobs[i].size());
            }

            for (const std::vector<uint8_t> &blob : blobs)
            {
                section.insert(section.end(), blob.begin(), blob.end());
            }
            return section;
        }
    }

    void writeArtifact(const std::string &path, const CollisionModel &collision,
                       uint32_t cacheRevision)
    {
        using namespace ww::format;

        const std::vector<uint8_t> collisionSection = buildCollisionSection(collision);

        const uint32_t sectionCount = 1;
        const uint64_t directoryBytes = static_cast<uint64_t>(sectionCount) * sizeof(SectionEntry);

        ArtifactHeader header{};
        header.magic = kArtifactMagic;
        header.formatVersion = kArtifactFormatVersion;
        header.cacheRevision = cacheRevision;
        header.datasetHash = 0;
        header.sectionCount = sectionCount;

        SectionEntry collisionEntry{};
        collisionEntry.id = static_cast<uint32_t>(SectionId::Collision);
        collisionEntry.offset = sizeof(ArtifactHeader) + directoryBytes;
        collisionEntry.length = collisionSection.size();

        std::vector<uint8_t> file;
        appendPod(file, header);
        appendPod(file, collisionEntry);
        file.insert(file.end(), collisionSection.begin(), collisionSection.end());

        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            throw std::runtime_error("failed to open artifact for writing: " + path);
        }
        stream.write(reinterpret_cast<const char *>(file.data()),
                     static_cast<std::streamsize>(file.size()));
        if (!stream)
        {
            throw std::runtime_error("failed to write artifact: " + path);
        }
    }
}
