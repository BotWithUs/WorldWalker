#ifndef WORLDWALKER_BUILD_AREAGRAPH_H
#define WORLDWALKER_BUILD_AREAGRAPH_H

#include "build/CollisionBuilder.h"
#include "build/CollisionLookup.h"
#include "data/Transitions.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ww::build
{
    // One flood-filled area: a maximal set of cardinally walk-connected tiles on
    // a single plane. The area id is this node's index in AreaGraphModel::nodes.
    struct AreaNode
    {
        uint8_t plane{};
        uint32_t tileCount{};
        int32_t centroidX{};
        int32_t centroidY{};
        int32_t minX{};
        int32_t minY{};
        int32_t maxX{};
        int32_t maxY{};
    };

    // A coarse graph edge: traversing transition `transitionIndex` moves from
    // `fromArea` to `toArea`. transitionIndex indexes the finalized transition
    // list the artifact also serializes, so the planner can recover the full
    // transition (object, chain, requirements) for the refine/execute step.
    struct AreaEdge
    {
        int32_t fromArea{};
        int32_t toArea{};
        uint32_t transitionIndex{};
        float cost{};
    };

    // The area-id grid for one (square, plane): kClipSize*kClipSize int32 ids in
    // x-major then y order, -1 for blocked / unreachable tiles.
    struct AreaGrid
    {
        int squareX{};
        int squareY{};
        int plane{};
        std::vector<int32_t> ids;
    };

    struct AreaGraphModel
    {
        std::vector<AreaNode> nodes;   // indexed by area id
        std::vector<AreaEdge> edges;   // sorted by (fromArea, toArea)
        std::vector<AreaGrid> grids;   // sorted by (squareY, squareX, plane)
    };

    // Build-log accounting for buildAreaGraph.
    struct AreaGraphReport
    {
        std::size_t areaCount{};
        std::size_t edgeCount{};
        std::size_t gridCount{};
        std::size_t largestArea{};          // tiles in the biggest area
        std::size_t resolvedTransitions{};  // transitions that produced >=1 edge
        std::size_t unresolvedOrigin{};     // local transitions whose origin touched no area
        std::size_t unresolvedDest{};       // local transitions whose dest tile is in no area
        std::size_t intraAreaSkipped{};     // edges dropped because from == to (walk suffices)
        std::size_t globalSkipped{};        // global-origin transitions (seeded at the frontier)
    };

    // Flood-fill the collision grid into per-plane areas and derive coarse edges
    // from `transitions` (which must be the finalized list serialized into the
    // artifact, so edge transitionIndex values line up). *outReport (nullable)
    // receives the counts.
    AreaGraphModel buildAreaGraph(const CollisionModel &collision, const CollisionLookup &lookup,
                                  const ww::data::TransitionModel &transitions,
                                  AreaGraphReport *outReport);
}

#endif  // WORLDWALKER_BUILD_AREAGRAPH_H
