#include "c_api/worldwalker_c.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "runtime/WorldView.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

// wwcli — WorldWalker dev harness (queries + benchmarks).
// `wwcli <artifact.wwa>` loads the artifact through the runtime reader and prints
// a per-section summary, decompressing the first collision square and area grid
// to prove the on-demand path works end-to-end. Query/benchmark commands land in
// later Phase 3/6 steps.
namespace
{
    void dumpCollision(const ww::format::ArtifactReader &reader)
    {
        const auto squares = reader.collisionSquares();
        std::printf("  collision: %zu squares", squares.size());
        if (!squares.empty())
        {
            std::vector<uint32_t> words;
            reader.decompressSquare(squares[0].squareX, squares[0].squareY, words);
            std::size_t nonZero = 0;
            for (uint32_t w : words)
            {
                nonZero += (w != 0) ? 1 : 0;
            }
            std::printf(" | square[0] (%u,%u) -> %zu words, %zu non-open",
                        squares[0].squareX, squares[0].squareY, words.size(), nonZero);
        }
        std::printf("\n");
    }

    void dumpAbstraction(const ww::format::ArtifactReader &reader)
    {
        const auto grids = reader.areaGrids();
        std::printf("  abstraction: %zu areas, %zu edges, %zu grids", reader.areaNodes().size(),
                    reader.areaEdges().size(), grids.size());
        if (!grids.empty())
        {
            std::vector<int32_t> ids;
            reader.decompressGrid(grids[0].squareX, grids[0].squareY, grids[0].plane, ids);
            std::size_t walkable = 0;
            for (int32_t id : ids)
            {
                walkable += (id >= 0) ? 1 : 0;
            }
            std::printf(" | grid[0] (%u,%u,p%u) -> %zu tiles, %zu in an area",
                        grids[0].squareX, grids[0].squareY, grids[0].plane, ids.size(), walkable);
        }
        std::printf("\n");
    }

    // Re-read every clip word of one square through the by-coordinate runtime
    // lookup and confirm it matches the directly-decompressed ground truth, so a
    // coordinate-math or cache bug surfaces here rather than in the planner.
    std::size_t crossCheckSquare(ww::runtime::WorldView &view,
                                 const ww::format::CollisionSquareEntry &sq,
                                 const std::vector<uint32_t> &words)
    {
        const int baseX = static_cast<int>(sq.squareX) * ww::format::kClipSize;
        const int baseY = static_cast<int>(sq.squareY) * ww::format::kClipSize;
        std::size_t mismatches = 0;
        for (std::size_t idx = 0; idx < words.size(); ++idx)
        {
            const int plane = static_cast<int>(idx / (ww::format::kClipSize * ww::format::kClipSize));
            const int lx = static_cast<int>((idx / ww::format::kClipSize) % ww::format::kClipSize);
            const int ly = static_cast<int>(idx % ww::format::kClipSize);
            if (view.clipAt(baseX + lx, baseY + ly, plane) != words[idx])
            {
                ++mismatches;
            }
        }
        return mismatches;
    }

    void dumpRuntimeLookup(const ww::format::ArtifactReader &reader)
    {
        ww::runtime::WorldView view(reader);
        const auto squares = reader.collisionSquares();
        if (squares.empty())
        {
            std::printf("  runtime: no collision squares to cross-check\n");
            return;
        }
        const ww::format::CollisionSquareEntry &sq = squares[0];
        std::vector<uint32_t> words;
        reader.decompressSquare(sq.squareX, sq.squareY, words);
        const std::size_t mismatches = crossCheckSquare(view, sq, words);

        const int baseX = static_cast<int>(sq.squareX) * ww::format::kClipSize;
        const int baseY = static_cast<int>(sq.squareY) * ww::format::kClipSize;
        std::printf("  runtime: square[0] (%u,%u) clip cross-check %zu tiles, %zu mismatches\n",
                    sq.squareX, sq.squareY, words.size(), mismatches);
        std::printf("  runtime: off-world clip=0x%08x | tile (%d,%d,p0) standable=%d area=%d\n",
                    view.clipAt(-1, baseY, 0), baseX, baseY,
                    view.isStandable(baseX, baseY, 0) ? 1 : 0, view.areaAt(baseX, baseY, 0));
    }

    void dumpArtifact(const ww::format::ArtifactReader &reader)
    {
        const ww::format::ArtifactInfo &info = reader.info();
        std::printf("  formatVersion=%u cacheRevision=%u datasetHash=0x%08x\n", info.formatVersion,
                    info.cacheRevision, info.datasetHash);
        dumpCollision(reader);
        std::printf("  transitions: %zu records, %zu requirements, %zu chain steps\n",
                    reader.transitions().size(), reader.requirements().size(),
                    reader.chainSteps().size());
        dumpAbstraction(reader);
        std::printf("  alt: %u landmarks over %u areas\n", reader.landmarkCount(),
                    reader.altAreaCount());
        std::printf("  teleport: %zu wilderness regions, %zu no-tele zones (cutoff=%u)\n",
                    reader.wildernessRegions().size(), reader.noTeleZones().size(),
                    reader.wildernessCutoff());
        dumpRuntimeLookup(reader);
    }
}

int main(int argc, char **argv)
{
    std::printf("wwcli - WorldWalker dev harness\n");
    std::printf("artifact format version: %u\n", static_cast<unsigned>(WW_ARTIFACT_FORMAT_VERSION));
    if (argc < 2)
    {
        std::printf("usage: wwcli <artifact.wwa>\n");
        return 0;
    }

    try
    {
        const ww::format::ArtifactReader reader(argv[1]);
        std::printf("artifact: %s\n", argv[1]);
        dumpArtifact(reader);
    }
    catch (const std::exception &e)
    {
        std::printf("failed to load artifact: %s\n", e.what());
        return 1;
    }
    return 0;
}
