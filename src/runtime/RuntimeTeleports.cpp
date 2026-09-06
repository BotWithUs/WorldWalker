#include "runtime/RuntimeTeleports.h"

#include "data/DatasetLoader.h"
#include "data/TransitionCost.h"
#include "data/Transitions.h"
#include "format/Artifact.h"
#include "runtime/TileScan.h"
#include "runtime/WorldView.h"

#include <cstring>
#include <vector>

namespace ww::runtime
{
    namespace
    {
        // Snap a teleport dest to the nearest standable tile within `radius`. A
        // lodestone's teleport tile usually sits on the (blocked) lodestone
        // object footprint, so its raw dest is off-area; the bake snaps dests
        // through the same findNearestTile with the same radius
        // (TransitionBuilder kSnapRadius=5), so a runtime-loaded teleport lands
        // on the tile the baked one would have. Without this the dest area is
        // -1 and frontier seeding skips it. Leaves the tile unchanged (returns
        // false) if nothing standable is in range — it then simply never seeds,
        // which is the safe failure.
        constexpr int32_t kDestSnapRadius = 5;

        bool snapDestToStandable(WorldView &view, int32_t &x, int32_t &y, int32_t plane)
        {
            const auto standable = [&](int32_t tx, int32_t ty)
            {
                return view.isStandable(tx, ty, plane);
            };
            return findNearestTile(x, y, kDestSnapRadius, true, standable, x, y);
        }

        // Flatten one build-time Transition into the serialized POD shape, with
        // requirement / chain ranges offset by the pools' current base lengths.
        // Mirrors ArtifactWriter::encodeTransition (build-side) — the runtime
        // path must produce byte-identical records to the baked ones.
        format::TransitionRecord encode(const data::Transition &t,
                                        std::size_t reqBase, std::size_t chainBase,
                                        std::vector<format::RequirementRecord> &reqPool,
                                        std::vector<format::ChainStepRecord> &chainPool)
        {
            format::TransitionRecord r{};
            r.kind = static_cast<uint8_t>(t.kind);
            r.flags = t.isGlobalOrigin ? format::kTransitionFlagGlobalOrigin
                                       : static_cast<uint8_t>(0);
            r.originPlane = t.originPlane;
            r.destPlane = t.destPlane;
            r.originX = t.originX;
            r.originY = t.originY;
            r.destX = t.destX;
            r.destY = t.destY;
            r.objectId = t.objectId;
            r.shape = t.shape;
            r.rotation = t.rotation;
            r.optionIndex = t.optionIndex;
            std::memcpy(r.code, t.code, sizeof(r.code));
            r.cost = data::computeCost(t);  // loadGlobalTeleports yields raw (pre-cost) records
            r.costQuick = t.costQuick;

            r.requirementStart = static_cast<uint32_t>(reqBase + reqPool.size());
            r.requirementCount = static_cast<uint32_t>(t.requirements.size());
            for (const data::Requirement &req : t.requirements)
            {
                format::RequirementRecord rr{};
                rr.kind = static_cast<uint8_t>(req.kind);
                rr.id = req.id;
                rr.amount = req.amount;
                reqPool.push_back(rr);
            }

            r.chainStart = static_cast<uint32_t>(chainBase + chainPool.size());
            r.chainCount = static_cast<uint32_t>(t.chain.size());
            for (const data::ChainStep &cs : t.chain)
            {
                format::ChainStepRecord cr{};
                cr.kind = static_cast<uint8_t>(cs.kind);
                cr.a = cs.a;
                cr.b = cs.b;
                cr.c = cs.c;
                cr.d = cs.d;
                cr.e = cs.e;
                cr.f = cs.f;
                cr.g = cs.g;
                cr.h = cs.h;
                cr.i = cs.i;
                chainPool.push_back(cr);
            }
            return r;
        }
    }

    std::size_t loadGlobalTeleportsInto(format::ArtifactReader &reader,
                                        const std::string &directory)
    {
        const data::LoadedDatasets loaded = data::loadGlobalTeleports(directory);

        // Drop any previously appended set first so a reload is idempotent, then
        // build records whose pool offsets are relative to the (now baked-only)
        // pool sizes.
        reader.truncateToBaked();
        const std::size_t reqBase = reader.requirements().size();
        const std::size_t chainBase = reader.chainSteps().size();

        // Snap dests to standable tiles (matches the bake). WorldView reads the
        // baked collision grids — appending transitions later does not affect it.
        WorldView view(reader);

        std::vector<format::TransitionRecord> txRecords;
        std::vector<format::RequirementRecord> reqPool;
        std::vector<format::ChainStepRecord> chainPool;
        txRecords.reserve(loaded.model.transitions.size());
        for (const data::Transition &source : loaded.model.transitions)
        {
            data::Transition t = source;
            snapDestToStandable(view, t.destX, t.destY, static_cast<int32_t>(t.destPlane));
            txRecords.push_back(encode(t, reqBase, chainBase, reqPool, chainPool));
        }

        reader.appendTransitions(txRecords, reqPool, chainPool);
        return txRecords.size();
    }
}
