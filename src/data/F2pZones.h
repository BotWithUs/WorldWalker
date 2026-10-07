#ifndef WORLDWALKER_DATA_F2PZONES_H
#define WORLDWALKER_DATA_F2PZONES_H

#include <cstdint>
#include <string>
#include <vector>

namespace ww::data
{
    // One box of datasets/f2p_zones.json: inclusive tile bounds on planes
    // [planeMin, planeMax]. `source` is the citation the box was drawn from (a
    // wiki page); the loader refuses a box without one, because a box nobody can
    // check is a box nobody can fix. Neither `name` nor `source` is baked.
    struct F2pBox
    {
        std::string name;
        std::string source;
        int32_t minX{};
        int32_t minY{};
        int32_t maxX{};
        int32_t maxY{};
        uint8_t planeMin{};
        uint8_t planeMax{};
    };

    // Free-to-play land: the union of `zones` minus the union of `holes`
    // (members-only pockets inside them, such as the Digsite). Every tile no
    // zone covers is members land. Baked as the F2pZones section.
    struct F2pZonesModel
    {
        std::vector<F2pBox> zones;
        std::vector<F2pBox> holes;
    };
}

#endif  // WORLDWALKER_DATA_F2PZONES_H
