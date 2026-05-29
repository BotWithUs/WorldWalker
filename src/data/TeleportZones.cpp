#include "data/TeleportZones.h"

namespace ww::data
{
    TeleportZonesModel buildTeleportZones()
    {
        TeleportZonesModel model;
        model.defaultWildernessCutoff = 20;

        // Surface Wilderness: x 2944..3392, y 3520..3967, all planes (it has
        // multi-storey structures). Level 1 at the southern edge, rising 1 per 8
        // tiles north to ~level 56 at the far north.
        WildernessRegion surface{};
        surface.minX = 2944;
        surface.minY = 3520;
        surface.maxX = 3392;
        surface.maxY = 3967;
        surface.baseY = 3520;
        surface.baseLevel = 1;
        surface.stepY = 8;
        surface.planeMin = 0;
        surface.planeMax = 3;
        model.wilderness.push_back(surface);

        // Underground Wilderness (dungeons below the surface band): same x, the y
        // band shifted by +6400, plane 0. Same level gradient.
        WildernessRegion underground{};
        underground.minX = 2944;
        underground.minY = 9920;
        underground.maxX = 3392;
        underground.maxY = 10367;
        underground.baseY = 9920;
        underground.baseLevel = 1;
        underground.stepY = 8;
        underground.planeMin = 0;
        underground.planeMax = 0;
        model.wilderness.push_back(underground);

        // No-teleport zones: curated boxes where teleporting is blocked outright.
        // Left empty until exact coordinates are verified against an authoritative
        // source (ADR 0009 follow-up); the format and runtime gate ship now so a
        // zone drops in with one push_back, no reformat.

        return model;
    }
}
