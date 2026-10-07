#ifndef WORLDWALKER_FORMAT_MOVECATEGORY_H
#define WORLDWALKER_FORMAT_MOVECATEGORY_H

#include "format/Artifact.h"

#include <cstdint>
#include <span>

namespace ww::format
{
    // The kind of movement a transition is, as a host that lets the player
    // switch some of them off sees it. The values are the bit numbers of the
    // WW_MOVE_* constants in c_api/worldwalker_c.h (asserted there); a mask
    // with bit (1u << category) set disables the category.
    enum class MoveCategory : uint8_t
    {
        Doors        = 0,   // doors and gates you open or pass through
        Shortcuts    = 1,   // agility shortcuts and obstacles
        Plane        = 2,   // stairs, ladders, trapdoors: up, down, underground
        ClimbOvers   = 3,   // stiles and walls you climb or step over
        Transports   = 4,   // boats, carts, portals, other loc / NPC links
        Teleports    = 5,   // spell and item teleports
        Lodestones   = 6,
        FairyRings   = 7,
        SpiritTrees  = 8,
        Gliders      = 9,
        Charters     = 10,  // charter ships
        MagicCarpets = 11,
        OtherChains  = 12,  // any other teleport_chains row
    };

    inline constexpr uint32_t kMoveCategoryCount = 13;

    // Not a category: a reserved bit of the same disabled-moves mask asking the
    // planner to plan as a free-to-play account (WW_RESTRICT_FREE_TO_PLAY).
    // The top bit, far above any category, so a reader that predates it shifts
    // by categories 0..12 only and never sees it.
    inline constexpr uint32_t kRestrictFreeToPlay = 1u << 31;
    static_assert(kMoveCategoryCount < 31u, "categories must stay clear of kRestrictFreeToPlay");

    // Short fixed name, for reports.
    const char *moveCategoryName(MoveCategory category);

    // The category of `tx`, from what the artifact already holds: its kind,
    // the interfaces its chain waits on (each network's travel map is its own
    // interface), where it goes, and for a loc a curated transport link
    // clicks, that loc's id (MoveCategoryLocs.inc, generated from the loc
    // definitions). `chain` is the artifact's whole chain-step pool.
    MoveCategory classifyMove(const TransitionRecord &tx, std::span<const ChainStepRecord> chain);
}

#endif  // WORLDWALKER_FORMAT_MOVECATEGORY_H
