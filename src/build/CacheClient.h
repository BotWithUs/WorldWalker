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

        // Decode one map square's clip. Returns false if the square is absent
        // from the cache; throws std::runtime_error on a decode error.
        bool mapSquareClip(int squareX, int squareY, SquareClip &outClip) const;

    private:
        nxt_cache *handle;
    };
}

#endif  // WORLDWALKER_BUILD_CACHECLIENT_H
