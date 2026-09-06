#include "cli/CrossCheck.h"

#include "format/ArtifactReader.h"
#include "format/ClipFlags.h"
#include "format/Zlib.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// Phase 6a — Oracle decode cross-check.
//
// Reads the prior nav stack's COLL/v8 collision_map.bin and compares it tile
// by tile against the clip section of a WorldWalker artifact. Bit layouts are
// byte-equal in the lower 26 bits (8 wall edges + OBJECT + FLOOR_DECORATION +
// FLOOR + BLOCKED, see collision_map.h:9 vs ClipFlags.h:20) and in the upper
// specials at 0x02000000..0x40000000 (WATER, DOOR, AGILITY_SHORTCUT,
// PLANE_CHANGE, CLIMBOVER, and the oracle's TRANSPORT). The diff buckets each
// XOR'd bit into one of those three categories and reports per-bit histograms
// plus a top-N most-divergent-square table so a bulk decoder bug shows up as a
// uniform XOR signature, while sparse cache-revision terrain drift shows up as
// per-square outliers.
namespace
{
    // ---- Local mirror of the COLL/v8 file format ----------------------------
    //
    // The prior collision_gen writes a packed header followed by RegionEntry[],
    // then a blob region holding zlib streams. We only need the region table and
    // its blobs; the door/shortcut/teleport extras after them are skipped. Keep
    // these defs identical to collision_gen/main.cpp's CollisionFileHeader and
    // RegionEntry — they live here vendored so wwcli doesn't link against the
    // prior repo.
#pragma pack(push, 1)
    struct OracleHeader
    {
        char     magic[4];                 // 'C','O','L','L'
        uint32_t version;                  // expected 8
        uint32_t regionCount;
        uint32_t planes;                   // expected 4
        uint32_t doorCount;
        uint32_t doorDataOffset;
        uint32_t shortcutCount;
        uint32_t shortcutDataOffset;
        uint32_t planeTransitionCount;
        uint32_t planeTransitionDataOffset;
        uint32_t climboverCount;
        uint32_t climboverDataOffset;
        uint32_t transportLinkCount;
        uint32_t transportLinkDataOffset;
        uint32_t teleportCount;
        uint32_t teleportDataOffset;
        uint32_t passageCount;
        uint32_t passageDataOffset;
    };
    static_assert(sizeof(OracleHeader) == 72, "OracleHeader must match COLL/v8 on disk");

    struct OracleRegionEntry
    {
        uint16_t mapSquareX;
        uint16_t mapSquareY;
        uint32_t dataOffset;        // absolute file offset
        uint32_t compressedSize;    // 0 for empty regions
        uint8_t  planeMask;         // bit p set if plane p stored
        uint8_t  padding[3];
    };
    static_assert(sizeof(OracleRegionEntry) == 16, "OracleRegionEntry must match COLL/v8 on disk");
#pragma pack(pop)

    constexpr std::uint32_t kPlaneWords = 64u * 64u;
    constexpr std::uint32_t kPlaneBytes = kPlaneWords * sizeof(std::uint32_t);

    // One uncompressed plane out of a region, indexed by game-plane (0..3).
    // Missing planes (mask bit clear) are not allocated — callers check the
    // mask before reading.
    struct OracleRegion
    {
        std::uint16_t squareX{};
        std::uint16_t squareY{};
        std::uint8_t  planeMask{};
        std::array<std::vector<std::uint32_t>, 4> planes;
    };

    // ---- File read helpers --------------------------------------------------

    std::vector<std::uint8_t> readAll(const char *path)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            throw std::runtime_error(std::string{"open failed: "} + path);
        }
        const std::streamoff size = stream.tellg();
        if (size < 0)
        {
            throw std::runtime_error(std::string{"tell failed: "} + path);
        }
        stream.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
        stream.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!stream)
        {
            throw std::runtime_error(std::string{"short read: "} + path);
        }
        return out;
    }

    // Parse the file bytes into a (squareX, squareY)-keyed map. Inflates every
    // non-empty region eagerly — the whole 200-region oracle is < 10 MB raw, so
    // up-front decompression keeps the diff loop linear and dodges the file
    // pointer / blob seek bookkeeping.
    std::unordered_map<std::uint32_t, OracleRegion>
    parseOracle(const std::vector<std::uint8_t> &bytes)
    {
        if (bytes.size() < sizeof(OracleHeader))
        {
            throw std::runtime_error("oracle: file too small for header");
        }
        OracleHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (header.magic[0] != 'C' || header.magic[1] != 'O' || header.magic[2] != 'L'
            || header.magic[3] != 'L')
        {
            throw std::runtime_error("oracle: bad magic (expected COLL)");
        }
        if (header.version != 8u)
        {
            throw std::runtime_error("oracle: unsupported format version (expected 8)");
        }
        if (header.planes != 4u)
        {
            throw std::runtime_error("oracle: unsupported plane count (expected 4)");
        }
        const std::size_t dirOffset = sizeof(OracleHeader);
        const std::size_t dirBytes  = static_cast<std::size_t>(header.regionCount)
                                    * sizeof(OracleRegionEntry);
        if (dirOffset + dirBytes > bytes.size())
        {
            throw std::runtime_error("oracle: region table overflows file");
        }

        std::unordered_map<std::uint32_t, OracleRegion> out;
        out.reserve(header.regionCount);
        for (std::uint32_t i = 0; i < header.regionCount; ++i)
        {
            OracleRegionEntry entry{};
            std::memcpy(&entry, bytes.data() + dirOffset + i * sizeof(entry), sizeof(entry));
            if (entry.compressedSize == 0u || entry.planeMask == 0u)
            {
                continue;                                  // empty region
            }
            if (entry.dataOffset > bytes.size()
                || static_cast<std::size_t>(entry.dataOffset) + entry.compressedSize > bytes.size())
            {
                throw std::runtime_error("oracle: region blob overflows file");
            }
            const std::size_t storedPlanes = static_cast<std::size_t>(
                std::popcount(static_cast<std::uint32_t>(entry.planeMask)));
            const std::size_t rawBytes = storedPlanes * kPlaneBytes;
            const std::vector<std::uint8_t> raw = ww::format::zlibDecompress(
                bytes.data() + entry.dataOffset, entry.compressedSize, rawBytes);

            OracleRegion region{};
            region.squareX   = entry.mapSquareX;
            region.squareY   = entry.mapSquareY;
            region.planeMask = entry.planeMask;
            std::size_t cursor = 0;
            for (int p = 0; p < 4; ++p)
            {
                if ((entry.planeMask & (1u << p)) == 0u)
                {
                    continue;
                }
                region.planes[static_cast<std::size_t>(p)].resize(kPlaneWords);
                std::memcpy(region.planes[static_cast<std::size_t>(p)].data(),
                            raw.data() + cursor, kPlaneBytes);
                cursor += kPlaneBytes;
            }
            const std::uint32_t key = (static_cast<std::uint32_t>(entry.mapSquareY) << 16)
                                    | entry.mapSquareX;
            out.emplace(key, std::move(region));
        }
        return out;
    }

    // ---- Bit-category classification ----------------------------------------
    //
    // Bit categories used to bucket XOR signatures. The block-mask group is
    // every bit *both* layouts agree on; specials are the upper-byte bits; an
    // "other" bucket catches anything the diff doesn't recognise (e.g. the
    // oracle's TRANSPORT bit, which WorldWalker has no equivalent for, would
    // land in specials too — they share the 0x40000000 slot).
    constexpr std::uint32_t kBlockMaskBits =
        ww::format::CLIP_WALL_NW | ww::format::CLIP_WALL_N | ww::format::CLIP_WALL_NE
        | ww::format::CLIP_WALL_E | ww::format::CLIP_WALL_SE | ww::format::CLIP_WALL_S
        | ww::format::CLIP_WALL_SW | ww::format::CLIP_WALL_W
        | ww::format::CLIP_OBJECT | ww::format::CLIP_FLOOR_DECORATION
        | ww::format::CLIP_FLOOR  | ww::format::CLIP_BLOCKED;
    constexpr std::uint32_t kSpecialBits =
        ww::format::CLIP_WATER | ww::format::CLIP_DOOR | ww::format::CLIP_AGILITY_SHORTCUT
        | ww::format::CLIP_PLANE_CHANGE | ww::format::CLIP_CLIMBOVER
        | 0x40000000u;                                   // oracle's TRANSPORT slot
    constexpr std::uint32_t kKnownBits = kBlockMaskBits | kSpecialBits;

    struct DivergentSquare
    {
        std::uint16_t squareX{};
        std::uint16_t squareY{};
        std::uint64_t mismatchTiles{};
    };

    struct DiffStats
    {
        std::uint64_t squaresOverlap{};
        std::uint64_t squaresOnlyWw{};
        std::uint64_t squaresOnlyOracle{};
        std::uint64_t planesCompared{};
        std::uint64_t tilesCompared{};
        std::uint64_t tilesMismatched{};
        std::uint64_t mismatchBlockMask{};
        std::uint64_t mismatchSpecials{};
        std::uint64_t mismatchOther{};
        std::array<std::uint64_t, 32> perBit{};
        std::vector<DivergentSquare> divergent;          // sorted descending at end
    };

    // Diff one (square, plane) word run, accumulating into stats and counting
    // the mismatched tiles for this square so it can be ranked later.
    std::uint64_t diffPlane(const std::vector<std::uint32_t> &wwWords,
                            std::size_t wwPlaneBase,
                            const std::vector<std::uint32_t> &oraclePlane, DiffStats &stats)
    {
        std::uint64_t mismatches = 0;
        for (std::size_t i = 0; i < kPlaneWords; ++i)
        {
            const std::uint32_t a = wwWords[wwPlaneBase + i];
            const std::uint32_t b = oraclePlane[i];
            const std::uint32_t x = a ^ b;
            ++stats.tilesCompared;
            if (x == 0u)
            {
                continue;
            }
            ++mismatches;
            ++stats.tilesMismatched;
            if ((x & kBlockMaskBits) != 0u) { ++stats.mismatchBlockMask; }
            if ((x & kSpecialBits)   != 0u) { ++stats.mismatchSpecials;  }
            if ((x & ~kKnownBits)    != 0u) { ++stats.mismatchOther;     }
            std::uint32_t bits = x;
            while (bits != 0u)
            {
                const int bit = std::countr_zero(bits);
                ++stats.perBit[static_cast<std::size_t>(bit)];
                bits &= bits - 1u;
            }
        }
        return mismatches;
    }

    const char *bitLabel(int bit)
    {
        switch (1u << static_cast<std::uint32_t>(bit))
        {
            case ww::format::CLIP_WALL_NW:          return "WALL_NW";
            case ww::format::CLIP_WALL_N:           return "WALL_N";
            case ww::format::CLIP_WALL_NE:          return "WALL_NE";
            case ww::format::CLIP_WALL_E:           return "WALL_E";
            case ww::format::CLIP_WALL_SE:          return "WALL_SE";
            case ww::format::CLIP_WALL_S:           return "WALL_S";
            case ww::format::CLIP_WALL_SW:          return "WALL_SW";
            case ww::format::CLIP_WALL_W:           return "WALL_W";
            case ww::format::CLIP_OBJECT:           return "OBJECT";
            case ww::format::CLIP_FLOOR_DECORATION: return "FLOOR_DEC";
            case ww::format::CLIP_FLOOR:            return "FLOOR";
            case ww::format::CLIP_BLOCKED:          return "BLOCKED";
            case ww::format::CLIP_WATER:            return "WATER";
            case ww::format::CLIP_DOOR:             return "DOOR";
            case ww::format::CLIP_AGILITY_SHORTCUT: return "AGILITY";
            case ww::format::CLIP_PLANE_CHANGE:     return "PLANE_CHG";
            case ww::format::CLIP_CLIMBOVER:        return "CLIMBOVER";
            // CLIP_CLIMBOVER is 0x20000000 (bit 29) and has its own case above;
            // bit 30 is the oracle's TRANSPORT slot, which WorldWalker does not
            // produce at all, so a difference here is expected drift.
            case 0x40000000u:                       return "bit30(TRANSPORT-oracle)";
            default:                                return "?";
        }
    }

    void printSummary(const DiffStats &stats)
    {
        std::printf("crosscheck: squares overlap=%llu only-ww=%llu only-oracle=%llu\n",
                    static_cast<unsigned long long>(stats.squaresOverlap),
                    static_cast<unsigned long long>(stats.squaresOnlyWw),
                    static_cast<unsigned long long>(stats.squaresOnlyOracle));
        std::printf("crosscheck: planes compared=%llu, tiles compared=%llu, mismatched=%llu (%.3f%%)\n",
                    static_cast<unsigned long long>(stats.planesCompared),
                    static_cast<unsigned long long>(stats.tilesCompared),
                    static_cast<unsigned long long>(stats.tilesMismatched),
                    stats.tilesCompared == 0 ? 0.0
                        : 100.0 * static_cast<double>(stats.tilesMismatched)
                                / static_cast<double>(stats.tilesCompared));
        std::printf("crosscheck: by-category — blockmask=%llu specials=%llu other=%llu\n",
                    static_cast<unsigned long long>(stats.mismatchBlockMask),
                    static_cast<unsigned long long>(stats.mismatchSpecials),
                    static_cast<unsigned long long>(stats.mismatchOther));

        // Per-bit histogram: only print bits that actually disagreed.
        std::printf("crosscheck: per-bit XOR counts (descending):\n");
        std::array<std::pair<int, std::uint64_t>, 32> rows{};
        for (int b = 0; b < 32; ++b)
        {
            rows[static_cast<std::size_t>(b)] = {b, stats.perBit[static_cast<std::size_t>(b)]};
        }
        std::sort(rows.begin(), rows.end(),
                  [](const auto &lhs, const auto &rhs) { return lhs.second > rhs.second; });
        for (const auto &row : rows)
        {
            if (row.second == 0u)
            {
                break;
            }
            std::printf("  bit %2d (0x%08x %-22s): %llu\n", row.first,
                        1u << static_cast<std::uint32_t>(row.first), bitLabel(row.first),
                        static_cast<unsigned long long>(row.second));
        }

        std::printf("crosscheck: top divergent squares:\n");
        const std::size_t topN = std::min<std::size_t>(stats.divergent.size(), 10);
        for (std::size_t i = 0; i < topN; ++i)
        {
            const DivergentSquare &d = stats.divergent[i];
            std::printf("  (%u,%u): %llu mismatched tiles\n", d.squareX, d.squareY,
                        static_cast<unsigned long long>(d.mismatchTiles));
        }
    }

    // CI gate rate over the block-mask bits — the eight wall edges plus OBJECT,
    // FLOOR_DECORATION, FLOOR and BLOCKED, the bits the two layouts genuinely
    // agree on. Two snapshots of adjacent cache revisions differ on a handful of
    // tiles where terrain actually changed; a decoder regression (an inverted
    // wall nibble, a dropped BLOCKED bit) disagrees on a large fraction of every
    // square. 1e-4 sits well above the former and orders of magnitude below the
    // latter.
    constexpr double kBlockMaskFailRate = 1e-4;

    // Exit code for the run, with the reasoning printed. Two independent gates:
    //
    //   * "other" bits — outside the agreed contract on both sides. These should
    //     never differ at all, so any count is a producer bug.
    //   * block-mask bits — the shared collision contract. Gated on a RATE, not
    //     a count, so genuine terrain drift passes and a systematic decode fault
    //     does not. This is the gate that catches a wholesale wall-decoder
    //     inversion, which the "other" bucket cannot see by construction.
    //
    // The upper specials (WATER, DOOR, AGILITY, PLANE_CHANGE, CLIMBOVER, and
    // bit 30, the oracle's TRANSPORT slot that WorldWalker never sets) are
    // intentional WW-vs-oracle drift and gate nothing.
    int gateVerdict(const DiffStats &stats)
    {
        const double blockRate = stats.tilesCompared == 0
            ? 0.0
            : static_cast<double>(stats.mismatchBlockMask)
                  / static_cast<double>(stats.tilesCompared);
        const bool blockFail = blockRate > kBlockMaskFailRate;
        const bool otherFail = stats.mismatchOther > 0;
        std::printf("crosscheck: gate — blockmask rate=%.6f%% (limit %.6f%%) %s;"
                    " other-bit mismatches=%llu %s\n",
                    100.0 * blockRate, 100.0 * kBlockMaskFailRate, blockFail ? "FAIL" : "ok",
                    static_cast<unsigned long long>(stats.mismatchOther),
                    otherFail ? "FAIL" : "ok");
        if (stats.tilesCompared == 0)
        {
            std::printf("crosscheck: gate — no overlapping tiles were compared;"
                        " NOTHING WAS VERIFIED\n");
            return 1;
        }
        return (blockFail || otherFail) ? 1 : 0;
    }
}

int runCrossCheck(const char *wwaPath, const char *oraclePath)
{
    try
    {
        const ww::format::ArtifactReader reader(wwaPath);
        if (!reader.hasCollision())
        {
            std::printf("crosscheck: artifact has no collision section\n");
            return 1;
        }
        std::printf("crosscheck: wwa=%s\n", wwaPath);
        std::printf("crosscheck: oracle=%s\n", oraclePath);

        const std::vector<std::uint8_t> oracleBytes = readAll(oraclePath);
        const std::unordered_map<std::uint32_t, OracleRegion> oracle = parseOracle(oracleBytes);
        std::printf("crosscheck: oracle non-empty regions=%zu\n", oracle.size());

        DiffStats stats{};
        std::unordered_map<std::uint32_t, std::uint8_t> wwKeys;
        wwKeys.reserve(reader.collisionSquares().size());
        for (const ww::format::CollisionSquareEntry &sq : reader.collisionSquares())
        {
            const std::uint32_t key = (static_cast<std::uint32_t>(sq.squareY) << 16) | sq.squareX;
            wwKeys.emplace(key, sq.planeMask);
        }

        for (const ww::format::CollisionSquareEntry &sq : reader.collisionSquares())
        {
            const std::uint32_t key = (static_cast<std::uint32_t>(sq.squareY) << 16) | sq.squareX;
            const auto oIt = oracle.find(key);
            if (oIt == oracle.end())
            {
                ++stats.squaresOnlyWw;
                continue;
            }
            ++stats.squaresOverlap;

            std::vector<std::uint32_t> wwWords;
            if (!reader.decompressSquare(sq.squareX, sq.squareY, wwWords))
            {
                throw std::runtime_error("decompressSquare returned false on indexed square");
            }
            std::uint64_t squareMismatches = 0;
            for (int p = 0; p < 4; ++p)
            {
                if ((oIt->second.planeMask & (1u << p)) == 0u)
                {
                    continue;
                }
                ++stats.planesCompared;
                const std::size_t wwPlaneBase = static_cast<std::size_t>(p) * kPlaneWords;
                squareMismatches += diffPlane(wwWords, wwPlaneBase,
                                              oIt->second.planes[static_cast<std::size_t>(p)],
                                              stats);
            }
            if (squareMismatches > 0)
            {
                stats.divergent.push_back({sq.squareX, sq.squareY, squareMismatches});
            }
        }
        for (const auto &kv : oracle)
        {
            if (wwKeys.find(kv.first) == wwKeys.end())
            {
                ++stats.squaresOnlyOracle;
            }
        }
        std::sort(stats.divergent.begin(), stats.divergent.end(),
                  [](const DivergentSquare &a, const DivergentSquare &b)
                  { return a.mismatchTiles > b.mismatchTiles; });

        printSummary(stats);
        return gateVerdict(stats);
    }
    catch (const std::exception &e)
    {
        std::printf("crosscheck: failed: %s\n", e.what());
        return 1;
    }
}
