#include "cli/AvoidTests.h"

#include "c_api/worldwalker_c.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace
{
    // Walk-only planning: every teleport category off, so a route is the
    // baked graph's and a box on the ground is the only thing in its way.
    constexpr std::uint32_t kNoTeleports = (1u << WW_MOVE_TELEPORTS) | (1u << WW_MOVE_LODESTONES)
        | (1u << WW_MOVE_FAIRY_RINGS) | (1u << WW_MOVE_SPIRIT_TREES) | (1u << WW_MOVE_GLIDERS)
        | (1u << WW_MOVE_CHARTERS) | (1u << WW_MOVE_MAGIC_CARPETS) | (1u << WW_MOVE_OTHER_CHAINS);

    // Lodestones only: spell and item teleports off, so a long trip lands on
    // the lodestone nearest the goal.
    constexpr std::uint32_t kLodestonesOnly = 1u << WW_MOVE_TELEPORTS;

    constexpr WwTile kLumbridge{ 3222, 3218, 0 };
    constexpr WwTile kVarrockSquare{ 3212, 3428, 0 };
    constexpr WwTile kLumbridgeBank{ 3208, 3220, 2 };  // castle top floor, up either staircase
    constexpr WwTile kBurthorpe{ 2899, 3544, 0 };

    // At most this many crossings are boxed one at a time in the sweep.
    constexpr std::size_t kMaxBoxedCrossings = 12;

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

    // A query's outcome, copied out of the C ABI's buffer, plus its links.
    struct Planned
    {
        ww_result                status{WW_ERR_INTERNAL};
        std::vector<WwStep>      steps;
        float                    cost{0.0f};
        bool                     isPathZeroed{false};
        std::string              error;
        ww_result                linkStatus{WW_ERR_INTERNAL};
        std::vector<WwRouteLink> links;
    };

    Planned plan(CHandles &h, const WwTile &start, const WwTile &goal, const WwPlanOptions *options)
    {
        WwPath path{};
        Planned out;
        out.status = ww_query_opts(h.artifact, h.pool, start, WwGoal{ goal.x, goal.y, goal.plane, 0 },
                                   nullptr, nullptr, options, &path);
        out.error = out.status == WW_OK ? std::string() : std::string(ww_last_error());
        out.isPathZeroed = path.steps == nullptr && path.stepCount == 0 && path.cost == 0.0f;
        out.steps.assign(path.steps, path.steps + path.stepCount);
        out.cost = path.cost;
        WwRouteLink *links = nullptr;
        std::size_t linkCount = 0;
        out.linkStatus = ww_path_links(h.artifact, &path, &links, &linkCount);
        if (links != nullptr)
        {
            out.links.assign(links, links + linkCount);
        }
        ww_free(links);
        ww_path_free(&path);
        return out;
    }

    WwPlanOptions avoiding(std::span<const WwArea> areas, std::uint32_t disabledMoves)
    {
        WwPlanOptions o{};
        o.structSize = sizeof(WwPlanOptions);
        o.disabledMoves = disabledMoves;
        o.avoidAreas = areas.empty() ? nullptr : areas.data();
        o.avoidAreaCount = areas.size();
        return o;
    }

    WwArea boxAround(const WwTile &centre, std::int32_t radius)
    {
        return WwArea{ centre.plane, centre.x - radius, centre.y - radius, centre.x + radius,
                       centre.y + radius };
    }

    bool isIn(const WwArea &a, const WwTile &t)
    {
        const bool isOnPlane = a.plane == WW_AREA_ALL_PLANES || a.plane == t.plane;
        return isOnPlane && t.x >= a.minX && t.x <= a.maxX && t.y >= a.minY && t.y <= a.maxY;
    }

    bool isInAny(std::span<const WwArea> areas, const WwTile &t)
    {
        return std::any_of(areas.begin(), areas.end(), [&](const WwArea &a) { return isIn(a, t); });
    }

    WwTile tileOf(const WwStep &s)
    {
        return WwTile{ s.targetX, s.targetY, static_cast<std::int32_t>(s.plane) };
    }

    // Whether the straight line a -> b (on one plane, Chebyshev steps, both
    // ends included) touches a box: what a walk click from a to b could cut.
    bool isLineInAny(std::span<const WwArea> areas, const WwTile &a, const WwTile &b)
    {
        const std::int32_t len = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
        for (std::int32_t i = 0; i <= len; ++i)
        {
            const std::int32_t x = len == 0 ? a.x : a.x + (b.x - a.x) * i / len;
            const std::int32_t y = len == 0 ? a.y : a.y + (b.y - a.y) * i / len;
            if (isInAny(areas, WwTile{ x, y, a.plane }))
            {
                return true;
            }
        }
        return false;
    }

    // Everything of `p` that enters a box: a step target, the line walked to a
    // target from the previous one, or a transition's origin or landing.
    // Each is counted and the first is named in outWhy.
    int countEntries(CHandles &h, const Planned &p, const WwTile &start,
                     std::span<const WwArea> areas, std::string &outWhy)
    {
        int entries = 0;
        const auto note = [&](const std::string &why)
        {
            outWhy = entries++ == 0 ? why : outWhy;
        };
        WwTile prev = start;
        for (std::size_t i = 0; i < p.steps.size(); ++i)
        {
            const WwTile at = tileOf(p.steps[i]);
            if (isInAny(areas, at))
            {
                note("step " + std::to_string(i) + " target in a box");
            }
            if (prev.plane == at.plane && isLineInAny(areas, prev, at))
            {
                note("line to step " + std::to_string(i) + " crosses a box");
            }
            prev = at;
            if (p.steps[i].kind != WW_STEP_KIND_TRANSITION)
            {
                continue;
            }
            WwTransitionInfo info{};
            if (ww_transition_describe(h.artifact, p.steps[i].transitionIndex, &info) != WW_OK)
            {
                note("step " + std::to_string(i) + " transition not describable");
                continue;
            }
            const bool isOriginIn = info.origin != WW_TRANSITION_ORIGIN_GLOBAL && isInAny(areas, info.originTile);
            if (isOriginIn || isInAny(areas, info.destTile))
            {
                note("step " + std::to_string(i) + " transition starts or lands in a box");
            }
            prev = info.destTile;
        }
        return entries;
    }

    bool isSamePath(const Planned &a, const Planned &b)
    {
        return a.status == b.status && a.cost == b.cost && a.steps.size() == b.steps.size()
            && (a.steps.empty()
                || std::memcmp(a.steps.data(), b.steps.data(), a.steps.size() * sizeof(WwStep)) == 0);
    }

    bool usesTransition(const Planned &p, std::uint32_t transitionIndex)
    {
        return std::any_of(p.steps.begin(), p.steps.end(), [&](const WwStep &s)
                           { return s.kind == WW_STEP_KIND_TRANSITION && s.transitionIndex == transitionIndex; });
    }

    std::string tileText(const WwTile &t)
    {
        return "(" + std::to_string(t.x) + "," + std::to_string(t.y) + ",p" + std::to_string(t.plane) + ")";
    }

    int report(bool isPass, const std::string &line)
    {
        std::printf("  avoid: %s %s\n", isPass ? "PASS" : "FAIL", line.c_str());
        return isPass ? 0 : 1;
    }

    // The walk target nearest the middle of `p` that lies at least `clear`
    // tiles (Chebyshev) from both ends, on the start's plane.
    bool pickMidWalkTarget(const Planned &p, const WwTile &start, const WwTile &goal,
                           std::int32_t clear, WwTile &outTile)
    {
        const std::size_t mid = p.steps.size() / 2;
        for (std::size_t off = 0; off <= mid; ++off)
        {
            for (const std::size_t i : { mid - off, mid + off })
            {
                if (i >= p.steps.size() || p.steps[i].kind != WW_STEP_KIND_WALK)
                {
                    continue;
                }
                const WwTile t = tileOf(p.steps[i]);
                const auto far = [&](const WwTile &e)
                { return std::max(std::abs(t.x - e.x), std::abs(t.y - e.y)) >= clear; };
                if (t.plane == start.plane && far(start) && far(goal))
                {
                    outTile = t;
                    return true;
                }
            }
        }
        return false;
    }

    // A: a box on the middle of a long walk. Failure: no route, any entry
    // into the box, the same path as without it, or a cheaper one.
    int testDetour(CHandles &h)
    {
        const WwPlanOptions none = avoiding({}, kNoTeleports);
        const Planned base = plan(h, kLumbridge, kVarrockSquare, &none);
        WwTile mid{};
        if (base.status != WW_OK || !pickMidWalkTarget(base, kLumbridge, kVarrockSquare, 20, mid))
        {
            return report(false, "detour: no baseline Lumbridge->Varrock walk (status "
                                     + std::to_string(base.status) + " " + base.error + ")");
        }
        const WwArea box[] = { boxAround(mid, 4) };
        const WwPlanOptions opts = avoiding(box, kNoTeleports);
        const Planned avoided = plan(h, kLumbridge, kVarrockSquare, &opts);
        std::string why;
        const int baseEntries = countEntries(h, base, kLumbridge, box, why);
        why.clear();
        const int entries = countEntries(h, avoided, kLumbridge, box, why);
        const bool isPass = baseEntries > 0 && avoided.status == WW_OK && entries == 0
            && !isSamePath(base, avoided) && avoided.cost >= base.cost;
        return report(isPass, "detour: Lumbridge->Varrock, 9x9 box at " + tileText(mid)
                                  + ": baseline entries=" + std::to_string(baseEntries)
                                  + " (expect > 0) status=" + std::to_string(avoided.status)
                                  + " (expect 0) entries=" + std::to_string(entries) + " (expect 0) "
                                  + why + " cost " + std::to_string(base.cost) + " -> "
                                  + std::to_string(avoided.cost));
    }

    // B: each local crossing the castle-bank route takes, boxed one at a
    // time (its origin tile, every plane). Failure: a refusal other than no
    // route, the boxed crossing still used, or any entry into the box; or no
    // crossing to box at all (the test would have tested nothing).
    int testBoxedCrossings(CHandles &h)
    {
        const WwPlanOptions none = avoiding({}, kNoTeleports);
        const Planned base = plan(h, kLumbridge, kLumbridgeBank, &none);
        int failures = 0;
        std::size_t boxed = 0;
        int detours = 0;
        for (const WwStep &s : base.steps)
        {
            WwTransitionInfo info{};
            const bool isLocal = s.kind == WW_STEP_KIND_TRANSITION
                && ww_transition_describe(h.artifact, s.transitionIndex, &info) == WW_OK
                && info.origin != WW_TRANSITION_ORIGIN_GLOBAL;
            if (!isLocal || boxed >= kMaxBoxedCrossings)
            {
                continue;
            }
            ++boxed;
            const WwArea box[] = { WwArea{ WW_AREA_ALL_PLANES, info.originTile.x, info.originTile.y,
                                           info.originTile.x, info.originTile.y } };
            const WwPlanOptions opts = avoiding(box, kNoTeleports);
            const Planned p = plan(h, kLumbridge, kLumbridgeBank, &opts);
            std::string why;
            const int entries = p.status == WW_OK ? countEntries(h, p, kLumbridge, box, why) : 0;
            const bool isPass = (p.status == WW_OK && entries == 0 && !usesTransition(p, s.transitionIndex))
                || p.status == WW_ERR_NOT_FOUND;
            detours += p.status == WW_OK ? 1 : 0;
            failures += report(isPass, "crossing " + std::to_string(s.transitionIndex) + " origin "
                                           + tileText(info.originTile) + " boxed: status="
                                           + std::to_string(p.status) + " (expect 0 or 2) entries="
                                           + std::to_string(entries) + " (expect 0) " + why);
        }
        failures += report(boxed > 0 && detours > 0,
                           "crossings boxed=" + std::to_string(boxed) + " (expect > 0), detoured="
                               + std::to_string(detours) + " (expect > 0: the castle has two staircases)");
        return failures;
    }

    // The ring of boxes two tiles thick at Chebyshev distance 6..7 round `g`,
    // on every plane. Any walk from outside to `g` must step on it.
    std::vector<WwArea> ringAround(const WwTile &g)
    {
        return {
            WwArea{ WW_AREA_ALL_PLANES, g.x - 7, g.y + 6, g.x + 7, g.y + 7 },
            WwArea{ WW_AREA_ALL_PLANES, g.x - 7, g.y - 7, g.x + 7, g.y - 6 },
            WwArea{ WW_AREA_ALL_PLANES, g.x - 7, g.y - 5, g.x - 6, g.y + 5 },
            WwArea{ WW_AREA_ALL_PLANES, g.x + 6, g.y - 5, g.x + 7, g.y + 5 },
        };
    }

    // C: no route outside the boxes. Failure: a route (it could only cross
    // the ring), a status other than NOT_FOUND, a non-zeroed path, no error
    // text, or no route even without the ring (then the ring proved nothing).
    int testNoRoute(CHandles &h)
    {
        const WwPlanOptions none = avoiding({}, kNoTeleports);
        const Planned base = plan(h, kLumbridge, kVarrockSquare, &none);
        const std::vector<WwArea> ring = ringAround(kVarrockSquare);
        const WwPlanOptions opts = avoiding(ring, kNoTeleports);
        const Planned p = plan(h, kLumbridge, kVarrockSquare, &opts);
        const bool isPass = base.status == WW_OK && p.status == WW_ERR_NOT_FOUND && p.isPathZeroed
            && !p.error.empty();
        return report(isPass, "no route: Varrock square ringed: without ring status="
                                  + std::to_string(base.status) + " (expect 0) with ring status="
                                  + std::to_string(p.status) + " (expect 2) zeroed="
                                  + std::to_string(p.isPathZeroed) + " \"" + p.error + "\"");
    }

    // G: a box holding the start, or the goal, is lifted. Failure: any path
    // other than the unboxed one.
    int testEndpointsExempt(CHandles &h)
    {
        const WwPlanOptions none = avoiding({}, kNoTeleports);
        const Planned base = plan(h, kLumbridge, kVarrockSquare, &none);
        const WwArea onStart[] = { boxAround(kLumbridge, 3) };
        const WwArea onGoal[] = { boxAround(kVarrockSquare, 3) };
        const WwPlanOptions startOpts = avoiding(onStart, kNoTeleports);
        const WwPlanOptions goalOpts = avoiding(onGoal, kNoTeleports);
        const Planned fromBox = plan(h, kLumbridge, kVarrockSquare, &startOpts);
        const Planned toBox = plan(h, kLumbridge, kVarrockSquare, &goalOpts);
        const bool isPass = base.status == WW_OK && isSamePath(base, fromBox) && isSamePath(base, toBox);
        return report(isPass, "endpoints exempt: box on start same=" + std::to_string(isSamePath(base, fromBox))
                                  + " box on goal same=" + std::to_string(isSamePath(base, toBox))
                                  + " (expect 1 1)");
    }

    // One malformed-options refusal. Failure: anything but WW_ERR_INVALID
    // with a zeroed path and an error.
    int expectRefused(CHandles &h, const char *label, const WwPlanOptions &o)
    {
        const Planned p = plan(h, kLumbridge, kVarrockSquare, &o);
        const bool isPass = p.status == WW_ERR_INVALID && p.isPathZeroed && !p.error.empty();
        return report(isPass, std::string("refused: ") + label + " status=" + std::to_string(p.status)
                                  + " (expect 1) \"" + p.error + "\"");
    }

    // F: the V1 size reads no avoid field; malformed V2 options are refused.
    int testOptionsAbi(CHandles &h)
    {
        const WwPlanOptions none = avoiding({}, kNoTeleports);
        const Planned base = plan(h, kLumbridge, kVarrockSquare, &none);
        const std::vector<WwArea> ring = ringAround(kVarrockSquare);
        WwPlanOptions v1 = avoiding(ring, kNoTeleports);
        v1.structSize = WW_PLAN_OPTIONS_SIZE_V1;  // an old caller: the ring past its struct is not its
        const Planned old = plan(h, kLumbridge, kVarrockSquare, &v1);
        int failures = report(isSamePath(base, old) && base.status == WW_OK,
                              "V1 structSize ignores the avoid fields: same path="
                                  + std::to_string(isSamePath(base, old)) + " (expect 1)");
        WwPlanOptions o = avoiding(ring, kNoTeleports);
        o.structSize = 40;
        failures += expectRefused(h, "structSize 40", o);
        o = avoiding({}, kNoTeleports);
        o.avoidAreaCount = 1;
        failures += expectRefused(h, "NULL avoidAreas with a count", o);
        const WwArea badPlane[] = { WwArea{ 4, 0, 0, 1, 1 } };
        failures += expectRefused(h, "plane 4", avoiding(badPlane, kNoTeleports));
        const WwArea inverted[] = { WwArea{ 0, 10, 0, 9, 1 } };
        failures += expectRefused(h, "minX > maxX", avoiding(inverted, kNoTeleports));
        const std::vector<WwArea> tooMany(WW_MAX_AVOID_AREAS + 1, WwArea{ 0, 0, 0, 0, 0 });
        failures += expectRefused(h, "WW_MAX_AVOID_AREAS + 1 areas", avoiding(tooMany, kNoTeleports));
        return failures;
    }

    // Whether `p`'s links are exactly its Transition steps, in order, each
    // landing where ww_transition_describe says and named only when
    // runtime-loaded.
    bool areLinksConsistent(CHandles &h, const Planned &p, std::string &outWhy)
    {
        std::size_t next = 0;
        for (std::size_t i = 0; i < p.steps.size(); ++i)
        {
            if (p.steps[i].kind != WW_STEP_KIND_TRANSITION)
            {
                continue;
            }
            WwTransitionInfo info{};
            const bool isDescribed = ww_transition_describe(h.artifact, p.steps[i].transitionIndex, &info) == WW_OK;
            if (next >= p.links.size() || !isDescribed)
            {
                outWhy = "missing link for step " + std::to_string(i);
                return false;
            }
            const WwRouteLink &l = p.links[next++];
            const bool isNamed = l.name[0] != '\0';
            const bool isMatch = l.stepIndex == i && l.transitionIndex == p.steps[i].transitionIndex
                && l.kind == info.kind && l.origin == info.origin
                && std::memcmp(&l.toTile, &info.destTile, sizeof(WwTile)) == 0
                && l.fromTile.x == p.steps[i].targetX && l.fromTile.y == p.steps[i].targetY
                && l.fromTile.plane == static_cast<std::int32_t>(p.steps[i].plane)
                && isNamed == (info.isRuntimeLoaded != 0);
            if (!isMatch)
            {
                outWhy = "link " + std::to_string(next - 1) + " does not match step " + std::to_string(i);
                return false;
            }
        }
        outWhy = next == p.links.size() ? "" : "extra links";
        return p.linkStatus == WW_OK && next == p.links.size();
    }

    // D: links on a baked-stairs route and a lodestone route. Failure: a
    // link list that is not the Transition steps; a stairs link with a name;
    // no "Burthorpe" lodestone cast from the start on the lodestone route.
    int testRouteLinks(CHandles &h, bool hasTeleports)
    {
        const WwPlanOptions walk = avoiding({}, kNoTeleports);
        const Planned stairs = plan(h, kLumbridge, kLumbridgeBank, &walk);
        std::string why;
        int failures = report(stairs.status == WW_OK && !stairs.links.empty() && areLinksConsistent(h, stairs, why),
                              "links: castle bank route links=" + std::to_string(stairs.links.size())
                                  + " (expect > 0, all baked, unnamed) " + why);
        if (!hasTeleports)
        {
            std::printf("  avoid: SKIP links: lodestone route (no teleport dir)\n");
            return failures;
        }
        const WwPlanOptions lodes = avoiding({}, kLodestonesOnly);
        const Planned trip = plan(h, kLumbridge, kBurthorpe, &lodes);
        const auto isBurthorpe = [](const WwRouteLink &l)
        {
            return l.kind == WW_TRANSITION_KIND_LODESTONE && l.origin == WW_TRANSITION_ORIGIN_GLOBAL
                && std::strcmp(l.name, "Burthorpe") == 0 && l.fromTile.x == kLumbridge.x
                && l.fromTile.y == kLumbridge.y && l.fromTile.plane == kLumbridge.plane;
        };
        const bool hasBurthorpe = std::any_of(trip.links.begin(), trip.links.end(), isBurthorpe);
        failures += report(trip.status == WW_OK && hasBurthorpe && areLinksConsistent(h, trip, why),
                           "links: Lumbridge->Burthorpe first link \""
                               + std::string(trip.links.empty() ? "" : trip.links.front().name)
                               + "\" kind=" + std::to_string(trip.links.empty() ? -1 : trip.links.front().kind)
                               + " (expect a Burthorpe lodestone cast at the start) " + why);
        return failures;
    }

    // E: a box over the Burthorpe lodestone's landing, the goal outside it.
    // Failure: the route still lands in the box, any entry into it, or a
    // baseline that never landed there (the box would prove nothing).
    int testTeleportLandingAvoided(CHandles &h, bool hasTeleports)
    {
        if (!hasTeleports)
        {
            std::printf("  avoid: SKIP teleport landing (no teleport dir)\n");
            return 0;
        }
        const WwTile goal{ kBurthorpe.x + 14, kBurthorpe.y - 12, 0 };
        const WwPlanOptions lodes = avoiding({}, kLodestonesOnly);
        const Planned base = plan(h, kLumbridge, goal, &lodes);
        const auto landing = std::find_if(base.links.begin(), base.links.end(), [](const WwRouteLink &l)
                                          { return l.kind == WW_TRANSITION_KIND_LODESTONE; });
        if (base.status != WW_OK || landing == base.links.end())
        {
            return report(false, "teleport landing: no lodestone in the baseline to " + tileText(goal)
                                     + " (status " + std::to_string(base.status) + ")");
        }
        const WwArea box[] = { boxAround(landing->toTile, 3) };
        const WwPlanOptions opts = avoiding(box, kLodestonesOnly);
        const Planned p = plan(h, kLumbridge, goal, &opts);
        std::string why;
        const int entries = p.status == WW_OK ? countEntries(h, p, kLumbridge, box, why) : 0;
        const bool isPass = (p.status == WW_OK || p.status == WW_ERR_NOT_FOUND) && entries == 0
            && !isIn(box[0], goal);
        const std::string used = p.links.empty() ? std::string("none") : std::string(p.links.front().name);
        return report(isPass, "teleport landing: \"" + std::string(landing->name) + "\" lands "
                                  + tileText(landing->toTile) + ", boxed: status=" + std::to_string(p.status)
                                  + " entries=" + std::to_string(entries) + " (expect 0) first link now \""
                                  + used + "\" " + why);
    }

    // ww_path_links' own refusals. Failure: anything but the documented code.
    int testLinksAbi(CHandles &h)
    {
        WwRouteLink sentinel{};
        WwRouteLink *links = &sentinel;  // must come back NULL
        std::size_t count = 7;
        WwPath empty{};
        const ww_result emptyStatus = ww_path_links(h.artifact, &empty, &links, &count);
        int failures = report(emptyStatus == WW_OK && links == nullptr && count == 0,
                              "links: empty path -> WW_OK, NULL, 0");
        WwStep stale{ WW_STEP_KIND_TRANSITION, 0, 0, 0, 0, UINT32_MAX - 1u };
        WwPath bad{ &stale, 1, 0.0f, 0 };
        const ww_result staleStatus = ww_path_links(h.artifact, &bad, &links, &count);
        failures += report(staleStatus == WW_ERR_NOT_FOUND && links == nullptr && count == 0,
                           "links: out-of-range index -> WW_ERR_NOT_FOUND (" + std::to_string(staleStatus) + ")");
        WwPath noSteps{ nullptr, 2, 0.0f, 0 };
        failures += report(ww_path_links(h.artifact, &noSteps, &links, &count) == WW_ERR_INVALID,
                           "links: NULL steps with a count -> WW_ERR_INVALID");
        failures += report(ww_path_links(nullptr, &empty, &links, &count) == WW_ERR_INVALID,
                           "links: NULL artifact -> WW_ERR_INVALID");
        return failures;
    }
}

int runAvoidTests(const char *wwaPath, const char *teleportDir)
{
    CHandles h(wwaPath, teleportDir);
    if (h.pool == nullptr)
    {
        std::printf("  avoid: could not open %s: %s\n", wwaPath, ww_last_error());
        return 1;
    }
    const bool hasTeleports = teleportDir != nullptr;
    int failures = testDetour(h);
    failures += testBoxedCrossings(h);
    failures += testNoRoute(h);
    failures += testEndpointsExempt(h);
    failures += testOptionsAbi(h);
    failures += testRouteLinks(h, hasTeleports);
    failures += testTeleportLandingAvoided(h, hasTeleports);
    failures += testLinksAbi(h);
    std::printf("avoid: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
