#ifndef WORLDWALKER_BUILD_CACHECLIENT_H
#define WORLDWALKER_BUILD_CACHECLIENT_H

#include "c_api/nxtcache_c.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ww::build
{
    // One map square's decoded directional clip: format::kClipWordsPerSquare
    // words, plane-major (x west-east 0..63, y south-north 0..63).
    struct SquareClip
    {
        int squareX{};
        int squareY{};
        uint8_t planeMask{};
        std::vector<uint32_t> words;
    };

    // Which interaction class a crossing loc belongs to. Mirrors the
    // NXT_CROSSING_* macros and maps::CrossingKind.
    enum class CrossingKind : uint8_t
    {
        Door        = 0,
        ClimbOver   = 1,
        PlaneChange = 2,
        Agility     = 3,
    };

    // An interactable scenery crossing (door / climb-over / ladder-stair /
    // agility shortcut) at an absolute world tile, carrying the loc id +
    // geometry the transition deriver needs. WorldWalker-side mirror of the
    // C ABI's nxt_crossing; CacheClient::crossings translates between them.
    struct Crossing
    {
        int32_t objectId{};
        int32_t worldX{};
        int32_t worldY{};
        uint8_t plane{};
        uint8_t shape{};
        uint8_t rotation{};
        uint8_t kind{};         // CrossingKind
        uint8_t sizeX{1};
        uint8_t sizeY{1};
        uint8_t optionIndex{};  // 0-based; 0xFF if none
        uint8_t climbDir{};     // PlaneChange: bit0 up, bit1 down
    };

    inline constexpr uint8_t kClimbUp   = 0x1;
    inline constexpr uint8_t kClimbDown = 0x2;

    // Crossing::optionIndex when the loc exposes no clickable option (a
    // varbit-only door, decorative scenery): nothing the executor can interact
    // with, so no transition may be derived through it.
    inline constexpr uint8_t kCrossingNoOption = 0xFF;

    // RAII wrapper over the NXTCache C ABI for offline cache decode.
    // Non-copyable. The constructor throws std::runtime_error on open failure.
    class CacheClient
    {
    public:
        CacheClient(const std::string &cachePath, bool enableLiveFallback);
        ~CacheClient();

        CacheClient(const CacheClient &) = delete;
        CacheClient &operator=(const CacheClient &) = delete;

        // Archive ids present in a cache index (5 = maps).
        std::vector<int> archiveIds(int indexId) const;

        // The map square an index-5 archive id addresses: x is the low 7 bits,
        // y is the rest. Returns false when y falls outside the 256-square grid
        // that the area-graph keys and the artifact reader both address — such
        // an id would alias another square's key downstream, so it must be
        // skipped, not decoded. Every caller that turns an archive id into a
        // square goes through here so the aliasing guard cannot be forgotten in
        // one of them.
        static bool mapSquareOf(int archiveId, int &outSquareX, int &outSquareY);

        // Decode one map square's clip. Returns false if the square is absent
        // from the cache; throws std::runtime_error on a decode error.
        bool mapSquareClip(int squareX, int squareY, SquareClip &outClip) const;

        // Decode one map square's interactable crossings into outCrossings
        // (cleared first). Returns false if the square is absent; throws
        // std::runtime_error on a decode error. An empty present square returns
        // true with outCrossings empty.
        bool crossings(int squareX, int squareY, std::vector<Crossing> &outCrossings) const;

        // Decode one map square's clip AND its crossings from a single cache
        // decode. The producer builds both from one landscape pass, so a caller
        // that wants both (the artifact bake wants both for every square) must
        // use this rather than the two calls above, which each re-run the whole
        // decode and discard half the result. Same contract as those two:
        // returns false if the square is absent, throws on a decode error, and
        // clears outCrossings first.
        bool mapSquareClipAndCrossings(int squareX, int squareY, SquareClip &outClip,
                                       std::vector<Crossing> &outCrossings) const;

    private:
        nxt_cache *handle;
    };
}

#endif  // WORLDWALKER_BUILD_CACHECLIENT_H
