#include "build/AltLandmarks.h"
#include "build/AreaGraph.h"
#include "build/ArtifactWriter.h"
#include "build/CacheClient.h"
#include "build/CollisionBuilder.h"
#include "build/CollisionLookup.h"
#include "build/Provenance.h"
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
        std::printf("  wwbuild collision <cache_dir> <out.wwa> [options]\n");
        std::printf("  wwbuild build <cache_dir> <dataset_dir> <out.wwa> [options]\n");
        std::printf("options:\n");
        std::printf("  --live                      complete missing cache data from the live JS5 servers\n");
        std::printf("  --allow-missing-datasets    bake even when the dataset dir holds none of them\n");
        std::printf("  --source-version <v>        record this as the WorldWalker revision baked from\n");
        std::printf("  --dataset-version <v>       record this as the datasets/ revision baked from\n");
        std::printf("  --no-sidecar                skip writing <out.wwa>.json beside the artifact\n");
        return 2;
    }

    // Trailing option flags shared by the bake commands. Unknown flags are
    // rejected rather than ignored: `--live` in the wrong slot used to be
    // silently dropped, which bakes a stale artifact without saying so.
    struct BuildFlags
    {
        bool isLive{false};
        bool allowMissingDatasets{false};
        bool writeSidecar{true};
        // Recorded verbatim into the provenance section. wwbuild does not shell
        // out to git to discover them: a build tool guessing at its own version
        // is how a wrong answer gets recorded confidently. The caller knows
        // (bake.ps1 passes `git describe`); when nobody says, it stays null.
        std::string sourceVersion;
        std::string datasetVersion;
    };

    // Reads the value of a `--flag <value>` pair. Returns false (having
    // complained) when the value is missing, so a trailing `--source-version`
    // fails the run instead of silently recording nothing.
    bool takeValue(int argc, char **argv, int &i, const char *name, std::string &outValue)
    {
        if (i + 1 >= argc)
        {
            std::fprintf(stderr, "wwbuild: %s needs a value\n", name);
            return false;
        }
        outValue = argv[++i];
        return true;
    }

    bool parseFlags(int argc, char **argv, int first, BuildFlags &outFlags)
    {
        for (int i = first; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--live") == 0)
            {
                outFlags.isLive = true;
            }
            else if (std::strcmp(argv[i], "--allow-missing-datasets") == 0)
            {
                outFlags.allowMissingDatasets = true;
            }
            else if (std::strcmp(argv[i], "--no-sidecar") == 0)
            {
                outFlags.writeSidecar = false;
            }
            else if (std::strcmp(argv[i], "--source-version") == 0)
            {
                if (!takeValue(argc, argv, i, "--source-version", outFlags.sourceVersion))
                {
                    return false;
                }
            }
            else if (std::strcmp(argv[i], "--dataset-version") == 0)
            {
                if (!takeValue(argc, argv, i, "--dataset-version", outFlags.datasetVersion))
                {
                    return false;
                }
            }
            else
            {
                std::fprintf(stderr, "wwbuild: unknown option %s\n", argv[i]);
                return false;
            }
        }
        return true;
    }

    int64_t stampOf(const std::filesystem::path &path)
    {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(path, ec);
        if (ec)
        {
            return 0;
        }
        return t.time_since_epoch().count();
    }

    // The header's legacy cacheRevision. IT DOES NOT MEAN WHAT ITS NAME SAYS,
    // and it is computed here unchanged only so nothing downstream shifts
    // behaviour in the commit that adds provenance.
    //
    // The two filenames below do not exist. An RS3 NXT cache directory is a set
    // of `js5-<N>.jcache` SQLite databases (see NXTCacheLibrary's RSCache::index,
    // which opens exactly that and nothing else); `main_file_cache.dat2` / `.js5`
    // are the *old Java client's* layout and appear nowhere else in this
    // workspace. So both stamps read 0 on every machine, always have, and the
    // hash collapses to a function of the cache directory's own mtime — which
    // moves when the client merely runs, and can sit still when the map data
    // changes. ADR 0005's "soft-warn on a stale artifact" has therefore never
    // worked.
    //
    // Do not "fix" this in place: changing the value changes what every shipped
    // client compares against. The honest fingerprint is collisionFingerprint in
    // the provenance record, and repointing the runtime at it is a deliberate
    // behaviour change that belongs in its own commit.
    uint32_t deriveCacheRevision(const std::string &cacheDir)
    {
        namespace fs = std::filesystem;
        const fs::path dir(cacheDir);
        const int64_t parts[] = {
            stampOf(dir / "main_file_cache.dat2"),  // always 0 — see above
            stampOf(dir / "main_file_cache.js5"),   // always 0 — see above
            stampOf(dir),
        };
        // FNV-1a 32 over the three mtime words.
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

    // The final component of the cache path, and nothing else. The path as given
    // is routinely under C:\Users\<name>, and a developer's username has no
    // business travelling inside an artifact that gets shipped or attached to a
    // pull request. This is not a private-repo-only precaution: a file written
    // under one policy outlives the era it was written in.
    std::string cacheIdOf(const std::string &cacheDir)
    {
        std::filesystem::path path(cacheDir);
        if (path.has_filename())
        {
            return path.filename().string();
        }
        return path.parent_path().filename().string();
    }

    // Assemble the record that answers "what was this baked from". `command`
    // distinguishes a full `build` from a collision-only bake, because the two
    // produce artifacts that are not interchangeable and are easy to confuse
    // once they are two files in a directory.
    ww::build::BakeProvenance describeBake(const char *command, const std::string &cacheDir,
                                           const BuildFlags &flags,
                                           const ww::build::CollisionModel &collision)
    {
        ww::build::BakeProvenance provenance;
        provenance.builtAtUtc = ww::build::utcTimestamp();
        provenance.command = command;
        provenance.sourceVersion = flags.sourceVersion;
        provenance.datasetVersion = flags.datasetVersion;
        provenance.wwbuildBuiltAt = std::string(__DATE__) + " " + __TIME__;
        provenance.cacheId = cacheIdOf(cacheDir);
        provenance.cacheKind = flags.isLive ? "local+live" : "local";
        provenance.cacheDirMtime = stampOf(std::filesystem::path(cacheDir));
        provenance.collisionFingerprint = ww::build::collisionFingerprint(collision);
        provenance.squares = collision.squares.size();
        return provenance;
    }

    // Write the artifact, then the sidecar beside it. The sidecar carries the
    // same document the artifact embeds: the deploy script, a reviewer reading a
    // pull request, and anyone asking what shipped should not need a C++ tool
    // and a 10 MB binary to read 800 bytes of text. Silent, so the caller keeps
    // control of the report order — nothing should announce a write before the
    // write has actually happened.
    void emitArtifact(const std::string &outPath, const BuildFlags &flags,
                      const ww::build::BakeProvenance &provenance,
                      const ww::build::CollisionModel &collision,
                      const ww::data::TransitionModel &transitions,
                      const ww::build::AreaGraphModel &abstraction,
                      const ww::build::AltLandmarksModel &landmarks,
                      const ww::data::TeleportZonesModel &teleportZones,
                      uint32_t cacheRevision, uint32_t datasetHash)
    {
        ww::build::ArtifactMeta meta;
        meta.cacheRevision = cacheRevision;
        meta.datasetHash = datasetHash;
        meta.provenanceJson = ww::build::renderProvenanceJson(provenance);

        ww::build::writeArtifact(outPath, collision, transitions, abstraction, landmarks,
                                 teleportZones, meta);
        if (flags.writeSidecar)
        {
            ww::build::writeProvenanceSidecar(outPath + ".json", meta.provenanceJson);
        }
    }

    void reportProvenance(const std::string &outPath, const BuildFlags &flags,
                          const ww::build::BakeProvenance &provenance)
    {
        std::printf("  provenance: collision=%s cache=%s/%s source=%s datasets=%s\n",
                    ww::build::hexOf(provenance.collisionFingerprint).c_str(),
                    provenance.cacheId.c_str(), provenance.cacheKind.c_str(),
                    provenance.sourceVersion.empty() ? "(unrecorded)" : provenance.sourceVersion.c_str(),
                    provenance.datasetVersion.empty() ? "(unrecorded)" : provenance.datasetVersion.c_str());
        if (flags.writeSidecar)
        {
            std::printf("  sidecar: %s.json\n", outPath.c_str());
        }
    }

    int runCollision(int argc, char **argv)
    {
        if (argc < 4)
        {
            return usage();
        }
        const std::string cacheDir = argv[2];
        const std::string outPath = argv[3];
        BuildFlags flags;
        if (!parseFlags(argc, argv, 4, flags))
        {
            return usage();
        }
        try
        {
            ww::build::CacheClient cache(cacheDir, flags.isLive);
            const ww::build::CollisionBuildResult decoded = ww::build::buildCollisionModel(cache);
            const ww::build::CollisionModel &model = decoded.model;
            if (model.squares.empty())
            {
                std::fprintf(stderr, "wwbuild collision: no map squares decoded from %s "
                                     "(is this a RuneScape cache directory?)\n", cacheDir.c_str());
                return 1;
            }
            const ww::build::BakeProvenance provenance =
                describeBake("collision", cacheDir, flags, model);
            emitArtifact(outPath, flags, provenance, model, ww::data::TransitionModel{},
                         ww::build::AreaGraphModel{}, ww::build::AltLandmarksModel{},
                         ww::data::TeleportZonesModel{}, deriveCacheRevision(cacheDir), 0u);
            std::printf("collision: %zu squares written to %s (%d archives skipped)\n",
                        model.squares.size(), outPath.c_str(), decoded.skippedArchives);
            reportProvenance(outPath, flags, provenance);
            return 0;
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr, "wwbuild collision failed: %s\n", e.what());
            return 1;
        }
    }

    struct TransitionBuildResult
    {
        ww::data::TransitionModel transitions;
        uint32_t datasetHash{};
        ww::data::TransitionReport finalize;
        ww::data::FreshnessReport freshness;
        ww::data::CrossingReport doors;
    };

    // Given the loaded datasets, derive cache-only vertical ladders/stairs and
    // doors (dataset priority), then finalize the union into the bakeable
    // transition set.
    TransitionBuildResult assembleTransitions(const ww::build::CollisionModel &collision,
                                              const ww::build::CollisionLookup &lookup,
                                              const std::vector<ww::build::Crossing> &crossings,
                                              const ww::data::LoadedDatasets &datasets)
    {
        TransitionBuildResult out;
        out.datasetHash = datasets.datasetHash;

        // Both cache derivers defer to the curated datasets on a shared origin
        // tile (ADR 0003), so both read the same origin set.
        const ww::data::DatasetOrigins datasetOrigins =
            ww::data::collectDatasetOrigins(datasets.model);
        const ww::data::TransitionModel derived = ww::data::deriveVerticalTransitions(
            collision, datasets.model, crossings, &out.freshness);
        const ww::data::TransitionModel doors =
            ww::data::deriveDoorTransitions(crossings, lookup, datasetOrigins, &out.doors);

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

    // The per-phase bake summary. Lifted out of runBuild because it is a dozen
    // printfs that say nothing about control flow, and burying the sequence of
    // build steps under them made the one thing runBuild is for hard to read.
    void reportBuild(const std::string &outPath, const ww::build::CollisionModel &collision,
                     const TransitionBuildResult &tr, const ww::build::AreaGraphReport &ag,
                     const ww::build::AltLandmarksReport &alt,
                     const ww::data::TeleportZonesModel &teleportZones)
    {
        std::printf("build: %zu squares, %zu/%zu transitions -> %s\n",
                    collision.squares.size(), tr.finalize.kept, tr.finalize.input,
                    outPath.c_str());
        std::printf("  dropped: dangling=%zu selfloop=%zu dup=%zu | snapped dest=%zu\n",
                    tr.finalize.droppedDangling, tr.finalize.droppedSelfLoop,
                    tr.finalize.droppedDuplicate, tr.finalize.snappedDest);
        std::printf("  freshness: %zu vertical pairs -> +%zu derived (%zu suppressed by datasets, %zu climb-dir mismatch, %zu no-option)\n",
                    tr.freshness.pairsFound, tr.freshness.kept,
                    tr.freshness.droppedDatasetConflict,
                    tr.freshness.droppedClimbMismatch,
                    tr.freshness.droppedNoOption);
        std::printf("  doors: %zu crossings -> +%zu directed hops (%zu suppressed by datasets, %zu blocked-origin, %zu no-option, %zu no-edge, %zu foreign-edge)\n",
                    tr.doors.doorCrossings, tr.doors.emitted,
                    tr.doors.droppedDatasetConflict,
                    tr.doors.blockedOrigin, tr.doors.noOption, tr.doors.noEdge,
                    tr.doors.foreignEdgeSkipped);
        std::printf("  areas: %zu nodes, %zu edges, %zu grids (largest %zu tiles)\n",
                    ag.areaCount, ag.edgeCount, ag.gridCount, ag.largestArea);
        std::printf("  adjacency: %zu transitions linked | unresolved origin=%zu dest=%zu | "
                    "intra-only=%zu intra-edges=%zu global=%zu\n",
                    ag.resolvedTransitions, ag.unresolvedOrigin, ag.unresolvedDest,
                    ag.intraAreaOnly, ag.intraAreaSkipped, ag.globalSkipped);
        std::printf("  landmarks: %zu chosen from %zu candidate areas | reachable entries=%zu\n",
                    alt.landmarkCount, alt.candidateAreas, alt.reachablePairs);
        std::printf("  teleport zones: %zu wilderness regions, %zu no-tele zones (cutoff=%u)\n",
                    teleportZones.wilderness.size(), teleportZones.noTele.size(),
                    static_cast<unsigned>(teleportZones.defaultWildernessCutoff));
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
        BuildFlags flags;
        if (!parseFlags(argc, argv, 5, flags))
        {
            return usage();
        }
        try
        {
            // Datasets first: a wrong directory fails here in milliseconds instead
            // of after the minutes-long cache decode. Files may be absent one at a
            // time (the in-tree set has no teleport_chains.json), but a directory with
            // none of them is a typo, and baking on regardless yields a valid artifact
            // that walks nowhere, so that is refused unless the caller opted in.
            const ww::data::LoadedDatasets datasets = ww::data::loadDatasets(datasetDir);
            if (datasets.filesFound == 0 && !flags.allowMissingDatasets)
            {
                std::fprintf(stderr,
                             "wwbuild build: no dataset files found under %s "
                             "(pass --allow-missing-datasets to bake without them)\n",
                             datasetDir.c_str());
                return 1;
            }
            ww::build::CacheClient cache(cacheDir, flags.isLive);
            // One pass over the map index yields both the clip grid and the
            // interactable crossings (doors / ladders-stairs / climb-overs /
            // agility) the transition derivers need to name the loc to interact
            // with — the clip grid alone cannot.
            const ww::build::CollisionBuildResult decoded = ww::build::buildCollisionModel(cache);
            const ww::build::CollisionModel &collision = decoded.model;
            // An empty decode is a wrong cache directory, not a world with no
            // squares: refuse rather than publish an artifact that walks nowhere.
            if (collision.squares.empty())
            {
                std::fprintf(stderr, "wwbuild build: no map squares decoded from %s "
                                     "(is this a RuneScape cache directory?)\n", cacheDir.c_str());
                return 1;
            }
            ww::build::CollisionLookup lookup(collision);

            const TransitionBuildResult tr =
                assembleTransitions(collision, lookup, decoded.crossings, datasets);

            ww::build::AreaGraphReport ag;
            const ww::build::AreaGraphModel abstraction =
                ww::build::buildAreaGraph(collision, lookup, tr.transitions, &ag);

            ww::build::AltLandmarksReport alt;
            const ww::build::AltLandmarksModel landmarks =
                ww::build::buildAltLandmarks(abstraction, kLandmarkCount, &alt);

            const ww::data::TeleportZonesModel teleportZones = ww::data::buildTeleportZones();

            ww::build::BakeProvenance provenance = describeBake("build", cacheDir, flags, collision);
            provenance.datasetHash = tr.datasetHash;
            provenance.datasetFiles = datasets.files;
            provenance.transitions = tr.transitions.transitions.size();
            provenance.areas = ag.areaCount;
            provenance.edges = ag.edgeCount;
            provenance.landmarks = alt.landmarkCount;
            provenance.teleportZones = teleportZones.wilderness.size() + teleportZones.noTele.size();

            emitArtifact(outPath, flags, provenance, collision, tr.transitions, abstraction,
                         landmarks, teleportZones, deriveCacheRevision(cacheDir), tr.datasetHash);

            reportBuild(outPath, collision, tr, ag, alt, teleportZones);
            reportProvenance(outPath, flags, provenance);
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
