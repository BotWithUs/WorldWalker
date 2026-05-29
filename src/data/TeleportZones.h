#ifndef WORLDWALKER_DATA_TELEPORTZONES_H
#define WORLDWALKER_DATA_TELEPORTZONES_H

#include <cstdint>
#include <vector>

namespace ww::data
{
    // An axis-aligned tile box (inclusive bounds) on planes [planeMin, planeMax]
    // where the wilderness level rises with y:
    //   level = ((y - baseY) / stepY) + baseLevel
    // A Global teleport is blocked where that level exceeds the teleport's
    // wilderness cutoff.
    struct WildernessRegion
    {
        int32_t minX{};
        int32_t minY{};
        int32_t maxX{};
        int32_t maxY{};
        int32_t baseY{};
        int32_t baseLevel{};
        int32_t stepY{};
        uint8_t planeMin{};
        uint8_t planeMax{};
    };

    // A curated tile box (inclusive bounds) on planes [planeMin, planeMax] where
    // teleporting is unconditionally blocked, independent of wilderness level.
    struct NoTeleZone
    {
        int32_t minX{};
        int32_t minY{};
        int32_t maxX{};
        int32_t maxY{};
        uint8_t planeMin{};
        uint8_t planeMax{};
    };

    // The Teleport-allowed model: a Tile is teleport-allowed unless it lies in a
    // wilderness region above the cutoff, or inside any no-teleport zone.
    struct TeleportZonesModel
    {
        uint32_t defaultWildernessCutoff{20};
        std::vector<WildernessRegion> wilderness;
        std::vector<NoTeleZone> noTele;
    };

    // Build the curated teleport-allowed model. Pure constants — no cache or
    // dataset input. The wilderness regions are load-bearing (they gate the
    // "walk out of the Wilderness, then teleport" re-plan in the Executor); the
    // no-teleport zone list is a tuning list (ADR 0009 follow-up).
    TeleportZonesModel buildTeleportZones();
}

#endif  // WORLDWALKER_DATA_TELEPORTZONES_H
