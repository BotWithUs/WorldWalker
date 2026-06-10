#include "data/FreshnessDeriver.h"

#include "build/CacheClient.h"
#include "build/CollisionBuilder.h"
#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/ClipFlags.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ww::data
{
    namespace
    {
        using ww::build::SquareClip;

        // Flat index into a square's clip words, matching the producer's layout:
        // plane-major, then x (west-east), then y (south-north).
        std::size_t clipIndex(int plane, int lx, int ly)
        {
            return static_cast<std::size_t>(plane) * format::kClipSize * format::kClipSize
                 + static_cast<std::size_t>(lx) * format::kClipSize
                 + static_cast<std::size_t>(ly);
        }

        // Pack a tile + plane into a unique key. World coords fit well under 2^15,
        // so the shifted fields never overlap.
        uint64_t originKey(int x, int y, int plane)
        {
            return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 40)
                 | (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 8)
                 | static_cast<uint64_t>(plane & 0xFF);
        }

        std::unordered_set<uint64_t> collectDatasetOrigins(const TransitionModel &datasets)
        {
            std::unordered_set<uint64_t> origins;
            origins.reserve(datasets.transitions.size() * 2 + 1);
            for (const Transition &t : datasets.transitions)
            {
                if (!t.isGlobalOrigin)
                {
                    origins.insert(originKey(t.originX, t.originY, t.originPlane));
                }
            }
            return origins;
        }

        // (x, y, plane) -> the PlaneChange crossing that names the climbable loc
        // at that tile. First crossing at a tile wins (duplicates are rare).
        using PlaneChangeMap = std::unordered_map<uint64_t, const ww::build::Crossing *>;

        PlaneChangeMap collectPlaneChangeCrossings(const std::vector<ww::build::Crossing> &crossings)
        {
            PlaneChangeMap byTile;
            byTile.reserve(crossings.size() * 2 + 1);
            for (const ww::build::Crossing &c : crossings)
            {
                if (c.kind != static_cast<uint8_t>(ww::build::CrossingKind::PlaneChange))
                {
                    continue;
                }
                byTile.emplace(originKey(c.worldX, c.worldY, c.plane), &c);
            }
            return byTile;
        }

        void addCandidate(TransitionModel &outModel, FreshnessReport &report,
                          const std::unordered_set<uint64_t> &datasetOrigins,
                          const PlaneChangeMap &planeChange,
                          int x, int y, int fromPlane, int toPlane)
        {
            ++report.emitted;
            if (datasetOrigins.find(originKey(x, y, fromPlane)) != datasetOrigins.end())
            {
                ++report.droppedDatasetConflict;
                return;
            }
            Transition t;
            t.kind = TransitionKind::Transport;
            t.isGlobalOrigin = false;
            t.originX = x;
            t.originY = y;
            t.originPlane = static_cast<uint8_t>(fromPlane);
            t.destX = x;
            t.destY = y;
            t.destPlane = static_cast<uint8_t>(toPlane);

            // Stamp the climbable loc id + option from the crossing at the origin
            // tile/plane so the executor can interact with it. Without a match the
            // candidate keeps objectId == -1 (cannot be executed).
            const auto it = planeChange.find(originKey(x, y, fromPlane));
            if (it != planeChange.end())
            {
                const ww::build::Crossing &c = *it->second;
                // A loc that names its climb direction must agree with this
                // edge's: a mid-landing's up-only loc stamped on the down edge
                // sends the executor further up instead. climbDir == 0
                // (unknown) keeps the candidate — the bake has no evidence
                // against it.
                const uint8_t needed = (toPlane > fromPlane) ? ww::build::kClimbUp
                                                             : ww::build::kClimbDown;
                if (c.climbDir != 0 && (c.climbDir & needed) == 0)
                {
                    ++report.droppedClimbMismatch;
                    return;
                }
                t.objectId = c.objectId;
                t.shape = c.shape;
                t.rotation = c.rotation;
                t.optionIndex = (c.optionIndex == 0xFF) ? 0u : c.optionIndex;
            }

            outModel.transitions.push_back(std::move(t));
            ++report.kept;
        }

        // Pair every column in this square that carries CLIP_PLANE_CHANGE on both
        // lowerPlane and lowerPlane + 1, emitting an up edge and a down edge.
        void scanPlanePair(const SquareClip &sq, int baseX, int baseY, int lowerPlane,
                           const std::unordered_set<uint64_t> &datasetOrigins,
                           const PlaneChangeMap &planeChange,
                           TransitionModel &outModel, FreshnessReport &report)
        {
            const uint8_t pairMask =
                static_cast<uint8_t>((1u << lowerPlane) | (1u << (lowerPlane + 1)));
            if ((sq.planeMask & pairMask) != pairMask)
            {
                return;
            }
            const uint32_t mask = static_cast<uint32_t>(format::CLIP_PLANE_CHANGE);
            for (int lx = 0; lx < format::kClipSize; ++lx)
            {
                for (int ly = 0; ly < format::kClipSize; ++ly)
                {
                    const uint32_t low = sq.words[clipIndex(lowerPlane, lx, ly)];
                    const uint32_t high = sq.words[clipIndex(lowerPlane + 1, lx, ly)];
                    if ((low & mask) == 0u || (high & mask) == 0u)
                    {
                        continue;
                    }
                    ++report.pairsFound;
                    const int wx = baseX + lx;
                    const int wy = baseY + ly;
                    addCandidate(outModel, report, datasetOrigins, planeChange,
                                 wx, wy, lowerPlane, lowerPlane + 1);
                    addCandidate(outModel, report, datasetOrigins, planeChange,
                                 wx, wy, lowerPlane + 1, lowerPlane);
                }
            }
        }

        void scanSquare(const SquareClip &sq, const std::unordered_set<uint64_t> &datasetOrigins,
                        const PlaneChangeMap &planeChange,
                        TransitionModel &outModel, FreshnessReport &report)
        {
            if (sq.words.size() < format::kClipWordsPerSquare)
            {
                return;
            }
            const int baseX = sq.squareX * format::kClipSize;
            const int baseY = sq.squareY * format::kClipSize;
            for (int p = 0; p + 1 < format::kClipPlanes; ++p)
            {
                scanPlanePair(sq, baseX, baseY, p, datasetOrigins, planeChange, outModel, report);
            }
        }
    }

    TransitionModel deriveVerticalTransitions(const ww::build::CollisionModel &collision,
                                              const TransitionModel &datasets,
                                              const std::vector<ww::build::Crossing> &crossings,
                                              FreshnessReport *outReport)
    {
        TransitionModel result;
        FreshnessReport report;
        const std::unordered_set<uint64_t> datasetOrigins = collectDatasetOrigins(datasets);
        const PlaneChangeMap planeChange = collectPlaneChangeCrossings(crossings);
        for (const SquareClip &sq : collision.squares)
        {
            scanSquare(sq, datasetOrigins, planeChange, result, report);
        }
        if (outReport != nullptr)
        {
            *outReport = report;
        }
        return result;
    }
}
