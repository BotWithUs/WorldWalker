#include "build/CacheClient.h"

#include "format/Artifact.h"

#include <stdexcept>
#include <string>

namespace ww::build
{
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
        std::vector<int> result(ids, ids + count);
        nxt_free(ids);
        return result;
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
        outClip.squareX = squareX;
        outClip.squareY = squareY;
        outClip.planeMask = planeMask;
        outClip.words.assign(words, words + count);
        nxt_free(words);
        // The WW collision format requires every square to ship all 4 planes
        // worth of clip words (kClipWordsPerSquare = kClipPlanes * 64 * 64).
        // A producer that ever returns a popcount(planeMask)-sized buffer (the
        // old prior-nav-stack layout) would silently mis-size the artifact's
        // per-square blob and the runtime would read garbage for planes 1-3.
        // Fail loud at the build boundary so the divergence shows up here,
        // not as wrong paths at runtime.
        if (outClip.words.size() != ww::format::kClipWordsPerSquare)
        {
            throw std::runtime_error("CacheClient: nxt_get_mapsquare_clip returned "
                                     + std::to_string(outClip.words.size())
                                     + " words, expected "
                                     + std::to_string(ww::format::kClipWordsPerSquare));
        }
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
        nxt_free(records);
        return true;
    }
}
