#include "data/FreshnessDeriver.h"

#include "build/CacheClient.h"
#include "build/CollisionBuilder.h"
#include "build/CollisionLookup.h"
#include "data/CrossingStamp.h"
#include "data/DatasetOrigins.h"
#include "data/OpCheck.h"
#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/ClipFlags.h"
#include "format/WallApproach.h"

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

        // Tile+plane key, shared with the door deriver so both suppress
        // against the same set the same way (data/DatasetOrigins.h).
        uint64_t originKey(int x, int y, int plane)
        {
            return datasetOriginKey(static_cast<int32_t>(x), static_cast<int32_t>(y),
                                    static_cast<uint8_t>(plane));
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

        // One derived climb: click the loc at (x, y, fromPlane), land near
        // (destX, destY, toPlane). A column pair lands on its own column; a
        // footprint pair lands on its partner loc's anchor. The builder snaps
        // the landing to the nearest standable tile either way.
        struct VerticalHop
        {
            int x;
            int y;
            int fromPlane;
            int destX;
            int destY;
            int toPlane;
            bool isFootprintPair;
        };

        // True when some tile within kTransitionApproachRadius of (x, y) is
        // standable and reaches (x, y) without crossing a wall: the rule
        // AreaGraph's collectOriginAreas resolves an origin by, so an origin
        // that passes here is one the graph can link.
        bool hasApproach(const ww::build::CollisionLookup &lookup, int x, int y, int plane)
        {
            const auto clipAt = [&lookup](int cx, int cy, int cp)
            {
                return lookup.clipAt(cx, cy, cp);
            };
            constexpr int radius = kTransitionApproachRadius;
            for (int ox = -radius; ox <= radius; ++ox)
            {
                for (int oy = -radius; oy <= radius; ++oy)
                {
                    const int cx = x + ox;
                    const int cy = y + oy;
                    if (lookup.isWalkable(cx, cy, plane)
                        && !format::approachSealed(clipAt, cx, cy, x, y, plane))
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        // A multi-tile spiral stair is blocked across its whole footprint, so
        // its anchor tile (the one the cache names) can sit two tiles from
        // the nearest floor: the lighthouse base (4568, 2x2) has nothing
        // standable within 1 of its anchor. Rather than widen the approach
        // radius for everyone (Transitions.h explains why 1 is the bake's
        // choice), move such an origin to the first footprint tile that does
        // have an approach. The host clicks the loc by id near the origin, and
        // every footprint tile is the loc. The anchor wins whenever it has an
        // approach, so no edge the graph could already link moves. Returns
        // true when the origin moved.
        bool moveOriginOntoApproach(const ww::build::Crossing &c,
                                    const ww::build::CollisionLookup &lookup, Transition &t)
        {
            const int plane = static_cast<int>(t.originPlane);
            if (hasApproach(lookup, t.originX, t.originY, plane))
            {
                return false;
            }
            for (int dx = 0; dx < static_cast<int>(c.sizeX); ++dx)
            {
                for (int dy = 0; dy < static_cast<int>(c.sizeY); ++dy)
                {
                    if (hasApproach(lookup, c.worldX + dx, c.worldY + dy, plane))
                    {
                        t.originX = c.worldX + dx;
                        t.originY = c.worldY + dy;
                        return true;
                    }
                }
            }
            return false;
        }

        // True when a curated transition starts anywhere on c's footprint on
        // `plane`. The anchor-tile check in addCandidate is enough for an edge
        // that stays on its anchor, but a moved origin or a footprint pair is
        // new ground: Lumbridge castle's stairs are curated at 3205,3208 and
        // anchored at 3204,3207, so without this a derived twin (clicking the
        // middle flight's bare `Climb`, which opens the up/down chooser)
        // would compete with the curated rows.
        bool footprintHasDatasetOrigin(const ww::build::Crossing &c, int plane,
                                       const DatasetOrigins &datasetOrigins)
        {
            for (int dx = 0; dx < static_cast<int>(c.sizeX); ++dx)
            {
                for (int dy = 0; dy < static_cast<int>(c.sizeY); ++dy)
                {
                    const uint64_t key = originKey(c.worldX + dx, c.worldY + dy, plane);
                    if (datasetOrigins.find(key) != datasetOrigins.end())
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        // Everything a candidate needs besides its own hop, so the scan
        // helpers do not each thread six arguments through. climbsUp /
        // climbsDown hold the origins the column scan paired, which the
        // footprint pass must leave alone.
        struct DeriveContext
        {
            const DatasetOrigins &datasetOrigins;
            const PlaneChangeMap &planeChange;
            const ww::build::CollisionLookup &lookup;
            TransitionModel &outModel;
            FreshnessReport &report;
            std::unordered_set<uint64_t> climbsUp;
            std::unordered_set<uint64_t> climbsDown;
        };

        // Stamp the climbable loc c onto t and settle its origin. Returns false
        // (counted) when the candidate must be dropped.
        bool stampClimbable(DeriveContext &ctx, const VerticalHop &hop,
                            const ww::build::Crossing &c, Transition &t)
        {
            FreshnessReport &report = ctx.report;
            // A loc that names its climb direction must agree with this
            // edge's: a mid-landing's up-only loc stamped on the down edge
            // sends the executor further up instead. climbDir == 0
            // (unknown) keeps the candidate - the bake has no evidence
            // against it.
            const uint8_t needed = (hop.toPlane > hop.fromPlane) ? ww::build::kClimbUp
                                                                 : ww::build::kClimbDown;
            if (c.climbDir != 0 && (c.climbDir & needed) == 0)
            {
                ++report.droppedClimbMismatch;
                return false;
            }
            // A climbable with no option slot cannot be clicked: drop the
            // candidate rather than bake an edge the executor stalls at.
            if (!stampCrossingLoc(c, t))
            {
                ++report.droppedNoOption;
                return false;
            }
            const bool isMoved = moveOriginOntoApproach(c, ctx.lookup, t);
            if ((isMoved || hop.isFootprintPair)
                && footprintHasDatasetOrigin(c, hop.fromPlane, ctx.datasetOrigins))
            {
                ++report.droppedFootprintDataset;
                return false;
            }
            if (isMoved)
            {
                ++report.originMoved;
            }
            return true;
        }

        void addCandidate(DeriveContext &ctx, const VerticalHop &hop)
        {
            FreshnessReport &report = ctx.report;
            ++report.emitted;
            const uint64_t key = originKey(hop.x, hop.y, hop.fromPlane);
            if (ctx.datasetOrigins.find(key) != ctx.datasetOrigins.end())
            {
                ++report.droppedDatasetConflict;
                return;
            }
            Transition t;
            t.kind = TransitionKind::Transport;
            t.isGlobalOrigin = false;
            t.originX = hop.x;
            t.originY = hop.y;
            t.originPlane = static_cast<uint8_t>(hop.fromPlane);
            t.destX = hop.destX;
            t.destY = hop.destY;
            t.destPlane = static_cast<uint8_t>(hop.toPlane);

            // Stamp the climbable loc id + option from the crossing at the origin
            // tile/plane so the executor can interact with it. Without a match the
            // candidate keeps objectId == -1 (cannot be executed).
            const auto it = ctx.planeChange.find(key);
            if (it != ctx.planeChange.end() && !stampClimbable(ctx, hop, *it->second, t))
            {
                return;
            }

            ctx.outModel.transitions.push_back(std::move(t));
            ++report.kept;
        }

        // Pair every column in this square that carries CLIP_PLANE_CHANGE on both
        // lowerPlane and lowerPlane + 1, emitting an up edge and a down edge.
        void scanPlanePair(const SquareClip &sq, int baseX, int baseY, int lowerPlane,
                           DeriveContext &ctx)
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
                    ++ctx.report.pairsFound;
                    const int wx = baseX + lx;
                    const int wy = baseY + ly;
                    ctx.climbsUp.insert(originKey(wx, wy, lowerPlane));
                    ctx.climbsDown.insert(originKey(wx, wy, lowerPlane + 1));
                    addCandidate(ctx, {wx, wy, lowerPlane, wx, wy, lowerPlane + 1, false});
                    addCandidate(ctx, {wx, wy, lowerPlane + 1, wx, wy, lowerPlane, false});
                }
            }
        }

        void scanSquare(const SquareClip &sq, DeriveContext &ctx)
        {
            if (sq.words.size() < format::kClipWordsPerSquare)
            {
                return;
            }
            const int baseX = sq.squareX * format::kClipSize;
            const int baseY = sq.squareY * format::kClipSize;
            for (int p = 0; p + 1 < format::kClipPlanes; ++p)
            {
                scanPlanePair(sq, baseX, baseY, p, ctx);
            }
        }

        // The widest plane-change loc the footprint pass looks for a partner
        // of: a partner anchored more than this many tiles south-west of a
        // loc cannot overlap it. Spiral stairs are 1x1 to 3x3.
        constexpr int kMaxPartnerSpan = 4;

        bool footprintsOverlap(const ww::build::Crossing &a, const ww::build::Crossing &b)
        {
            return a.worldX < b.worldX + b.sizeX && b.worldX < a.worldX + a.sizeX
                && a.worldY < b.worldY + b.sizeY && b.worldY < a.worldY + a.sizeY;
        }

        // The one plane-change loc on `plane` whose footprint overlaps c's.
        // nullptr when there is none, or more than one: two candidates means
        // the bake cannot tell which stair c meets, and an edge between
        // unrelated stairs is worse than no edge.
        const ww::build::Crossing *findPartner(const ww::build::Crossing &c, int plane,
                                               const PlaneChangeMap &planeChange)
        {
            const ww::build::Crossing *partner = nullptr;
            for (int x = c.worldX - kMaxPartnerSpan + 1; x < c.worldX + c.sizeX; ++x)
            {
                for (int y = c.worldY - kMaxPartnerSpan + 1; y < c.worldY + c.sizeY; ++y)
                {
                    const auto it = planeChange.find(originKey(x, y, plane));
                    if (it == planeChange.end() || !footprintsOverlap(c, *it->second))
                    {
                        continue;
                    }
                    if (partner != nullptr)
                    {
                        return nullptr;
                    }
                    partner = it->second;
                }
            }
            return partner;
        }

        // A spiral stair's floors need not share an anchor: the lighthouse's
        // middle flight (4569, 2x2) is anchored at 2505,3640 on plane 1 and
        // its top (4570, 1x1) at 2506,3641 on plane 2, so no column carries
        // the plane-change flag on both and the column scan never pairs them.
        // Pair such a loc with the one loc on the plane above whose footprint
        // overlaps it, when neither already has a column partner and each is
        // the other's only overlap. Both locs must name their climb: the
        // lower one up, the upper one down. That is what a flight's bottom
        // and top say, and it keeps out the overlaps that are not one flight
        // (a cave rope under an `Ascend`-only stair, a down-only stair under
        // an up-only ladder). The climb lands on the partner's anchor (the
        // builder snaps it to the nearest standable tile).
        void pairByFootprint(const ww::build::Crossing &c, DeriveContext &ctx)
        {
            const int lower = static_cast<int>(c.plane);
            if (lower + 1 >= format::kClipPlanes || (c.climbDir & ww::build::kClimbUp) == 0
                || ctx.climbsUp.count(originKey(c.worldX, c.worldY, lower)) != 0)
            {
                return;
            }
            const ww::build::Crossing *upper = findPartner(c, lower + 1, ctx.planeChange);
            if (upper == nullptr || (upper->climbDir & ww::build::kClimbDown) == 0
                || (upper->worldX == c.worldX && upper->worldY == c.worldY)
                || ctx.climbsDown.count(originKey(upper->worldX, upper->worldY, lower + 1)) != 0
                || findPartner(*upper, lower, ctx.planeChange) != &c)
            {
                return;
            }
            ++ctx.report.footprintPairs;
            const int ux = upper->worldX;
            const int uy = upper->worldY;
            addCandidate(ctx, {c.worldX, c.worldY, lower, ux, uy, lower + 1, true});
            addCandidate(ctx, {ux, uy, lower + 1, c.worldX, c.worldY, lower, true});
        }

        // Walks the crossings in decode order, so the derived edges come out
        // in the same order on every bake. Only the crossing that won its
        // tile in planeChange is considered, matching what the stamp uses.
        void scanFootprints(const std::vector<ww::build::Crossing> &crossings, DeriveContext &ctx)
        {
            for (const ww::build::Crossing &c : crossings)
            {
                const auto it = ctx.planeChange.find(originKey(c.worldX, c.worldY, c.plane));
                if (it != ctx.planeChange.end() && it->second == &c)
                {
                    pairByFootprint(c, ctx);
                }
            }
        }
    }

    TransitionModel deriveVerticalTransitions(const ww::build::CollisionModel &collision,
                                              const ww::build::CollisionLookup &lookup,
                                              const TransitionModel &datasets,
                                              const std::vector<ww::build::Crossing> &crossings,
                                              FreshnessReport *outReport)
    {
        TransitionModel result;
        FreshnessReport report;
        const DatasetOrigins datasetOrigins = collectDatasetOrigins(datasets);
        const PlaneChangeMap planeChange = collectPlaneChangeCrossings(crossings);
        DeriveContext ctx{datasetOrigins, planeChange, lookup, result, report, {}, {}};
        for (const SquareClip &sq : collision.squares)
        {
            scanSquare(sq, ctx);
        }
        scanFootprints(crossings, ctx);
        if (outReport != nullptr)
        {
            *outReport = report;
        }
        return result;
    }

    void useDirectionalClimbOps(TransitionModel &ioDerived, const OpTable &table,
                                ClimbOpReport *outReport)
    {
        ClimbOpReport report;
        for (Transition &t : ioDerived.transitions)
        {
            if (t.objectId <= 0 || t.originPlane == t.destPlane)
            {
                continue;
            }
            ++report.checked;
            const bool isUp = t.destPlane > t.originPlane;
            const int32_t option = directionalLocOp(table, t.objectId, isUp);
            if (option < 0)
            {
                ++report.undirected;
                continue;
            }
            if (option == static_cast<int32_t>(t.optionIndex))
            {
                ++report.directional;
                continue;
            }
            t.optionIndex = static_cast<uint8_t>(option);
            ++report.retargeted;
        }
        if (outReport != nullptr)
        {
            *outReport = report;
        }
    }
}
