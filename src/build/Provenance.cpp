#include "build/Provenance.h"

#include "format/Artifact.h"

#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include <nlohmann/json.hpp>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>

namespace ww::build
{
    namespace
    {
        using json = nlohmann::json;

        constexpr std::uint64_t kFnvOffset64 = 14695981039346656037ull;
        constexpr std::uint64_t kFnvPrime64 = 1099511628211ull;

        void fold(std::uint64_t &hash, const void *data, std::size_t size)
        {
            const auto *bytes = static_cast<const std::uint8_t *>(data);
            for (std::size_t i = 0; i < size; ++i)
            {
                hash ^= bytes[i];
                hash *= kFnvPrime64;
            }
        }

        // Empty string / zero means "we did not record this", which must
        // serialize as null. Writing "" or 0 instead would be a claim, and an
        // invented provenance field is worse than an absent one — it cannot be
        // told apart from a real one later.
        json orNull(const std::string &value)
        {
            return value.empty() ? json(nullptr) : json(value);
        }

        json orNull(std::uint32_t value)
        {
            return value == 0u ? json(nullptr) : json(value);
        }

        std::string hex32(std::uint32_t value)
        {
            char buffer[11];
            std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
            return buffer;
        }
    }

    std::uint64_t collisionFingerprint(const CollisionModel &model)
    {
        std::uint64_t hash = kFnvOffset64;
        for (const SquareClip &square : model.squares)
        {
            const std::int32_t coords[] = {square.squareX, square.squareY};
            fold(hash, coords, sizeof(coords));
            fold(hash, &square.planeMask, sizeof(square.planeMask));
            fold(hash, square.words.data(), square.words.size() * sizeof(std::uint32_t));
        }
        return hash;
    }

    // Hex is how every other fingerprint in this codebase is read and compared
    // (the header's datasetHash prints as 0x...), and a 64-bit integer in JSON is
    // a portability trap in any consumer whose numbers are doubles. Strings, then.
    std::string hexOf(std::uint64_t value)
    {
        char buffer[19];
        std::snprintf(buffer, sizeof(buffer), "0x%016llX", static_cast<unsigned long long>(value));
        return buffer;
    }

    std::string utcTimestamp()
    {
        const std::time_t now = std::time(nullptr);
        std::tm utc{};
#if defined(_MSC_VER)
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return buffer;
    }

    std::string renderProvenanceJson(const BakeProvenance &provenance)
    {
        json files = json::array();
        for (const ww::data::DatasetFileInfo &file : provenance.datasetFiles)
        {
            files.push_back({
                {"name", file.name},
                {"bytes", file.bytes},
                {"fnv1a64", hexOf(file.fingerprint)},
            });
        }

        const json document = {
            {"schema", ww::format::kProvenanceSchema},
            {"builtAtUtc", provenance.builtAtUtc},
            {"command", provenance.command},
            {"sourceVersion", orNull(provenance.sourceVersion)},
            {"datasetVersion", orNull(provenance.datasetVersion)},
            {"wwbuildBuiltAt", provenance.wwbuildBuiltAt},
            {"cacheId", orNull(provenance.cacheId)},
            {"cacheKind", provenance.cacheKind},
            {"cacheDirMtime", provenance.cacheDirMtime},
            {"cacheIndexVersion", orNull(provenance.cacheIndexVersion)},
            {"serverVersion", orNull(provenance.serverVersion)},
            {"collisionFingerprint", hexOf(provenance.collisionFingerprint)},
            {"datasetHash", hex32(provenance.datasetHash)},
            {"datasetFiles", files},
            {"counts", {
                {"squares", provenance.squares},
                {"transitions", provenance.transitions},
                {"areas", provenance.areas},
                {"edges", provenance.edges},
                {"landmarks", provenance.landmarks},
                {"teleportZones", provenance.teleportZones},
            }},
        };
        return document.dump();
    }

    void writeProvenanceSidecar(const std::string &path, const std::string &text)
    {
        // Re-parse and re-dump so the sidecar is indented: the embedded copy is
        // compact because it rides inside the artifact, but the sidecar's whole
        // reason to exist is being read and diffed by people.
        const json document = json::parse(text);

        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            throw std::runtime_error("writeProvenanceSidecar: cannot open " + path);
        }
        const std::string pretty = document.dump(2);
        stream.write(pretty.data(), static_cast<std::streamsize>(pretty.size()));
        stream.put('\n');
        if (!stream)
        {
            throw std::runtime_error("writeProvenanceSidecar: write failed for " + path);
        }
    }
}
