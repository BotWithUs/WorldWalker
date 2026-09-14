#include "build/ArtifactWriter.h"

#include "data/TeleportZones.h"
#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/Zlib.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace ww::build
{
    namespace
    {
        struct Section
        {
            ww::format::SectionId id;
            std::vector<uint8_t> payload;
        };

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

        // Flatten one transition into a fixed-size record, appending its
        // requirements and chain steps to the shared pools and recording the spans.
        ww::format::TransitionRecord encodeTransition(const ww::data::Transition &t,
                                                      std::vector<ww::format::RequirementRecord> &reqPool,
                                                      std::vector<ww::format::ChainStepRecord> &chainPool)
        {
            using namespace ww::format;
            TransitionRecord r{};
            r.kind = static_cast<uint8_t>(t.kind);
            r.flags = t.isGlobalOrigin ? kTransitionFlagGlobalOrigin : static_cast<uint8_t>(0);
            r.originPlane = t.originPlane;
            r.destPlane = t.destPlane;
            r.originX = t.originX;
            r.originY = t.originY;
            r.destX = t.destX;
            r.destY = t.destY;
            r.objectId = t.objectId;
            r.shape = t.shape;
            r.rotation = t.rotation;
            r.optionIndex = t.optionIndex;
            std::memcpy(r.code, t.code, sizeof(r.code));
            r.cost = t.cost;
            r.costQuick = t.costQuick;
            r.requirementStart = static_cast<uint32_t>(reqPool.size());
            r.requirementCount = static_cast<uint32_t>(t.requirements.size());
            for (const ww::data::Requirement &req : t.requirements)
            {
                RequirementRecord rr{};
                rr.kind = static_cast<uint8_t>(req.kind);
                rr.id = req.id;
                rr.amount = req.amount;
                reqPool.push_back(rr);
            }
            r.chainStart = static_cast<uint32_t>(chainPool.size());
            r.chainCount = static_cast<uint32_t>(t.chain.size());
            for (const ww::data::ChainStep &cs : t.chain)
            {
                ChainStepRecord cr{};
                cr.kind = static_cast<uint8_t>(cs.kind);
                cr.a = cs.a;
                cr.b = cs.b;
                cr.c = cs.c;
                cr.d = cs.d;
                cr.e = cs.e;
                cr.f = cs.f;
                cr.g = cs.g;
                cr.h = cs.h;
                cr.i = cs.i;
                chainPool.push_back(cr);
            }
            return r;
        }

        std::vector<uint8_t> buildTransitionSection(const ww::data::TransitionModel &model)
        {
            using namespace ww::format;
            std::vector<RequirementRecord> reqPool;
            std::vector<ChainStepRecord> chainPool;
            std::vector<TransitionRecord> records;
            records.reserve(model.transitions.size());
            for (const ww::data::Transition &t : model.transitions)
            {
                records.push_back(encodeTransition(t, reqPool, chainPool));
            }

            TransitionSectionHeader header{};
            header.transitionCount = static_cast<uint32_t>(records.size());
            header.requirementCount = static_cast<uint32_t>(reqPool.size());
            header.chainStepCount = static_cast<uint32_t>(chainPool.size());

            std::vector<uint8_t> section;
            appendPod(section, header);
            for (const TransitionRecord &r : records)
            {
                appendPod(section, r);
            }
            for (const RequirementRecord &r : reqPool)
            {
                appendPod(section, r);
            }
            for (const ChainStepRecord &c : chainPool)
            {
                appendPod(section, c);
            }
            return section;
        }

        void appendAreaNodes(std::vector<uint8_t> &section, const AreaGraphModel &model)
        {
            for (const AreaNode &node : model.nodes)
            {
                ww::format::AreaNodeRecord rec{};
                rec.plane = node.plane;
                rec.tileCount = node.tileCount;
                rec.centroidX = node.centroidX;
                rec.centroidY = node.centroidY;
                rec.minX = node.minX;
                rec.minY = node.minY;
                rec.maxX = node.maxX;
                rec.maxY = node.maxY;
                appendPod(section, rec);
            }
        }

        void appendAreaEdges(std::vector<uint8_t> &section, const AreaGraphModel &model)
        {
            for (const AreaEdge &edge : model.edges)
            {
                ww::format::AreaEdgeRecord rec{};
                rec.fromArea = edge.fromArea;
                rec.toArea = edge.toArea;
                rec.transitionIndex = edge.transitionIndex;
                rec.cost = edge.cost;
                appendPod(section, rec);
            }
        }

        // Abstraction section payload: header + node table + edge table + grid
        // table + per-grid zlib blobs.
        std::vector<uint8_t> buildAbstractionSection(const AreaGraphModel &model)
        {
            using namespace ww::format;

            std::vector<std::vector<uint8_t>> blobs;
            blobs.reserve(model.grids.size());
            for (const AreaGrid &grid : model.grids)
            {
                const auto *raw = reinterpret_cast<const uint8_t *>(grid.ids.data());
                blobs.push_back(zlibCompress(raw, grid.ids.size() * sizeof(int32_t)));
            }

            AbstractionSectionHeader header{};
            header.areaCount = static_cast<uint32_t>(model.nodes.size());
            header.edgeCount = static_cast<uint32_t>(model.edges.size());
            header.gridCount = static_cast<uint32_t>(model.grids.size());

            const uint32_t nodesBytes = header.areaCount * static_cast<uint32_t>(sizeof(AreaNodeRecord));
            const uint32_t edgesBytes = header.edgeCount * static_cast<uint32_t>(sizeof(AreaEdgeRecord));
            const uint32_t gridsBytes = header.gridCount * static_cast<uint32_t>(sizeof(AreaGridEntry));
            const uint32_t blobBase =
                static_cast<uint32_t>(sizeof(AbstractionSectionHeader)) + nodesBytes + edgesBytes + gridsBytes;

            std::vector<uint8_t> section;
            appendPod(section, header);
            appendAreaNodes(section, model);
            appendAreaEdges(section, model);

            uint32_t blobCursor = blobBase;
            for (std::size_t i = 0; i < model.grids.size(); ++i)
            {
                const AreaGrid &grid = model.grids[i];
                AreaGridEntry entry{};
                entry.squareX = static_cast<uint16_t>(grid.squareX);
                entry.squareY = static_cast<uint16_t>(grid.squareY);
                entry.plane = static_cast<uint8_t>(grid.plane);
                entry.blobOffset = blobCursor;
                entry.blobLength = static_cast<uint32_t>(blobs[i].size());
                entry.rawLength = static_cast<uint32_t>(grid.ids.size() * sizeof(int32_t));
                appendPod(section, entry);
                blobCursor += static_cast<uint32_t>(blobs[i].size());
            }

            for (const std::vector<uint8_t> &blob : blobs)
            {
                section.insert(section.end(), blob.begin(), blob.end());
            }
            return section;
        }

        // ALT landmarks section payload: header + landmark area-id list + two
        // table descriptors + the two zlib-compressed distance tables.
        std::vector<uint8_t> buildAltSection(const AltLandmarksModel &model)
        {
            using namespace ww::format;
            const auto *fromRaw = reinterpret_cast<const uint8_t *>(model.fromLandmark.data());
            const auto *toRaw = reinterpret_cast<const uint8_t *>(model.toLandmark.data());
            const std::vector<uint8_t> fromBlob =
                zlibCompress(fromRaw, model.fromLandmark.size() * sizeof(float));
            const std::vector<uint8_t> toBlob =
                zlibCompress(toRaw, model.toLandmark.size() * sizeof(float));

            AltLandmarksSectionHeader header{};
            header.landmarkCount = static_cast<uint32_t>(model.landmarks.size());
            header.areaCount = model.areaCount;

            const uint32_t idsBytes = static_cast<uint32_t>(model.landmarks.size() * sizeof(int32_t));
            const uint32_t descBytes = 2u * static_cast<uint32_t>(sizeof(AltTableDescriptor));
            const uint32_t blobBase =
                static_cast<uint32_t>(sizeof(AltLandmarksSectionHeader)) + idsBytes + descBytes;

            AltTableDescriptor fromDesc{};
            fromDesc.blobOffset = blobBase;
            fromDesc.blobLength = static_cast<uint32_t>(fromBlob.size());
            fromDesc.rawLength = static_cast<uint32_t>(model.fromLandmark.size() * sizeof(float));
            AltTableDescriptor toDesc{};
            toDesc.blobOffset = blobBase + static_cast<uint32_t>(fromBlob.size());
            toDesc.blobLength = static_cast<uint32_t>(toBlob.size());
            toDesc.rawLength = static_cast<uint32_t>(model.toLandmark.size() * sizeof(float));

            std::vector<uint8_t> section;
            appendPod(section, header);
            for (int32_t id : model.landmarks)
            {
                appendPod(section, id);
            }
            appendPod(section, fromDesc);
            appendPod(section, toDesc);
            section.insert(section.end(), fromBlob.begin(), fromBlob.end());
            section.insert(section.end(), toBlob.begin(), toBlob.end());
            return section;
        }

        // Teleport-allowed section payload: header + wilderness region table +
        // no-teleport zone table. Stored uncompressed (a handful of boxes).
        std::vector<uint8_t> buildTeleportSection(const ww::data::TeleportZonesModel &model)
        {
            using namespace ww::format;
            TeleportAllowedSectionHeader header{};
            header.wildernessCount = static_cast<uint32_t>(model.wilderness.size());
            header.noTeleCount = static_cast<uint32_t>(model.noTele.size());
            header.defaultWildernessCutoff = model.defaultWildernessCutoff;

            std::vector<uint8_t> section;
            appendPod(section, header);
            for (const ww::data::WildernessRegion &w : model.wilderness)
            {
                WildernessRegion rec{};
                rec.minX = w.minX;
                rec.minY = w.minY;
                rec.maxX = w.maxX;
                rec.maxY = w.maxY;
                rec.baseY = w.baseY;
                rec.baseLevel = w.baseLevel;
                rec.stepY = w.stepY;
                rec.planeMin = w.planeMin;
                rec.planeMax = w.planeMax;
                appendPod(section, rec);
            }
            for (const ww::data::NoTeleZone &z : model.noTele)
            {
                NoTeleZone rec{};
                rec.minX = z.minX;
                rec.minY = z.minY;
                rec.maxX = z.maxX;
                rec.maxY = z.maxY;
                rec.planeMin = z.planeMin;
                rec.planeMax = z.planeMax;
                appendPod(section, rec);
            }
            return section;
        }

        // Provenance section payload: header + the UTF-8 JSON body, stored
        // uncompressed (under a kilobyte, and a record nobody can read without
        // running code is a worse record).
        std::vector<uint8_t> buildProvenanceSection(const std::string &document)
        {
            using namespace ww::format;
            ProvenanceSectionHeader header{};
            header.schema = kProvenanceSchema;
            header.jsonLength = static_cast<uint32_t>(document.size());

            std::vector<uint8_t> section;
            appendPod(section, header);
            section.insert(section.end(), document.begin(), document.end());
            return section;
        }

        // Atomic write: serialize to `path.tmp`, flush + close, then rename
        // over `path`. A mid-write failure (disk full, signal, antivirus
        // delete) leaves the *temporary* corrupted file behind — not the
        // user's existing artifact — so the next wwbuild invocation either
        // sees the rename complete (good artifact) or the rename never
        // happened (old artifact still in place). On any failure path the
        // temp file is best-effort removed so /tmp doesn't accumulate junk.
        void writeFile(const std::string &path, const std::vector<uint8_t> &bytes)
        {
            namespace fs = std::filesystem;
            const fs::path target(path);
            fs::path tmp = target;
            tmp += ".tmp";

            // Best-effort: drop any stale tmp from a previous crashed run so
            // the truncating ofstream below starts from a clean slot.
            std::error_code ignore;
            fs::remove(tmp, ignore);

            try
            {
                std::ofstream stream(tmp, std::ios::binary | std::ios::trunc);
                if (!stream)
                {
                    throw std::runtime_error("failed to open artifact tmp for writing: "
                                             + tmp.string());
                }
                stream.write(reinterpret_cast<const char *>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size()));
                stream.flush();
                if (!stream)
                {
                    throw std::runtime_error("failed to write artifact tmp: " + tmp.string());
                }
                stream.close();
                if (!stream)
                {
                    throw std::runtime_error("failed to close artifact tmp: " + tmp.string());
                }
            }
            catch (...)
            {
                fs::remove(tmp, ignore);
                throw;
            }

            // std::filesystem::rename on MSVC's stdlib uses MoveFileExW with
            // MOVEFILE_REPLACE_EXISTING, atomically replacing the destination
            // on the same volume. If the platform implementation declines to
            // overwrite, fall back to remove + rename — not strictly atomic,
            // but no worse than the prior trunc-and-write behavior.
            std::error_code ec;
            fs::rename(tmp, target, ec);
            if (ec)
            {
                fs::remove(target, ignore);
                fs::rename(tmp, target, ec);
                if (ec)
                {
                    fs::remove(tmp, ignore);
                    throw std::runtime_error("failed to publish artifact " + target.string()
                                             + ": " + ec.message());
                }
            }
        }
    }

    void writeArtifact(const std::string &path, const CollisionModel &collision,
                       const ww::data::TransitionModel &transitions,
                       const AreaGraphModel &abstraction,
                       const AltLandmarksModel &altLandmarks,
                       const ww::data::TeleportZonesModel &teleportZones,
                       const ArtifactMeta &meta)
    {
        using namespace ww::format;

        std::vector<Section> sections;
        sections.push_back({SectionId::Collision, buildCollisionSection(collision)});
        if (!transitions.transitions.empty())
        {
            sections.push_back({SectionId::Transitions, buildTransitionSection(transitions)});
        }
        if (!abstraction.nodes.empty())
        {
            sections.push_back({SectionId::Abstraction, buildAbstractionSection(abstraction)});
        }
        if (!altLandmarks.landmarks.empty())
        {
            sections.push_back({SectionId::AltLandmarks, buildAltSection(altLandmarks)});
        }
        if (!teleportZones.wilderness.empty() || !teleportZones.noTele.empty())
        {
            sections.push_back({SectionId::TeleportAllowed, buildTeleportSection(teleportZones)});
        }
        if (!meta.provenanceJson.empty())
        {
            sections.push_back({SectionId::Provenance, buildProvenanceSection(meta.provenanceJson)});
        }

        const uint32_t sectionCount = static_cast<uint32_t>(sections.size());
        const uint64_t directoryBytes = static_cast<uint64_t>(sectionCount) * sizeof(SectionEntry);

        ArtifactHeader header{};
        header.magic = kArtifactMagic;
        header.formatVersion = kArtifactFormatVersion;
        header.cacheRevision = meta.cacheRevision;
        header.datasetHash = meta.datasetHash;
        header.sectionCount = sectionCount;

        std::vector<uint8_t> file;
        appendPod(file, header);

        uint64_t cursor = sizeof(ArtifactHeader) + directoryBytes;
        for (const Section &s : sections)
        {
            SectionEntry entry{};
            entry.id = static_cast<uint32_t>(s.id);
            entry.offset = cursor;
            entry.length = s.payload.size();
            appendPod(file, entry);
            cursor += s.payload.size();
        }
        for (const Section &s : sections)
        {
            file.insert(file.end(), s.payload.begin(), s.payload.end());
        }

        writeFile(path, file);
    }
}
