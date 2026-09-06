#include "build/CacheClient.h"

#include "format/Artifact.h"

#include <stdexcept>
#include <string>

namespace ww::build
{
    namespace
    {
        // Frees an NXTCache C-ABI buffer on scope exit, so a bad_alloc thrown
        // while copying into a vector cannot leak the producer-owned block.
        struct NxtBufferGuard
        {
            void *ptr;

            ~NxtBufferGuard()
            {
                nxt_free(ptr);
            }
        };

        // Copy the producer's clip words into outClip and check the word count.
        // The WW collision format requires every square to ship all 4 planes
        // worth of clip words (kClipWordsPerSquare = kClipPlanes * 64 * 64).
        // A producer that ever returned a popcount(planeMask)-sized buffer (the
        // old prior-nav-stack layout) would silently mis-size the artifact's
        // per-square blob and the runtime would read garbage for planes 1-3.
        // Fail loud at the build boundary so the divergence shows up here,
        // not as wrong paths at runtime.
        void takeClip(const uint32_t *words, size_t count, uint8_t planeMask,
                      int squareX, int squareY, SquareClip &outClip)
        {
            outClip.squareX = squareX;
            outClip.squareY = squareY;
            outClip.planeMask = planeMask;
            outClip.words.assign(words, words + count);
            if (outClip.words.size() != ww::format::kClipWordsPerSquare)
            {
                throw std::runtime_error("CacheClient: map square clip returned "
                                         + std::to_string(outClip.words.size())
                                         + " words, expected "
                                         + std::to_string(ww::format::kClipWordsPerSquare));
            }
        }

        // Translate the C ABI's crossing records into the WorldWalker mirror.
        void takeCrossings(const nxt_crossing *records, size_t count,
                           std::vector<Crossing> &outCrossings)
        {
            outCrossings.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                const nxt_crossing &r = records[i];
                Crossing c;
                c.objectId    = r.object_id;
                c.worldX      = r.world_x;
                c.worldY      = r.world_y;
                c.plane       = r.plane;
                c.shape       = r.shape;
                c.rotation    = r.rotation;
                c.kind        = r.kind;
                c.sizeX       = r.size_x;
                c.sizeY       = r.size_y;
                c.optionIndex = r.option_index;
                c.climbDir    = r.climb_dir;
                outCrossings.push_back(c);
            }
        }
    }

    bool CacheClient::mapSquareOf(int archiveId, int &outSquareX, int &outSquareY)
    {
        const int squareX = archiveId & 0x7F;
        const int squareY = archiveId >> 7;
        if (squareY > 255)
        {
            return false;
        }
        outSquareX = squareX;
        outSquareY = squareY;
        return true;
    }

    CacheClient::CacheClient(const std::string &cachePath, bool enableLiveFallback)
        : handle(nullptr)
    {
        handle = nxt_cache_open_local(cachePath.c_str());
        if (handle == nullptr)
        {
            throw std::runtime_error(std::string("nxt_cache_open_local failed: ") + nxt_last_error());
        }
        if (enableLiveFallback && nxt_cache_enable_live_fallback(handle) != NXT_OK)
        {
            std::string message = nxt_last_error();
            nxt_cache_close(handle);
            handle = nullptr;
            throw std::runtime_error("nxt_cache_enable_live_fallback failed: " + message);
        }
    }

    CacheClient::~CacheClient()
    {
        if (handle != nullptr)
        {
            nxt_cache_close(handle);
        }
    }

    std::vector<int> CacheClient::archiveIds(int indexId) const
    {
        int *ids = nullptr;
        size_t count = 0;
        if (nxt_list_archive_ids(handle, indexId, &ids, &count) != NXT_OK)
        {
            throw std::runtime_error(std::string("nxt_list_archive_ids failed: ") + nxt_last_error());
        }
        const NxtBufferGuard guard{ids};
        return std::vector<int>(ids, ids + count);
    }

    bool CacheClient::mapSquareClip(int squareX, int squareY, SquareClip &outClip) const
    {
        uint32_t *words = nullptr;
        size_t count = 0;
        uint8_t planeMask = 0;
        nxt_result rc = nxt_get_mapsquare_clip(handle, squareX, squareY, &words, &count, &planeMask);
        if (rc == NXT_ERR_NOT_FOUND)
        {
            return false;
        }
        if (rc != NXT_OK)
        {
            throw std::runtime_error(std::string("nxt_get_mapsquare_clip failed: ") + nxt_last_error());
        }
        const NxtBufferGuard guard{words};
        takeClip(words, count, planeMask, squareX, squareY, outClip);
        return true;
    }

    bool CacheClient::crossings(int squareX, int squareY,
                                std::vector<Crossing> &outCrossings) const
    {
        outCrossings.clear();
        nxt_crossing *records = nullptr;
        size_t count = 0;
        nxt_result rc = nxt_get_mapsquare_crossings(handle, squareX, squareY, &records, &count);
        if (rc == NXT_ERR_NOT_FOUND)
        {
            return false;
        }
        if (rc != NXT_OK)
        {
            throw std::runtime_error(std::string("nxt_get_mapsquare_crossings failed: ")
                                     + nxt_last_error());
        }
        const NxtBufferGuard guard{records};
        takeCrossings(records, count, outCrossings);
        return true;
    }

    bool CacheClient::mapSquareClipAndCrossings(int squareX, int squareY, SquareClip &outClip,
                                                std::vector<Crossing> &outCrossings) const
    {
        outCrossings.clear();
        uint32_t *words = nullptr;
        size_t wordCount = 0;
        uint8_t planeMask = 0;
        nxt_crossing *records = nullptr;
        size_t crossingCount = 0;
        nxt_result rc = nxt_get_mapsquare_clip_and_crossings(handle, squareX, squareY,
                                                             &words, &wordCount, &planeMask,
                                                             &records, &crossingCount);
        if (rc == NXT_ERR_NOT_FOUND)
        {
            return false;
        }
        if (rc != NXT_OK)
        {
            throw std::runtime_error(std::string("nxt_get_mapsquare_clip_and_crossings failed: ")
                                     + nxt_last_error());
        }
        const NxtBufferGuard clipGuard{words};
        const NxtBufferGuard crossingGuard{records};
        takeClip(words, wordCount, planeMask, squareX, squareY, outClip);
        takeCrossings(records, crossingCount, outCrossings);
        return true;
    }
}
