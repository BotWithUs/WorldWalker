#include "data/TeleportZones.h"

namespace ww::data
{
    namespace
    {
        // Surface Wilderness: all planes (it has multi-storey structures). Level
        // 1 at y 3520, rising 1 per 8 tiles north to ~level 56 at the far north.
        // Every surface box shares this gradient, so splitting the area into
        // boxes moves no tile's level.
        WildernessRegion surfaceBox(int32_t minX, int32_t minY, int32_t maxX, int32_t maxY)
        {
            WildernessRegion box{};
            box.minX = minX;
            box.minY = minY;
            box.maxX = maxX;
            box.maxY = maxY;
            box.baseY = 3520;
            box.baseLevel = 1;
            box.stepY = 8;
            box.planeMin = 0;
            box.planeMax = 3;
            return box;
        }

        // Underground Wilderness: the surface band shifted by +6400, plane 0,
        // same gradient.
        WildernessRegion undergroundBox(int32_t minX, int32_t minY, int32_t maxX, int32_t maxY)
        {
            WildernessRegion box{};
            box.minX = minX;
            box.minY = minY;
            box.maxX = maxX;
            box.maxY = maxY;
            box.baseY = 9920;
            box.baseLevel = 1;
            box.stepY = 8;
            box.planeMin = 0;
            box.planeMax = 0;
            return box;
        }
    }

    TeleportZonesModel buildTeleportZones()
    {
        TeleportZonesModel model;
        model.defaultWildernessCutoff = 20;

        // The planner keeps a walk whose start and goal are outside these boxes
        // out of them entirely, so they have to be the Wilderness and not a
        // rectangle around it. Walkable ground a player reaches from the
        // mainland without crossing the wall is not Wilderness; the extents
        // below come from the bake's walk-connected areas.
        //
        // South edge: y 3521, the wall's own row (the ditch locs sit on 3521,
        // the far side lands on 3523). Row 3520 is the mainland side of the
        // wall, walkable from Edgeville.
        //
        // Fort Forinthry: the 2023 fort took the south-east corner out of the
        // Wilderness. The mainland area walks from Varrock into x >= 3276..3303
        // up to y 3575, plus a north-east spur at x >= 3359 up to y 3588, while
        // the Wilderness area wraps it at x <= 3281 to the west and x <= 3358
        // across the north. The fort is carved out as three rectangles that
        // hold no Wilderness tile: x 3282..3391 y 3521..3534, x 3278..3391 y
        // 3535..3569, x 3359..3391 y 3570..3588. The few fort tiles left
        // outside them, along the jagged north wall, stay in the Wilderness,
        // which only keeps a walk off them.
        model.wilderness.push_back(surfaceBox(2944, 3521, 3281, 3534));
        model.wilderness.push_back(surfaceBox(2944, 3535, 3277, 3569));
        model.wilderness.push_back(surfaceBox(2944, 3570, 3358, 3588));
        model.wilderness.push_back(surfaceBox(3392, 3521, 3392, 3588));
        model.wilderness.push_back(surfaceBox(2944, 3589, 3392, 3967));

        // Underground: the Wilderness dungeons all sit at x <= 3134: the
        // Edgeville dungeon's north half, the Forinthry dungeon, the Lava Maze
        // dungeon (KBD lever 3069,10250) and the deep dungeon down to the
        // agility pit (3005,10363). East of them, War's Retreat (3294,10127) is
        // a safe boss hub that the old x..3392 box counted as level 27. In the
        // Edgeville dungeon the Wilderness starts at the gate (locs 29319 and
        // 29320, 3131..3132, 9917 -> 9918), so the box starts on 9918.
        model.wilderness.push_back(undergroundBox(2944, 9918, 3135, 10367));

        // No-teleport zones: curated boxes where teleporting is blocked outright.
        // Left empty until exact coordinates are verified against an authoritative
        // source (ADR 0009 follow-up); the format and runtime gate ship now so a
        // zone drops in with one push_back, no reformat.

        return model;
    }
}
