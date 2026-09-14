#ifndef WORLDWALKER_BUILD_PROVENANCE_H
#define WORLDWALKER_BUILD_PROVENANCE_H

#include "build/CollisionBuilder.h"
#include "data/DatasetLoader.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ww::build
{
    // What one bake was made from. Assembled by wwbuild, serialized into the
    // artifact's Provenance section and into the sidecar JSON beside it, so
    // "where did this file come from" survives the machine that produced it.
    //
    // Nothing here feeds the planner. It exists because a required payload that
    // is untracked build output from an unrecorded hand-run is a bus factor of
    // one, and because a fixture and a production bake are indistinguishable
    // once they are two binaries in the same directory.
    struct BakeProvenance
    {
        std::string builtAtUtc;      // ISO-8601, second resolution
        std::string command;         // "build" or "collision"

        // Supplied by the caller (bake.ps1 reads them from git). Empty means
        // "not recorded" and serializes as null rather than as a guess — wwbuild
        // does not shell out to git to invent them.
        std::string sourceVersion;   // WorldWalker HEAD, with a dirty marker
        std::string datasetVersion;  // HEAD that last touched datasets/, with a dirty marker

        std::string wwbuildBuiltAt;  // compile stamp of the wwbuild binary itself

        // Cache identity. Deliberately NOT the path as given: that is routinely
        // C:\Users\<name>\..., and a developer's username has no business
        // travelling inside a shipped artifact. Basename plus kind is everything
        // a reader actually needs.
        std::string cacheId;         // final path component only
        std::string cacheKind;       // "local" or "local+live"
        std::int64_t cacheDirMtime{}; // the raw input to the legacy cacheRevision

        // The real cache/build revisions, when they can be had. Both are 0 =
        // "unavailable": NXTCacheLibrary's C ABI exposes neither Index::version
        // nor the JS5 server build today, so they serialize as null. They are
        // recorded because collisionFingerprint is the better signal anyway and
        // these are nice-to-have corroboration, not because anything waits on
        // them.
        std::uint32_t cacheIndexVersion{};
        std::uint32_t serverVersion{};

        // FNV-1a 64 over every baked clip word. THE fingerprint of the map data
        // that actually went into this artifact: machine-independent, immune to
        // mtimes, --live and sparse local caches, and equal across two bakes of
        // the same game data. This is what a staleness check should compare,
        // unlike the header's cacheRevision (see format/Artifact.h).
        std::uint64_t collisionFingerprint{};

        std::uint32_t datasetHash{};  // mirrors the header field, so the record stands alone
        std::vector<ww::data::DatasetFileInfo> datasetFiles;

        std::uint64_t squares{};
        std::uint64_t transitions{};
        std::uint64_t areas{};
        std::uint64_t edges{};
        std::uint64_t landmarks{};
        std::uint64_t teleportZones{};
    };

    // FNV-1a 64 over the clip words of every square, in the model's own square
    // order (which the builder keeps sorted by (squareY, squareX), so the result
    // is stable). Squares' coordinates are folded in as well as their words, so
    // the same clip data at different coordinates does not collide.
    std::uint64_t collisionFingerprint(const CollisionModel &model);

    // Current UTC as ISO-8601 ("2026-09-13T04:21:07Z").
    std::string utcTimestamp();

    // "0x" + 16 uppercase hex digits. Fingerprints are compared by eye far more
    // often than by machine, so they are rendered one way everywhere.
    std::string hexOf(std::uint64_t value);

    // The provenance document as compact UTF-8 JSON — the Provenance section's
    // body, and the base of the sidecar.
    std::string renderProvenanceJson(const BakeProvenance &provenance);

    // Write `json` to `path`, pretty-printed. The sidecar exists because the
    // people who most need this record — a reviewer reading a pull request, the
    // deploy script, someone asking what shipped — should not need a C++ tool and
    // a 10 MB binary to read 800 bytes of text. Throws std::runtime_error on an
    // I/O failure.
    void writeProvenanceSidecar(const std::string &path, const std::string &json);
}

#endif  // WORLDWALKER_BUILD_PROVENANCE_H
