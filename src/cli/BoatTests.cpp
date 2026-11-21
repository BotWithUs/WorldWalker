#include "cli/BoatTests.h"

#include "c_api/worldwalker_c.h"
#include "runtime/BoatWater.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

namespace
{
    constexpr std::int32_t kLakePlane = 1;

    // The four wayward souls' spots on Lake Mnemosyne, read off a player's
    // rowing clicks live (BotWithUs-Scripts SpiritOfWarStage).
    constexpr WwTile kNorthWestSpot{ 1007, 1892, kLakePlane };
    constexpr WwTile kSouthWestSpot{ 1003, 1701, kLakePlane };
    constexpr WwTile kSouthEastSpot{ 1196, 1687, kLakePlane };
    constexpr WwTile kNorthEastSpot{ 1198, 1890, kLakePlane };

    // Tiles the boat stood on live 2026-11-21: on the way to the north-west
    // spot, and stuck south of the island north-east of the city after a
    // straight row toward the north-east spot ran into it.
    constexpr WwTile kAfloatNorth{ 1023, 1882, kLakePlane };
    constexpr WwTile kBelowIsland{ 1098, 1846, kLakePlane };

    // Ed's shed, where the boat is rented, ashore.
    constexpr WwTile kEdsShed{ 1119, 1833, kLakePlane };

    // The City of Um lodestone's landing, ashore in the city.
    constexpr WwTile kUmLodestone{ 1083, 1768, kLakePlane };

    // The furthest a boarding step may land from the start, as
    // PathAssembler's kBoardReachTiles allows.
    constexpr std::int32_t kBoardReach = 20;
    // The furthest a rowed route to a goal ashore may end from it, as
    // PathAssembler's kShoreReachTiles allows.
    constexpr std::int32_t kShoreReach = 32;

    // Lumbridge, a walk nowhere near the lake.
    constexpr WwTile kLumbridgeA{ 3222, 3218, 0 };
    constexpr WwTile kLumbridgeB{ 3233, 3222, 0 };

    struct Tally
    {
        int run{};
        int failed{};
    };

    // RAII over the C ABI handles the suite plans with.
    struct CHandles
    {
        ww_artifact     *artifact{nullptr};
        ww_context_pool *pool{nullptr};

        explicit CHandles(const char *wwaPath)
        {
            artifact = ww_artifact_open(wwaPath);
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

    // RAII over one planned path.
    struct OwnedPath
    {
        WwPath path{};

        ~OwnedPath()
        {
            ww_path_free(&path);
        }

        std::span<const WwStep> steps() const
        {
            return {path.steps, path.stepCount};
        }
    };

    std::int32_t chebyshev(std::int32_t ax, std::int32_t ay, std::int32_t bx, std::int32_t by)
    {
        return std::max(std::abs(ax - bx), std::abs(ay - by));
    }

    void report(Tally &tally, const char *name, bool isPass, const char *detail)
    {
        ++tally.run;
        if (!isPass)
        {
            ++tally.failed;
        }
        std::printf("boat: %-34s %s  %s\n", name, isPass ? "PASS" : "FAIL", detail);
    }

    bool plan(CHandles &h, WwTile start, WwTile goal, OwnedPath &out)
    {
        const WwGoal target{ goal.x, goal.y, goal.plane, 0 };
        return ww_query(h.artifact, h.pool, start, target, nullptr, &out.path) == WW_OK;
    }

    // The index of the first step from `from` that is not lake water, or -1.
    int firstDryStep(std::span<const WwStep> steps, std::size_t from)
    {
        for (std::size_t i = from; i < steps.size(); ++i)
        {
            if (!ww::runtime::boat_water::isWater(steps[i].targetX, steps[i].targetY,
                                                  steps[i].plane))
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // A route afloat to a spot afloat: every step on the water, the last on
    // the spot.
    void checkRowed(CHandles &h, Tally &tally, const char *name, WwTile start, WwTile goal)
    {
        OwnedPath path;
        char detail[160];
        if (!plan(h, start, goal, path) || path.path.stepCount == 0)
        {
            report(tally, name, false, "no route");
            return;
        }
        const auto steps = path.steps();
        const int dry = firstDryStep(steps, 0);
        const WwStep &last = steps.back();
        const bool isOnGoal = last.targetX == goal.x && last.targetY == goal.y;
        std::snprintf(detail, sizeof detail, "%zu steps, first dry step %d, ends %d,%d",
                      steps.size(), dry, last.targetX, last.targetY);
        report(tally, name, dry < 0 && isOnGoal, detail);
    }

    // From Ed's shed ashore: one step onto the water within kBoardReach, then
    // every step on the water to the spot.
    void checkBoarding(CHandles &h, Tally &tally)
    {
        OwnedPath path;
        char detail[160];
        if (!plan(h, kEdsShed, kNorthWestSpot, path) || path.path.stepCount < 2)
        {
            report(tally, "board at Ed's shed", false, "no route");
            return;
        }
        const auto steps = path.steps();
        const WwStep &board = steps.front();
        const std::int32_t reach = chebyshev(board.targetX, board.targetY, kEdsShed.x, kEdsShed.y);
        const int dry = firstDryStep(steps, 0);
        std::snprintf(detail, sizeof detail, "boards at %d,%d (%d tiles), first dry step %d",
                      board.targetX, board.targetY, reach, dry);
        report(tally, "board at Ed's shed", dry < 0 && reach <= kBoardReach, detail);
    }

    // Afloat to the lodestone ashore: rowed to the water nearest it.
    void checkShoreGoal(CHandles &h, Tally &tally)
    {
        OwnedPath path;
        char detail[160];
        if (!plan(h, kAfloatNorth, kUmLodestone, path) || path.path.stepCount == 0)
        {
            report(tally, "row to the shore by a goal ashore", false, "no route");
            return;
        }
        const auto steps = path.steps();
        const WwStep &last = steps.back();
        const std::int32_t gap = chebyshev(last.targetX, last.targetY, kUmLodestone.x, kUmLodestone.y);
        const int dry = firstDryStep(steps, 0);
        std::snprintf(detail, sizeof detail, "ends %d,%d, %d tiles from the goal, first dry step %d",
                      last.targetX, last.targetY, gap, dry);
        report(tally, "row to the shore by a goal ashore", dry < 0 && gap <= kShoreReach, detail);
    }

    // A walk far from the lake is planned as before: a route, and no step of
    // it in the lake table.
    void checkLandUnchanged(CHandles &h, Tally &tally)
    {
        OwnedPath path;
        const bool isRouted = plan(h, kLumbridgeA, kLumbridgeB, path) && path.path.stepCount > 0;
        report(tally, "Lumbridge walk unchanged", isRouted, isRouted ? "routed" : "no route");
    }
}

int runBoatTests(const char *wwaPath)
{
    CHandles h(wwaPath);
    if (h.artifact == nullptr || h.pool == nullptr)
    {
        std::printf("boat: load FAIL  %s\n", ww_last_error());
        return 1;
    }
    Tally tally;
    checkRowed(h, tally, "row north-west to south-east", kAfloatNorth, kSouthEastSpot);
    checkRowed(h, tally, "row round the island to north-east", kBelowIsland, kNorthEastSpot);
    checkRowed(h, tally, "row north-west to south-west", kNorthWestSpot, kSouthWestSpot);
    checkBoarding(h, tally);
    checkShoreGoal(h, tally);
    checkLandUnchanged(h, tally);
    std::printf("boat: %d of %d failed\n", tally.failed, tally.run);
    return tally.failed > 0 ? 1 : 0;
}
