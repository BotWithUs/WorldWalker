#include "cli/ExcludeTests.h"

#include "c_api/worldwalker_c.h"
#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "format/MoveCategory.h"
#include "runtime/RuntimeTeleports.h"
#include "runtime/TileScan.h"
#include "runtime/WorldView.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace
{
    using ww::format::TransitionRecord;

    // Local transitions tried by the detour scan, spread over the whole table.
    constexpr std::size_t kDetourCases = 800;
    // Cases each of the narrower checks stops after.
    constexpr std::size_t kSoleCases = 20;
    constexpr std::size_t kSiblingCases = 40;
    constexpr std::size_t kGlobalCases = 40;
    constexpr std::size_t kGlobalStartsPerTx = 6;
    constexpr std::int32_t kMinGlobalSpread = 300;

    struct Query
    {
        WwTile start;
        WwGoal goal;
    };

    // RAII over the C ABI handles the suite drives.
    struct CHandles
    {
        ww_artifact     *artifact{nullptr};
        ww_context_pool *pool{nullptr};

        CHandles(const char *wwaPath, const char *teleportDir)
        {
            artifact = ww_artifact_open(wwaPath);
            if (artifact != nullptr && teleportDir != nullptr)
            {
                ww_artifact_load_teleports(artifact, teleportDir);
            }
            pool = artifact != nullptr ? ww_context_pool_create(artifact, 1) : nullptr;
        }

        ~CHandles()
        {
            ww_context_pool_destroy(pool);
            ww_artifact_close(artifact);
        }

        CHandles(const CHandles &) = delete;
        CHandles &operator=(const CHandles &) = delete;
    };

    // A query's outcome, copied out of the C ABI's buffer.
    struct Planned
    {
        ww_result           status{WW_ERR_INTERNAL};
        std::vector<WwStep> steps;
        float               cost{0.0f};
        bool                isPathZeroed{false};  // outPath came back all-zero
    };

    Planned copyOut(ww_result status, WwPath &path)
    {
        Planned out;
        out.status = status;
        out.isPathZeroed = path.steps == nullptr && path.stepCount == 0 && path.cost == 0.0f
                        && path.pad == 0;
        out.steps.assign(path.steps, path.steps + path.stepCount);
        out.cost = path.cost;
        ww_path_free(&path);
        return out;
    }

    Planned queryOpts(CHandles &h, const Query &q, const WwPlanOptions *options)
    {
        WwPath path{};
        const ww_result status =
            ww_query_opts(h.artifact, h.pool, q.start, q.goal, nullptr, nullptr, options, &path);
        return copyOut(status, path);
    }

    Planned queryMoves(CHandles &h, const Query &q, std::uint32_t mask)
    {
        WwPath path{};
        const ww_result status =
            ww_query_moves(h.artifact, h.pool, q.start, q.goal, nullptr, nullptr, &path, mask);
        return copyOut(status, path);
    }

    Planned queryEx(CHandles &h, const Query &q)
    {
        WwPath path{};
        const ww_result status = ww_query_ex(h.artifact, h.pool, q.start, q.goal, nullptr, nullptr,
                                             &path);
        return copyOut(status, path);
    }

    WwPlanOptions makeOptions(std::span<const std::uint32_t> excluded, std::uint32_t flags = 0,
                              std::uint32_t disabledMoves = 0)
    {
        WwPlanOptions o{};
        o.structSize = sizeof(WwPlanOptions);
        o.disabledMoves = disabledMoves;
        o.flags = flags;
        o.excludedTransitions = excluded.empty() ? nullptr : excluded.data();
        o.excludedTransitionCount = excluded.size();
        return o;
    }

    Planned queryExcluding(CHandles &h, const Query &q, std::span<const std::uint32_t> excluded,
                           std::uint32_t flags = 0)
    {
        const WwPlanOptions o = makeOptions(excluded, flags);
        return queryOpts(h, q, &o);
    }

    bool isSamePath(const Planned &a, const Planned &b)
    {
        return a.status == b.status && a.cost == b.cost && a.steps.size() == b.steps.size()
            && (a.steps.empty()
                || std::memcmp(a.steps.data(), b.steps.data(), a.steps.size() * sizeof(WwStep)) == 0);
    }

    bool usesAny(const Planned &p, std::span<const std::uint32_t> indices)
    {
        return std::any_of(p.steps.begin(), p.steps.end(), [&](const WwStep &s)
        {
            return s.kind == WW_STEP_KIND_TRANSITION
                && std::find(indices.begin(), indices.end(), s.transitionIndex) != indices.end();
        });
    }

    bool uses(const Planned &p, std::uint32_t index)
    {
        return usesAny(p, std::span<const std::uint32_t>(&index, 1));
    }

    bool isLocal(const TransitionRecord &tx)
    {
        return (tx.flags & ww::format::kTransitionFlagGlobalOrigin) == 0u;
    }

    // The rows of `tx`'s loc as WW_PLAN_EXCLUDE_LOC_SIBLINGS defines them in the
    // header: local rows with the same loc id at the same origin tile.
    std::vector<std::uint32_t> locRows(std::span<const TransitionRecord> txs,
                                       const TransitionRecord &tx)
    {
        std::vector<std::uint32_t> rows;
        for (std::size_t i = 0; i < txs.size(); ++i)
        {
            const TransitionRecord &o = txs[i];
            if (isLocal(o) && o.objectId == tx.objectId && o.originX == tx.originX
                && o.originY == tx.originY && o.originPlane == tx.originPlane)
            {
                rows.push_back(static_cast<std::uint32_t>(i));
            }
        }
        return rows;
    }

    // From a standable tile beside a local transition's origin (in `fromArea`
    // when it is >= 0) to its destination.
    bool localQuery(ww::runtime::WorldView &view, const TransitionRecord &tx, std::int32_t fromArea,
                    Query &outQuery)
    {
        const std::int32_t plane = static_cast<std::int32_t>(tx.originPlane);
        const auto isUsable = [&](std::int32_t x, std::int32_t y)
        {
            return view.isStandable(x, y, plane) && (fromArea < 0 || view.areaAt(x, y, plane) == fromArea);
        };
        std::int32_t x = 0;
        std::int32_t y = 0;
        if (!ww::runtime::findNearestTile(tx.originX, tx.originY, ww::data::kTransitionApproachRadius,
                                          true, isUsable, tx.originX, tx.originY, x, y))
        {
            return false;
        }
        outQuery.start = WwTile{ x, y, plane };
        outQuery.goal = WwGoal{ tx.destX, tx.destY, static_cast<std::int32_t>(tx.destPlane), 0 };
        return true;
    }

    int fail(const char *what)
    {
        std::printf("exclude: FAIL %s\n", what);
        return 1;
    }

    // Shared state of one suite run.
    struct Suite
    {
        const ww::format::ArtifactReader &reader;
        ww::runtime::WorldView           &view;
        CHandles                         &h;
    };

    // The four ways of asking with nothing excluded must give one answer, and
    // the mask must mean the same through either entry point.
    int checkIdentity(Suite &s, const Query &q, const Planned &base)
    {
        const WwPlanOptions empty = makeOptions({});
        WwPlanOptions emptyWithList = empty;
        const std::uint32_t unused = 0;
        emptyWithList.excludedTransitions = &unused;  // count 0: the pointer must not be read
        int failures = 0;
        failures += isSamePath(base, queryOpts(s.h, q, &empty)) ? 0 : 1;
        failures += isSamePath(base, queryOpts(s.h, q, &emptyWithList)) ? 0 : 1;
        failures += isSamePath(base, queryEx(s.h, q)) ? 0 : 1;
        failures += isSamePath(base, queryMoves(s.h, q, 0u)) ? 0 : 1;
        const std::uint32_t doors = 1u << WW_MOVE_DOORS;
        const std::uint32_t masks[] = { doors, WW_RESTRICT_FREE_TO_PLAY };
        for (const std::uint32_t mask : masks)
        {
            const WwPlanOptions masked = makeOptions({}, 0, mask);
            failures += isSamePath(queryMoves(s.h, q, mask), queryOpts(s.h, q, &masked)) ? 0 : 1;
        }
        return failures;
    }

    // Every local transition a route from beside it uses, excluded: the
    // re-plan never contains it. Counts how often it went round and how often
    // nothing else reached the goal.
    int checkDetours(Suite &s)
    {
        const auto txs = s.reader.transitions();
        const std::size_t stride = std::max<std::size_t>(1, txs.size() / kDetourCases);
        std::size_t tried = 0;
        std::size_t detoured = 0;
        std::size_t notFound = 0;
        std::size_t leaks = 0;
        std::size_t identityMismatches = 0;
        for (std::size_t i = 0; i < txs.size(); i += stride)
        {
            Query q{};
            if (!isLocal(txs[i]) || !localQuery(s.view, txs[i], -1, q))
            {
                continue;
            }
            const Planned base = queryOpts(s.h, q, nullptr);
            identityMismatches += static_cast<std::size_t>(checkIdentity(s, q, base));
            const auto t = static_cast<std::uint32_t>(i);
            if (base.status != WW_OK || !uses(base, t))
            {
                continue;
            }
            ++tried;
            const std::uint32_t excluded[] = { t };
            const Planned around = queryExcluding(s.h, q, excluded);
            if (around.status == WW_OK)
            {
                ++detoured;
                if (uses(around, t))
                {
                    ++leaks;
                    std::printf("exclude: LEAK tx%u still planned (%d,%d,p%d)->(%d,%d,p%d)\n", t,
                                q.start.x, q.start.y, q.start.plane, q.goal.x, q.goal.y, q.goal.plane);
                }
            }
            else
            {
                notFound += around.status == WW_ERR_NOT_FOUND ? 1u : 0u;
            }
        }
        std::printf("exclude: detour scan: %zu routes used their transition; excluded -> %zu went"
                    " round, %zu NOT_FOUND, %zu still used it (expect 0); identity mismatches=%zu"
                    " (expect 0)\n", tried, detoured, notFound, leaks, identityMismatches);
        int failures = 0;
        failures += leaks == 0 ? 0 : fail("an excluded transition was planned");
        failures += identityMismatches == 0 ? 0 : fail("empty options did not match the old entry points");
        failures += detoured > 0 ? 0 : fail("no excluded route went round (expected some)");
        failures += (detoured + notFound == tried) ? 0 : fail("an excluded query errored");
        return failures;
    }

    // An edge that is the only way into its area, with no global teleport
    // landing there: excluded, nothing reaches the goal, so NOT_FOUND. The
    // "no other way" comes from the area graph, not from the planner.
    int checkSoleEntries(Suite &s)
    {
        const auto edges = s.reader.areaEdges();
        const auto txs = s.reader.transitions();
        std::vector<std::uint32_t> entries(s.reader.areaNodes().size(), 0u);
        for (const ww::format::AreaEdgeRecord &e : edges)
        {
            if (static_cast<std::size_t>(e.toArea) < entries.size())
            {
                ++entries[static_cast<std::size_t>(e.toArea)];
            }
        }
        for (const std::uint32_t g : s.reader.globalOriginTransitions())
        {
            const TransitionRecord &tx = txs[g];
            const std::int32_t area = s.view.areaAt(tx.destX, tx.destY, static_cast<std::int32_t>(tx.destPlane));
            if (area >= 0 && static_cast<std::size_t>(area) < entries.size())
            {
                ++entries[static_cast<std::size_t>(area)];
            }
        }
        std::size_t tested = 0;
        std::size_t correct = 0;
        for (const ww::format::AreaEdgeRecord &e : edges)
        {
            if (tested >= kSoleCases)
            {
                break;
            }
            const bool isSole = e.fromArea != e.toArea && static_cast<std::size_t>(e.toArea) < entries.size()
                             && entries[static_cast<std::size_t>(e.toArea)] == 1u;
            Query q{};
            if (!isSole || !isLocal(txs[e.transitionIndex])
                || !localQuery(s.view, txs[e.transitionIndex], e.fromArea, q))
            {
                continue;
            }
            const Planned base = queryOpts(s.h, q, nullptr);
            if (base.status != WW_OK || !uses(base, e.transitionIndex))
            {
                continue;
            }
            ++tested;
            const std::uint32_t excluded[] = { e.transitionIndex };
            const Planned none = queryExcluding(s.h, q, excluded);
            const bool isRight = none.status == WW_ERR_NOT_FOUND && none.isPathZeroed;
            correct += isRight ? 1u : 0u;
            if (!isRight)
            {
                std::printf("exclude: sole entry tx%u gave status %d (expect %d)\n", e.transitionIndex,
                            static_cast<int>(none.status), WW_ERR_NOT_FOUND);
            }
        }
        std::printf("exclude: sole entries: %zu tested, %zu NOT_FOUND once excluded (expect all)\n",
                    tested, correct);
        int failures = tested > 0 ? 0 : fail("no sole-entry case found");
        failures += correct == tested ? 0 : fail("a sole entry excluded still found a route");
        return failures;
    }

    // A loc with several rows. Asked to reach row B's destination, the route
    // uses B. Excluding another row A of the same loc without the flag keeps B
    // (the plan is unchanged); with WW_PLAN_EXCLUDE_LOC_SIBLINGS no row of the
    // loc is planned.
    int checkSiblings(Suite &s)
    {
        const auto txs = s.reader.transitions();
        std::size_t tested = 0;
        std::size_t kept = 0;
        std::size_t clean = 0;
        for (std::size_t b = 0; b < txs.size() && tested < kSiblingCases; ++b)
        {
            if (!isLocal(txs[b]))
            {
                continue;
            }
            const std::vector<std::uint32_t> rows = locRows(txs, txs[b]);
            Query q{};
            if (rows.size() < 2 || rows.front() != b || !localQuery(s.view, txs[b], -1, q))
            {
                continue;  // one case per loc: only from its first row
            }
            const auto bIndex = static_cast<std::uint32_t>(b);
            const Planned base = queryOpts(s.h, q, nullptr);
            if (base.status != WW_OK || !uses(base, bIndex))
            {
                continue;
            }
            ++tested;
            const std::uint32_t excluded[] = { rows[1] };  // A: a sibling, not B
            const Planned plain = queryExcluding(s.h, q, excluded);
            const Planned widened = queryExcluding(s.h, q, excluded, WW_PLAN_EXCLUDE_LOC_SIBLINGS);
            const bool isKept = plain.status == WW_OK && uses(plain, bIndex);
            const bool isClean = !usesAny(widened, rows);
            kept += isKept ? 1u : 0u;
            clean += isClean ? 1u : 0u;
            if (!isKept || !isClean)
            {
                std::printf("exclude: siblings loc %d rows=%zu B=tx%u A=tx%u: plain status=%d uses B=%d;"
                            " widened status=%d uses a row=%d\n", txs[b].objectId, rows.size(), bIndex,
                            rows[1], static_cast<int>(plain.status), uses(plain, bIndex) ? 1 : 0,
                            static_cast<int>(widened.status), usesAny(widened, rows) ? 1 : 0);
            }
        }
        std::printf("exclude: loc siblings: %zu locs tested; flag 0 kept the sibling in %zu (expect all);"
                    " WW_PLAN_EXCLUDE_LOC_SIBLINGS planned no row in %zu (expect all)\n",
                    tested, kept, clean);
        int failures = tested > 0 ? 0 : fail("no multi-row loc case found");
        failures += kept == tested ? 0 : fail("flag 0 excluded a sibling it was not given");
        failures += clean == tested ? 0 : fail("WW_PLAN_EXCLUDE_LOC_SIBLINGS left a sibling planned");
        return failures;
    }

    // A global teleport a far route seeds, excluded: never planned.
    int checkGlobals(Suite &s)
    {
        const auto txs = s.reader.transitions();
        const auto nodes = s.reader.areaNodes();
        std::size_t tested = 0;
        std::size_t clean = 0;
        for (const std::uint32_t g : s.reader.globalOriginTransitions())
        {
            if (tested >= kGlobalCases)
            {
                break;
            }
            const TransitionRecord &tx = txs[g];
            std::size_t starts = 0;
            for (std::size_t a = 0; a < nodes.size() && starts < kGlobalStartsPerTx; a += 97)
            {
                const ww::format::AreaNodeRecord &n = nodes[a];
                const bool isFar = n.plane == 0 && n.tileCount >= 256
                    && std::max(std::abs(n.centroidX - tx.destX), std::abs(n.centroidY - tx.destY))
                           >= kMinGlobalSpread
                    && s.view.isStandable(n.centroidX, n.centroidY, 0);
                if (!isFar)
                {
                    continue;
                }
                ++starts;
                const Query q{ WwTile{ n.centroidX, n.centroidY, 0 },
                               WwGoal{ tx.destX, tx.destY, static_cast<std::int32_t>(tx.destPlane), 0 } };
                const Planned base = queryOpts(s.h, q, nullptr);
                if (base.status != WW_OK || !uses(base, g))
                {
                    continue;
                }
                ++tested;
                const std::uint32_t excluded[] = { g };
                const Planned around = queryExcluding(s.h, q, excluded);
                const bool isClean = !uses(around, g);
                clean += isClean ? 1u : 0u;
                if (!isClean)
                {
                    std::printf("exclude: LEAK global tx%u seeded from (%d,%d)\n", g, q.start.x, q.start.y);
                }
                break;  // one route per teleport
            }
        }
        std::printf("exclude: globals: %zu teleports a route seeded; excluded, %zu never planned (expect all)\n",
                    tested, clean);
        int failures = tested > 0 ? 0 : fail("no route seeded a global teleport");
        failures += clean == tested ? 0 : fail("an excluded global teleport was seeded");
        return failures;
    }

    // Malformed options and out-of-range indices are refused with
    // WW_ERR_INVALID, a zeroed path and a message; the last valid index is not.
    int checkRefusals(Suite &s)
    {
        const auto count = static_cast<std::uint32_t>(s.reader.transitions().size());
        const Query q{ WwTile{ s.reader.areaNodes()[0].centroidX, s.reader.areaNodes()[0].centroidY,
                               static_cast<std::int32_t>(s.reader.areaNodes()[0].plane) },
                       WwGoal{ s.reader.areaNodes()[0].centroidX, s.reader.areaNodes()[0].centroidY,
                               static_cast<std::int32_t>(s.reader.areaNodes()[0].plane), 0 } };
        const std::uint32_t pastEnd[] = { 0u, count };
        const std::uint32_t maxIndex[] = { std::numeric_limits<std::uint32_t>::max() };
        const std::uint32_t lastIndex[] = { count - 1u };

        struct Refusal
        {
            const char   *name;
            WwPlanOptions options;
        };
        std::vector<Refusal> refusals;
        WwPlanOptions o = makeOptions({});
        o.structSize = 0;
        refusals.push_back({ "structSize 0", o });
        o.structSize = 24;
        refusals.push_back({ "structSize 24", o });
        o.structSize = sizeof(WwPlanOptions) + 8;
        refusals.push_back({ "structSize from a newer header", o });
        o = makeOptions({}, 1u << 1);
        refusals.push_back({ "unknown flag bit 1", o });
        o = makeOptions({}, 1u << 31);
        refusals.push_back({ "unknown flag bit 31", o });
        o = makeOptions({});
        o.pad = 1;
        refusals.push_back({ "non-zero pad", o });
        o = makeOptions({});
        o.excludedTransitionCount = 1;
        refusals.push_back({ "NULL list with count 1", o });
        refusals.push_back({ "index == transition count", makeOptions(pastEnd) });
        refusals.push_back({ "index UINT32_MAX", makeOptions(maxIndex) });
        refusals.push_back({ "index UINT32_MAX with siblings", makeOptions(maxIndex, WW_PLAN_EXCLUDE_LOC_SIBLINGS) });

        int failures = 0;
        for (const Refusal &r : refusals)
        {
            const Planned p = queryOpts(s.h, q, &r.options);
            const std::string message = ww_last_error();
            const bool isRefused = p.status == WW_ERR_INVALID && p.isPathZeroed && !message.empty();
            std::printf("exclude: refuse %-32s status=%d (expect %d) \"%s\"\n", r.name,
                        static_cast<int>(p.status), WW_ERR_INVALID, message.c_str());
            failures += isRefused ? 0 : fail(r.name);
        }
        const WwPlanOptions last = makeOptions(lastIndex);
        const Planned p = queryOpts(s.h, q, &last);
        std::printf("exclude: last index tx%u status=%d (expect not %d)\n", count - 1u,
                    static_cast<int>(p.status), WW_ERR_INVALID);
        failures += p.status != WW_ERR_INVALID ? 0 : fail("the last valid index was refused");
        return failures;
    }
}

int runExcludeTests(const char *wwaPath, const char *teleportDir)
{
    try
    {
        // The plain artifact for every local check, so the area graph the sole
        // entry check reads is exactly the one the C ABI handle plans over.
        const ww::format::ArtifactReader reader(wwaPath);
        ww::runtime::WorldView view(reader);
        CHandles h(wwaPath, nullptr);
        if (h.artifact == nullptr || h.pool == nullptr)
        {
            std::printf("exclude: could not open %s through the C ABI: %s\n", wwaPath, ww_last_error());
            return 1;
        }
        Suite suite{ reader, view, h };
        int failures = checkRefusals(suite);
        failures += checkDetours(suite);
        failures += checkSoleEntries(suite);
        failures += checkSiblings(suite);
        if (teleportDir != nullptr)
        {
            ww::format::ArtifactReader withTeleports(wwaPath);
            ww::runtime::loadGlobalTeleportsInto(withTeleports, teleportDir);
            ww::runtime::WorldView teleportView(withTeleports);
            CHandles th(wwaPath, teleportDir);
            Suite teleportSuite{ withTeleports, teleportView, th };
            failures += checkGlobals(teleportSuite);
        }
        else
        {
            std::printf("exclude: globals skipped (no teleport dir)\n");
        }
        std::printf("exclude: %d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    catch (const std::exception &e)
    {
        std::printf("exclude: %s\n", e.what());
        return 1;
    }
}
