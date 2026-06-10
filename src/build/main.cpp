#include "build/AltLandmarks.h"
#include "build/AreaGraph.h"
#include "build/ArtifactWriter.h"
#include "build/CacheClient.h"
#include "build/CollisionBuilder.h"
#include "build/CollisionLookup.h"
#include "data/CrossingDeriver.h"
#include "data/DatasetLoader.h"
#include "data/FreshnessDeriver.h"
#include "data/TeleportZones.h"
#include "data/TransitionBuilder.h"
#include "data/Transitions.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>

// wwbuild — WorldWalker's offline artifact builder. Decodes the RS cache and
// datasets into the baked artifact the runtime planner loads. `collision` bakes
// just the directional clip grid; `build` adds the transitions, abstraction
// (area-graph), ALT landmark, and teleport-allowed sections — the full Phase 2
// artifact.
namespace
{
    // ALT landmark count — a tunable; well-spread landmarks over the area graph.
    constexpr std::size_t kLandmarkCount = 16;

    int usage()
    {
        std::printf("wwbuild - WorldWalker offline artifact builder\n");
        std::printf("usage:\n");
        std::printf("  wwbuild collision <cache_dir> <out.wwa> [--live]\n");
        std::printf("  wwbuild build <cache_dir> <dataset_dir> <out.wwa> [--live]\n");
        return 2;
    }

    // Poor-man's cache revision: hash the mtimes of the primary NXTCache data
    // files (main_file_cache.dat2 + .js5, with directory mtime as fallback)
    // into a stable uint32. NXTCacheLibrary's C ABI does not yet expose the
    // engine's own cache revision; once it does, route that through here
    // instead. Until then, an mtime-derived value is enough for the runtime's
    // "soft-warn on stale artifact" path to function — it changes every time
    // the cache does, which is the contract that matters.
    uint32_t deriveCacheRevision(const std::string &cacheDir)
    {
        namespace fs = std::filesystem;
        auto stamp = [](const fs::path &p) -> int64_t
        {
            std::error_code ec;
            const auto t = fs::last_write_time(p, ec);
            if (ec) { return 0; }
            return t.time_since_epoch().count();
        };
        const fs::path dir(cacheDir);
        const int64_t parts[] = {
            stamp(dir / "main_file_cache.dat2"),
            stamp(dir / "main_file_cache.js5"),
            stamp(dir),
        };
        // FNV-1a 32 over the three mtime words. A non-existent file
        // contributes 0; the resulting hash still changes when one of the
        // others does.
        uint32_t h = 2166136261u;
        for (const int64_t v : parts)
        {
            const auto u = static_cast<uint64_t>(v);
            for (int i = 0; i < 8; ++i)
            {
                h ^= static_cast<uint8_t>(u >> (i * 8));
                h *= 16777619u;
            }
        }
        return h;
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
            ww::build::writeArtifact(outPath, model, ww::data::TransitionModel{},
                                     ww::build::AreaGraphModel{}, ww::build::AltLandmarksModel{},
                                     ww::data::TeleportZonesModel{},
                                     deriveCacheRevision(cacheDir), 0u);
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

    // Map cache index: each archive id encodes a square (x = id & 0x7F, y = id >> 7).
    constexpr int kMapIndex = 5;

    struct TransitionBuildResult
    {
        ww::data::TransitionModel transitions;
        uint32_t datasetHash{};
        ww::data::TransitionReport finalize;
        ww::data::FreshnessReport freshness;
        ww::data::CrossingReport doors;
    };

    // Decode the interactable crossings (doors / ladders-stairs / climb-overs /
    // agility) across every map square once, so the transition derivers can name
    // the loc to interact with — the clip grid alone cannot.
    std::vector<ww::build::Crossing> gatherCrossings(const ww::build::CacheClient &cache)
    {
        std::vector<ww::build::Crossing> all;
        std::vector<ww::build::Crossing> square;
        for (int id : cache.archiveIds(kMapIndex))
        {
            const int squareX = id & 0x7F;
            const int squareY = id >> 7;
            if (cache.crossings(squareX, squareY, square))
            {
                all.insert(all.end(), square.begin(), square.end());
            }
        }
        return all;
    }

    // Load the datasets, derive cache-only vertical ladders/stairs and doors
    // (dataset priority), then finalize the union into the bakeable transition set.
    TransitionBuildResult assembleTransitions(const ww::build::CollisionModel &collision,
                                              const ww::build::CollisionLookup &lookup,
                                              const std::vector<ww::build::Crossing> &crossings,
                                              const std::string &datasetDir)
    {
        TransitionBuildResult out;
        const ww::data::LoadedDatasets datasets = ww::data::loadDatasets(datasetDir);
        out.datasetHash = datasets.datasetHash;

        const ww::data::TransitionModel derived = ww::data::deriveVerticalTransitions(
            collision, datasets.model, crossings, &out.freshness);
        const ww::data::TransitionModel doors =
            ww::data::deriveDoorTransitions(crossings, lookup, &out.doors);

        ww::data::TransitionModel combined = datasets.model;
        combined.transitions.insert(combined.transitions.end(),
                                    derived.transitions.begin(), derived.transitions.end());
        combined.transitions.insert(combined.transitions.end(),
                                    doors.transitions.begin(), doors.transitions.end());

        // Global teleports (spell + lodestone) are NOT baked: they are loaded
        // from editable JSON at runtime (ww_artifact_load_teleports) so scripters
        // can edit them without re-baking. Drop any that came in via the dataset
        // dir so the runtime-loaded set isn't double-counted. The .wwa carries
        // collision + areas + local/door crossings; runtime JSON carries globals.
        // (Today's doors-only dataset has none, so this normally drops zero.)
        std::size_t droppedGlobals = 0;
        {
            ww::data::TransitionModel localOnly;
            localOnly.transitions.reserve(combined.transitions.size());
            for (ww::data::Transition &t : combined.transitions)
            {
                if (t.isGlobalOrigin)
                {
                    ++droppedGlobals;
                    continue;
                }
                localOnly.transitions.push_back(std::move(t));
            }
            combined = std::move(localOnly);
        }
        if (droppedGlobals > 0)
        {
            std::printf("wwbuild: dropped %zu global teleport(s) from bake "
                        "(runtime-loaded via ww_artifact_load_teleports)\n", droppedGlobals);
        }

        out.transitions = ww::data::finalizeTransitions(combined, lookup, &out.finalize);
        return out;
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

            const std::vector<ww::build::Crossing> crossings = gatherCrossings(cache);

            const TransitionBuildResult tr =
                assembleTransitions(collision, lookup, crossings, datasetDir);

            ww::build::AreaGraphReport ag;
            const ww::build::AreaGraphModel abstraction =
                ww::build::buildAreaGraph(collision, lookup, tr.transitions, &ag);

            ww::build::AltLandmarksReport alt;
            const ww::build::AltLandmarksModel landmarks =
                ww::build::buildAltLandmarks(abstraction, kLandmarkCount, &alt);

            const ww::data::TeleportZonesModel teleportZones = ww::data::buildTeleportZones();

            ww::build::writeArtifact(outPath, collision, tr.transitions, abstraction, landmarks,
                                     teleportZones, deriveCacheRevision(cacheDir),
                                     tr.datasetHash);

            std::printf("build: %zu squares, %zu/%zu transitions -> %s\n",
                        collision.squares.size(), tr.finalize.kept, tr.finalize.input,
                        outPath.c_str());
            std::printf("  dropped: dangling=%zu selfloop=%zu dup=%zu | snapped dest=%zu\n",
                        tr.finalize.droppedDangling, tr.finalize.droppedSelfLoop,
                        tr.finalize.droppedDuplicate, tr.finalize.snappedDest);
            std::printf("  freshness: %zu vertical pairs -> +%zu derived (%zu suppressed by datasets, %zu climb-dir mismatch)\n",
                        tr.freshness.pairsFound, tr.freshness.kept,
                        tr.freshness.droppedDatasetConflict,
                        tr.freshness.droppedClimbMismatch);
            std::printf("  doors: %zu crossings -> +%zu directed hops (%zu blocked-origin, %zu no-edge, %zu foreign-edge)\n",
                        tr.doors.doorCrossings, tr.doors.emitted,
                        tr.doors.blockedOrigin, tr.doors.noEdge,
                        tr.doors.foreignEdgeSkipped);
            std::printf("  areas: %zu nodes, %zu edges, %zu grids (largest %zu tiles)\n",
                        ag.areaCount, ag.edgeCount, ag.gridCount, ag.largestArea);
            std::printf("  adjacency: %zu transitions linked | unresolved origin=%zu dest=%zu | intra=%zu global=%zu\n",
                        ag.resolvedTransitions, ag.unresolvedOrigin, ag.unresolvedDest,
                        ag.intraAreaSkipped, ag.globalSkipped);
            std::printf("  landmarks: %zu chosen from %zu candidate areas | reachable entries=%zu\n",
                        alt.landmarkCount, alt.candidateAreas, alt.reachablePairs);
            std::printf("  teleport zones: %zu wilderness regions, %zu no-tele zones (cutoff=%u)\n",
                        teleportZones.wilderness.size(), teleportZones.noTele.size(),
                        static_cast<unsigned>(teleportZones.defaultWildernessCutoff));
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
    if (std::strcmp(argv[1], "crossings") == 0)
    {
        if (argc < 5)
        {
            std::fprintf(stderr, "usage: wwbuild crossings <cache_dir> <sqx> <sqy>\n");
            return 2;
        }
        try
        {
            ww::build::CacheClient cache(argv[2], false);
            std::vector<ww::build::Crossing> xs;
            const bool present = cache.crossings(std::atoi(argv[3]), std::atoi(argv[4]), xs);
            std::printf("crossings: square (%s,%s) present=%d count=%zu\n", argv[3], argv[4],
                        present ? 1 : 0, xs.size());
            for (const ww::build::Crossing &c : xs)
            {
                std::printf("  kind=%u obj=%d (%d,%d,p%u) shape=%u rot=%u size=%ux%u opt=%u dir=%u\n",
                            c.kind, c.objectId, c.worldX, c.worldY, c.plane, c.shape, c.rotation,
                            c.sizeX, c.sizeY, c.optionIndex, c.climbDir);
            }
            return 0;
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr, "crossings: failed: %s\n", e.what());
            return 1;
        }
    }
    std::fprintf(stderr, "unknown command: %s\n", argv[1]);
    return usage();
}
