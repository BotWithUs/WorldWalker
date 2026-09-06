#include "cli/HarnessPicks.h"

#include "data/Transitions.h"
#include "runtime/TileScan.h"

#include <algorithm>
#include <cstdlib>

namespace ww::cli
{
    bool acceptAnyTransition(const format::TransitionRecord &tx)
    {
        static_cast<void>(tx);
        return true;
    }

    bool acceptChainedUngated(const format::TransitionRecord &tx)
    {
        return tx.chainCount != 0u && tx.requirementCount == 0u;
    }

    bool pickCrossAreaPair(const format::ArtifactReader &reader, runtime::WorldView &view,
                           TransitionFilter filter, CrossAreaPick &outPick)
    {
        const auto nodes = reader.areaNodes();
        const auto edges = reader.areaEdges();
        const auto txs   = reader.transitions();
        for (std::size_t i = 0; i < edges.size(); ++i)
        {
            const format::AreaEdgeRecord &edge = edges[i];
            if (static_cast<std::size_t>(edge.fromArea) >= nodes.size()
                || static_cast<std::size_t>(edge.toArea) >= nodes.size()
                || edge.transitionIndex >= txs.size())
            {
                continue;
            }
            const format::TransitionRecord &tx = txs[edge.transitionIndex];
            if (!filter(tx))
            {
                continue;
            }
            // Resolve the interact tile through the production scan
            // PathAssembler::resolveInteractTile uses, over the same shared
            // radius, so the harness starts exactly where the transition step
            // will be emitted. A miss here is also the traversability test: an
            // edge whose fromArea attribution is too loose for tile refinement
            // has no such tile.
            const std::int32_t plane = static_cast<std::int32_t>(tx.originPlane);
            const auto standableInArea = [&](std::int32_t x, std::int32_t y)
            {
                return view.isStandable(x, y, plane) && view.areaAt(x, y, plane) == edge.fromArea;
            };
            std::int32_t startX = 0;
            std::int32_t startY = 0;
            if (!runtime::findNearestTile(tx.originX, tx.originY,
                                          data::kTransitionApproachRadius, true, standableInArea,
                                          tx.originX, tx.originY, startX, startY))
            {
                continue;
            }
            outPick.start      = {startX, startY};
            outPick.startPlane = plane;
            outPick.goal       = {tx.destX, tx.destY};
            outPick.goalPlane  = static_cast<std::int32_t>(tx.destPlane);
            outPick.edgeIndex  = i;
            return true;
        }
        return false;
    }

    runtime::TilePoint farthestInArea(runtime::WorldView &view, std::int32_t cx, std::int32_t cy,
                                      std::int32_t plane, std::int32_t area, std::int32_t radius)
    {
        runtime::TilePoint best{cx, cy};
        std::int32_t bestDist = 0;
        for (std::int32_t dx = -radius; dx <= radius; ++dx)
        {
            for (std::int32_t dy = -radius; dy <= radius; ++dy)
            {
                const std::int32_t x = cx + dx;
                const std::int32_t y = cy + dy;
                const std::int32_t dist = std::max(std::abs(dx), std::abs(dy));
                if (dist > bestDist && view.isStandable(x, y, plane)
                    && view.areaAt(x, y, plane) == area)
                {
                    bestDist = dist;
                    best = {x, y};
                }
            }
        }
        return best;
    }
}
