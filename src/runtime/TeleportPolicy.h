#ifndef WORLDWALKER_RUNTIME_TELEPORTPOLICY_H
#define WORLDWALKER_RUNTIME_TELEPORTPOLICY_H

#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "runtime/CapabilitySnapshot.h"

#include <cstdint>
#include <span>

namespace ww::runtime
{
    // Whether a Global-origin transition (spell, lodestone, item-teleport) may be
    // initiated from the tile (x, y, plane), evaluated against the artifact's
    // baked TeleportAllowed section:
    //   * Any NoTeleZone hit is an unconditional block (instances, safe areas).
    //   * Each WildernessRegion hit computes
    //       level = ((y - baseY) / stepY) + baseLevel
    //     and blocks when level exceeds the wildernessCutoff (the standard
    //     spellbook cap; a per-teleport requirement slot is reserved for
    //     tighter caps but not yet wired).
    // No section, no boxes, or no hits → teleport is allowed. The span overload
    // keeps the rule pure so unit-style checks can drive it with synthetic
    // boxes; the reader overload is the production path.
    inline bool isTeleportAllowed(std::span<const format::NoTeleZone> noTele,
                                  std::span<const format::WildernessRegion> wild,
                                  int32_t wildernessCutoff,
                                  int32_t x, int32_t y, int32_t plane)
    {
        if (plane < 0 || plane > 3)
        {
            return false;
        }
        for (const format::NoTeleZone &z : noTele)
        {
            if (plane < z.planeMin || plane > z.planeMax)
            {
                continue;
            }
            if (x >= z.minX && x <= z.maxX && y >= z.minY && y <= z.maxY)
            {
                return false;
            }
        }
        for (const format::WildernessRegion &w : wild)
        {
            if (plane < w.planeMin || plane > w.planeMax)
            {
                continue;
            }
            if (x < w.minX || x > w.maxX || y < w.minY || y > w.maxY)
            {
                continue;
            }
            const int32_t stepY = w.stepY > 0 ? w.stepY : 1;
            const int32_t level = ((y - w.baseY) / stepY) + w.baseLevel;
            if (level > wildernessCutoff)
            {
                return false;
            }
        }
        return true;
    }

    inline bool isTeleportAllowed(const format::ArtifactReader &reader,
                                  int32_t x, int32_t y, int32_t plane)
    {
        return isTeleportAllowed(reader.noTeleZones(), reader.wildernessRegions(),
                                 static_cast<int32_t>(reader.wildernessCutoff()),
                                 x, y, plane);
    }

    // COMBATV2_PLAYER_IS_IN_COMBAT (varp 689 bit 4): 1 while the player is in
    // combat. The game refuses lodestones and spell and item teleports then,
    // so a plan that casts one stalls on the spot. V1 navigation resolved with
    // teleports disabled while this read 1 and re-planned when it cleared; the
    // planner does the same by seeding no global teleport, and the executor
    // re-plans on the in-combat -> out-of-combat flip exactly as it does on
    // leaving the Wilderness.
    //
    // No requirement names this varbit, so the executor asks the host for it
    // explicitly (Executor::planVarbitIds) on every plan.
    inline constexpr int32_t kInCombatVarbitId = 1899;

    // True when the snapshot says the player is in combat. A null snapshot is
    // the unfiltered no-gate query and reads as out of combat, as does a
    // snapshot the host never filled this varbit into.
    inline bool isInCombat(const CapabilitySnapshot *snapshot)
    {
        return snapshot != nullptr && snapshot->varbit(kInCombatVarbitId) == 1;
    }

    // Whether a global teleport may be cast right now from (x, y, plane): the
    // tile allows it and the player is not in combat. The one rule the planner
    // seeds by and the executor anchors its re-plan flip on.
    inline bool isTeleportAllowed(const format::ArtifactReader &reader,
                                  const CapabilitySnapshot *snapshot,
                                  int32_t x, int32_t y, int32_t plane)
    {
        return !isInCombat(snapshot) && isTeleportAllowed(reader, x, y, plane);
    }
}

#endif  // WORLDWALKER_RUNTIME_TELEPORTPOLICY_H
