#include "build/ArtifactWriter.h"
#include "build/CacheClient.h"
#include "build/CollisionBuilder.h"
#include "build/CollisionLookup.h"
#include "data/DatasetLoader.h"
#include "data/TransitionBuilder.h"
#include "data/Transitions.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

// wwbuild — WorldWalker's offline artifact builder. Decodes the RS cache and
// datasets into the baked artifact the runtime planner loads. `collision` bakes
// just the directional clip grid; `build` adds the ingested transitions section.
// Abstraction, ALT, and the teleport map land in subsequent Phase 2 sub-steps.
namespace
{
    int usage()
    {
        std::printf("wwbuild - WorldWalker offline artifact builder\n");
        std::printf("usage:\n");
        std::printf("  wwbuild collision <cache_dir> <out.wwa> [--live]\n");
        std::printf("  wwbuild build <cache_dir> <dataset_dir> <out.wwa> [--live]\n");
        return 2;
    }

    int runCollision(int argc, char **argv)
    {
        if (argc < 4)
        {
            return usage();
        }
        const std::string cacheDir = argv[2];
        const std::string outPath = argv[3];
        const bool live = (argc > 4 && std::strcmp(argv[4], "--live") == 0);
        try
        {
            ww::build::CacheClient cache(cacheDir, live);
            int skipped = 0;
            ww::build::CollisionModel model = ww::build::buildCollisionModel(cache, &skipped);
            ww::build::writeArtifact(outPath, model, ww::data::TransitionModel{}, 0u, 0u);
            std::printf("collision: %zu squares written to %s (%d archives skipped)\n",
                        model.squares.size(), outPath.c_str(), skipped);
            return 0;
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr, "wwbuild collision failed: %s\n", e.what());
            return 1;
        }
    }

    int runBuild(int argc, char **argv)
    {
        if (argc < 5)
        {
            return usage();
        }
        const std::string cacheDir = argv[2];
        const std::string datasetDir = argv[3];
        const std::string outPath = argv[4];
        const bool live = (argc > 5 && std::strcmp(argv[5], "--live") == 0);
        try
        {
            ww::build::CacheClient cache(cacheDir, live);
            int skipped = 0;
            ww::build::CollisionModel collision = ww::build::buildCollisionModel(cache, &skipped);
            ww::build::CollisionLookup lookup(collision);

            const ww::data::LoadedDatasets datasets = ww::data::loadDatasets(datasetDir);
            ww::data::TransitionReport report;
            const ww::data::TransitionModel transitions =
                ww::data::finalizeTransitions(datasets.model, lookup, &report);

            ww::build::writeArtifact(outPath, collision, transitions, 0u, datasets.datasetHash);

            std::printf("build: %zu squares, %zu/%zu transitions -> %s\n",
                        collision.squares.size(), report.kept, report.input, outPath.c_str());
            std::printf("  dropped: dangling=%zu selfloop=%zu dup=%zu | snapped dest=%zu\n",
                        report.droppedDangling, report.droppedSelfLoop, report.droppedDuplicate,
                        report.snappedDest);
            return 0;
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr, "wwbuild build failed: %s\n", e.what());
            return 1;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        return usage();
    }
    if (std::strcmp(argv[1], "collision") == 0)
    {
        return runCollision(argc, argv);
    }
    if (std::strcmp(argv[1], "build") == 0)
    {
        return runBuild(argc, argv);
    }
    std::fprintf(stderr, "unknown command: %s\n", argv[1]);
    return usage();
}
