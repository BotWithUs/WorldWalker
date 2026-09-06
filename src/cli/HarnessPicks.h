#ifndef WORLDWALKER_CLI_HARNESSPICKS_H
#define WORLDWALKER_CLI_HARNESSPICKS_H

#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "runtime/TileSearch.h"
#include "runtime/WorldView.h"

#include <cstddef>
#include <cstdint>

// Case discovery shared by the wwcli harnesses. The artifact under test is cut
// from whichever cache snapshot the developer ran wwbuild against, so nothing
// can be pinned by coordinate; every suite instead finds a representative case
// in the artifact's own indices. These are those finders, in one place so the
// planner harness and the executor harness pose the same query.
namespace ww::cli
{
    // A cross-area query the harness can drive end to end: stand on the
    // transition's interact tile, walk through the transition, land on its
    // destination.
    struct CrossAreaPick
    {
        runtime::TilePoint start{};
        runtime::TilePoint goal{};
        std::int32_t       startPlane{};
        std::int32_t       goalPlane{};
        std::size_t        edgeIndex{};
    };

    // Filter over the TransitionRecord behind a candidate AreaEdge. Stateless by
    // design — a plain function pointer, so the picker stays one function and
    // the callers differ only in what they will accept.
    using TransitionFilter = bool (*)(const format::TransitionRecord &tx);

    // Accepts every transition: "any traversable cross-area edge".
    bool acceptAnyTransition(const format::TransitionRecord &tx);

    // Accepts only transitions carrying a non-empty embedded chain and no
    // requirements — what the executor harness needs so the chain Click/Wait
    // dispatch is exercised and an empty capability snapshot cannot filter the
    // picked edge out of the route.
    bool acceptChainedUngated(const format::TransitionRecord &tx);

    // First AreaEdge accepted by `filter` whose transition is traversable — a
    // standable interact-tile in the declared fromArea exists within radius 2 of
    // the origin. The pick's start is that interact-tile, so the harness query
    // targets the transition itself rather than a long intra-area trek through
    // whatever the artifact calls fromArea; the goal is the destination tile.
    // Returns false when no edge qualifies.
    bool pickCrossAreaPair(const format::ArtifactReader &reader, runtime::WorldView &view,
                           TransitionFilter filter, CrossAreaPick &outPick);

    // Standable tile in `area` farthest from (cx, cy) within `radius`, or
    // (cx, cy) itself when the centre is the only one. Gives the tile searches a
    // genuine multi-step route rather than a neighbour hop.
    runtime::TilePoint farthestInArea(runtime::WorldView &view, std::int32_t cx, std::int32_t cy,
                                      std::int32_t plane, std::int32_t area, std::int32_t radius);
}

#endif  // WORLDWALKER_CLI_HARNESSPICKS_H
