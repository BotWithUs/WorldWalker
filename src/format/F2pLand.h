#ifndef WORLDWALKER_FORMAT_F2PLAND_H
#define WORLDWALKER_FORMAT_F2PLAND_H

#include "format/Artifact.h"
#include "format/MoveCategory.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ww::format
{
    // What a box of tiles is to a free-to-play account.
    enum class LandClass : uint8_t
    {
        Members    = 0,  // no tile in it is free-to-play land
        FreeToPlay = 1,  // every tile in it is
        Mixed      = 2,  // some are, some are not (or the index cannot tell)
    };

    // Free-to-play land as the F2pZones section describes it: the union of the
    // zone boxes minus the union of the hole boxes. Everything else, including
    // every tile outside the indexed range, is members land. An allowlist, so a
    // tile nobody curated counts as members: a free player is kept out of land
    // nobody vouched for, and a members account never consults this at all.
    //
    // Tile queries are answered per map square first: a square wholly inside one
    // zone and touching no hole, or touching no zone, is answered by one table
    // lookup; only a square some box edge crosses tests the boxes. Immutable
    // after construction, so it is safe to share across query threads.
    class F2pLand
    {
    public:
        F2pLand() = default;
        F2pLand(std::span<const F2pBoxRecord> zones, std::span<const F2pBoxRecord> holes);

        bool isFreeToPlay(int32_t x, int32_t y, int32_t plane) const;

        // The class of the inclusive box [minX..maxX] x [minY..maxY] on one
        // plane. Conservative: Mixed whenever the per-square index cannot prove
        // either answer, so FreeToPlay and Members are always exact.
        LandClass classify(int32_t minX, int32_t minY, int32_t maxX, int32_t maxY,
                           int32_t plane) const;

        std::span<const F2pBoxRecord> zones() const
        {
            return {zoneList.data(), zoneList.size()};
        }

        std::span<const F2pBoxRecord> holes() const
        {
            return {holeList.data(), holeList.size()};
        }

    private:
        static constexpr int32_t kSquaresPerAxis = 256;

        static std::size_t slotOf(int32_t squareX, int32_t squareY, int32_t plane);
        bool isFreeToPlayByBoxes(int32_t x, int32_t y, int32_t plane) const;
        LandClass classifySquare(int32_t squareX, int32_t squareY, int32_t plane) const;
        LandClass squareClass(int32_t squareX, int32_t squareY, int32_t plane) const;

        std::vector<F2pBoxRecord> zoneList;
        std::vector<F2pBoxRecord> holeList;
        // LandClass per (plane, squareY, squareX); empty when there are no zones.
        std::vector<uint8_t> squareTable;
    };

    // The networks a free-to-play account cannot use anywhere: charter ships,
    // gnome gliders, fairy rings, spirit trees and magic carpets are all
    // members-only in RS3, at every station, whatever the land under them.
    bool isMembersNetwork(MoveCategory category);

    // Whether a free-to-play account must not take `tx`. In order:
    //   - `override` (-1 none, 0 free-to-play, 1 members), a dataset row's
    //     `"members"` flag, decides outright;
    //   - a transition of a members network (isMembersNetwork) is members;
    //   - with `land` (null when the artifact has no F2pZones section): a
    //     local transition whose origin is not free-to-play land, or any
    //     transition whose destination is not, is members.
    // Anything else is free to play.
    bool isMembersOnly(const TransitionRecord &tx, MoveCategory category, int8_t override,
                       const F2pLand *land);
}

#endif  // WORLDWALKER_FORMAT_F2PLAND_H
