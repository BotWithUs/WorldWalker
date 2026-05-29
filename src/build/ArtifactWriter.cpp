#include "build/ArtifactWriter.h"

#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/Zlib.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <stdexcept>
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

        void writeFile(const std::string &path, const std::vector<uint8_t> &bytes)
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                throw std::runtime_error("failed to open artifact for writing: " + path);
            }
            stream.write(reinterpret_cast<const char *>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
            if (!stream)
            {
                throw std::runtime_error("failed to write artifact: " + path);
            }
        }
    }

    void writeArtifact(const std::string &path, const CollisionModel &collision,
                       const ww::data::TransitionModel &transitions,
                       const AreaGraphModel &abstraction,
                       uint32_t cacheRevision, uint32_t datasetHash)
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

        const uint32_t sectionCount = static_cast<uint32_t>(sections.size());
        const uint64_t directoryBytes = static_cast<uint64_t>(sectionCount) * sizeof(SectionEntry);

        ArtifactHeader header{};
        header.magic = kArtifactMagic;
        header.formatVersion = kArtifactFormatVersion;
        header.cacheRevision = cacheRevision;
        header.datasetHash = datasetHash;
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
