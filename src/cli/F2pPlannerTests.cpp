#include "cli/F2pPlannerTests.h"

#include "c_api/worldwalker_c.h"
#include "data/DatasetLoader.h"
#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "format/F2pLand.h"
#include "format/MoveCategory.h"
#include "runtime/RuntimeTeleports.h"
#include "runtime/TileSearch.h"
#include "runtime/WorldView.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace
{
    using ww::format::MoveCategory;

    // The lodestones a free-to-play plan may seed: the eleven of cache enum
    // 12260 CHEEVO_LODESTONE_FREE, and the two newer ones the enum predates
    // that the wiki lists as free (tools/f2p/gen_members_flags.py,
    // CURATED_LODESTONES). Fort Forinthry, also newer, is members.
    const std::set<std::string> kFreeLodestones = {
        "Al Kharid", "Ashdale", "Burthorpe", "Draynor Village", "Edgeville", "Falador",
        "Lumbridge", "Port Sarim", "Taverley", "Varrock", "Wilderness Crater",
        "City of Um", "Wendlewick",
    };

    // A plan that cannot find its way out of members land must say so at once:
    // the live failure this exists for gave up after five re-plans.
    constexpr double kFastRefusalMs = 250.0;

    constexpr uint32_t kFreeToPlay = WW_RESTRICT_FREE_TO_PLAY;

    int fail(const std::string &message)
    {
        std::printf("f2p: FAIL %s\n", message.c_str());
        return 1;
    }

    int expect(bool isOk, const std::string &message)
    {
        return isOk ? 0 : fail(message);
    }

    // RAII over the C ABI handles, with the runtime teleports loaded.
    struct CHandles
    {
        ww_artifact     *artifact{nullptr};
        ww_context_pool *pool{nullptr};
        bool             isTeleportsLoaded{false};

        CHandles(const char *wwaPath, const std::string &datasetDir)
        {
            artifact = ww_artifact_open(wwaPath);
            if (artifact != nullptr)
            {
                isTeleportsLoaded = ww_artifact_load_teleports(artifact, datasetDir.c_str()) == WW_OK;
                pool = ww_context_pool_create(artifact, 1);
            }
        }

        ~CHandles()
        {
            ww_context_pool_destroy(pool);
            ww_artifact_close(artifact);
        }

        CHandles(const CHandles &) = delete;
        CHandles &operator=(const CHandles &) = delete;
    };

    struct Planned
    {
        bool                isFound{false};
        std::vector<WwStep> steps;
        float               cost{0.0f};
        double              ms{0.0};
    };

    Planned plan(CHandles &h, WwTile start, WwTile goal, uint32_t mask)
    {
        WwPath path{};
        Planned out;
        const WwGoal g{goal.x, goal.y, goal.plane, 0};
        const auto t0 = std::chrono::steady_clock::now();
        out.isFound = ww_query_moves(h.artifact, h.pool, start, g, nullptr, nullptr, &path, mask) == WW_OK;
        out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        out.steps.assign(path.steps, path.steps + path.stepCount);
        out.cost = path.cost;
        ww_path_free(&path);
        return out;
    }

    Planned planEx(CHandles &h, WwTile start, WwTile goal)
    {
        WwPath path{};
        Planned out;
        out.isFound = ww_query_ex(h.artifact, h.pool, start, WwGoal{goal.x, goal.y, goal.plane, 0},
                                  nullptr, nullptr, &path) == WW_OK;
        out.steps.assign(path.steps, path.steps + path.stepCount);
        out.cost = path.cost;
        ww_path_free(&path);
        return out;
    }

    bool isSamePath(const Planned &a, const Planned &b)
    {
        return a.isFound == b.isFound && a.cost == b.cost && a.steps.size() == b.steps.size()
            && (a.steps.empty()
                || std::memcmp(a.steps.data(), b.steps.data(), a.steps.size() * sizeof(WwStep)) == 0);
    }

    std::string tileText(WwTile t)
    {
        return "(" + std::to_string(t.x) + "," + std::to_string(t.y) + ",p" + std::to_string(t.plane) + ")";
    }

    // What the planner layer reads besides the C handles: a reader carrying
    // the same transitions in the same order (the same artifact with the same
    // teleport files appended), and each runtime teleport's name.
    struct Fixture
    {
        ww::format::ArtifactReader reader;
        std::vector<std::string> labels;  // by transition index; empty for baked

        Fixture(const char *wwaPath, const std::string &datasetDir)
            : reader(wwaPath)
        {
            ww::runtime::loadGlobalTeleportsInto(reader, datasetDir);
            labels.assign(reader.bakedTransitions(), std::string());
            for (const ww::data::Transition &t : ww::data::loadGlobalTeleports(datasetDir).model.transitions)
            {
                labels.push_back(t.label);
            }
        }

        bool isMembersOnly(uint32_t i) const
        {
            return i < reader.membersOnly().size() && reader.membersOnly()[i] != 0u;
        }

        MoveCategory categoryOf(uint32_t i) const
        {
            return static_cast<MoveCategory>(reader.moveCategories()[i]);
        }
    };

    // ---- The free-to-play plan's invariants -----------------------------------

    // Every transition free to play, every walk on free land, every lodestone
    // one of the eleven.
    int checkFreeToPlayPlan(const Fixture &f, const Planned &p, const std::string &what)
    {
        int failures = 0;
        const ww::format::F2pLand &land = f.reader.f2pLand();
        for (const WwStep &s : p.steps)
        {
            if (s.kind == WW_STEP_KIND_WALK)
            {
                failures += expect(land.isFreeToPlay(s.targetX, s.targetY, s.plane),
                                   what + ": walks onto members land at "
                                       + tileText(WwTile{s.targetX, s.targetY, s.plane}));
                continue;
            }
            failures += expect(!f.isMembersOnly(s.transitionIndex),
                               what + ": takes members-only transition #" + std::to_string(s.transitionIndex));
            const auto &tx = f.reader.transitions()[s.transitionIndex];
            const bool isLodestone = static_cast<ww::data::TransitionKind>(tx.kind)
                                     == ww::data::TransitionKind::Lodestone;
            failures += expect(!isLodestone || kFreeLodestones.contains(f.labels[s.transitionIndex]),
                               what + ": seeds lodestone " + f.labels[s.transitionIndex]);
        }
        return failures;
    }

    bool usesTransition(const Planned &p, uint32_t index)
    {
        return std::any_of(p.steps.begin(), p.steps.end(), [&](const WwStep &s)
                           { return s.kind == WW_STEP_KIND_TRANSITION && s.transitionIndex == index; });
    }

    // ---- Test 1: charter ships ------------------------------------------------

    bool isCharterNpcRow(const ww::format::ArtifactReader &reader, const ww::format::TransitionRecord &tx)
    {
        if (tx.chainCount == 0u)
        {
            return false;
        }
        const ww::format::ChainStepRecord &first = reader.chainSteps()[tx.chainStart];
        return static_cast<ww::data::ChainStepKind>(first.kind) == ww::data::ChainStepKind::ClickNpc
            && first.f <= 4656 && first.g >= 4650;
    }

    int checkCharters(const Fixture &f, CHandles &h)
    {
        int failures = 0;
        std::size_t charters = 0;
        std::size_t traderNpcRows = 0;
        const auto txs = f.reader.transitions();
        for (uint32_t i = 0; i < txs.size(); ++i)
        {
            const bool isCharter = f.categoryOf(i) == MoveCategory::Charters;
            const bool isTrader = isCharterNpcRow(f.reader, txs[i]);
            charters += isCharter ? 1u : 0u;
            traderNpcRows += isTrader ? 1u : 0u;
            if ((isCharter || isTrader) && !f.isMembersOnly(i))
            {
                failures += fail("charters: #" + std::to_string(i) + " is not members-only");
            }
        }
        failures += expect(charters > 0 && traderNpcRows > 0,
                           "charters: no charter transitions found to test");
        const WwTile lumbridge{3222, 3218, 0};
        const WwTile brimhaven{2760, 3238, 0};
        const Planned members = plan(h, lumbridge, brimhaven, 0u);
        const Planned freeToPlay = plan(h, lumbridge, brimhaven, kFreeToPlay);
        failures += expect(members.isFound && isSamePath(members, planEx(h, lumbridge, brimhaven)),
                           "charters: Lumbridge -> Brimhaven with the bit clear must plan as before");
        failures += expect(!freeToPlay.isFound && freeToPlay.ms < kFastRefusalMs,
                           "charters: Lumbridge -> Brimhaven free-to-play must be refused at plan time");
        std::printf("f2p: test 1 charters: %zu charter, %zu trader-NPC rows members-only; "
                    "Lumbridge->Brimhaven members=%s (%zu steps) f2p=%s (%.1f ms)\n",
                    charters, traderNpcRows, members.isFound ? "found" : "NOT FOUND",
                    members.steps.size(), freeToPlay.isFound ? "FOUND" : "NOT_FOUND", freeToPlay.ms);
        return failures;
    }

    // ---- Test 2: the Taverley Dungeon entrance ------------------------------

    int checkTaverleyDungeon(const Fixture &f)
    {
        int failures = 0;
        std::size_t found = 0;
        const auto txs = f.reader.transitions();
        for (uint32_t i = 0; i < txs.size(); ++i)
        {
            const auto &tx = txs[i];
            if (tx.objectId == 66990 && tx.originX == 2879 && tx.originY == 3459 && tx.destY > 6400)
            {
                ++found;
                failures += expect(f.isMembersOnly(i), "loc 66990: #" + std::to_string(i)
                                                           + " into Taverley Dungeon is not members-only");
            }
        }
        failures += expect(found > 0, "loc 66990: the Taverley Dungeon entrance is not in the artifact");
        std::printf("f2p: test 2 loc 66990 -> Taverley Dungeon: %zu transition(s), members-only\n", found);
        return failures;
    }

    // ---- Tests 3 and 4: members goals --------------------------------------

    int checkRefused(CHandles &h, WwTile start, WwTile goal, const char *what)
    {
        const Planned members = plan(h, start, goal, 0u);
        const Planned freeToPlay = plan(h, start, goal, kFreeToPlay);
        std::printf("f2p: %s %s: members=%s f2p=%s (%.1f ms)\n", what, tileText(goal).c_str(),
                    members.isFound ? "found" : "NOT FOUND",
                    freeToPlay.isFound ? "FOUND" : "NOT_FOUND", freeToPlay.ms);
        return expect(!freeToPlay.isFound && freeToPlay.ms < kFastRefusalMs,
                      std::string(what) + ": a free-to-play plan must be refused at once");
    }

    int checkArrives(const Fixture &f, CHandles &h, WwTile start, WwTile goal, const char *what)
    {
        const Planned freeToPlay = plan(h, start, goal, kFreeToPlay);
        const Planned members = plan(h, start, goal, 0u);
        std::printf("f2p: %s %s -> %s: f2p=%s (%zu steps, cost %.1f, %.1f ms) members cost %.1f\n",
                    what, tileText(start).c_str(), tileText(goal).c_str(),
                    freeToPlay.isFound ? "found" : "NOT FOUND", freeToPlay.steps.size(),
                    static_cast<double>(freeToPlay.cost), freeToPlay.ms, static_cast<double>(members.cost));
        int failures = expect(freeToPlay.isFound, std::string(what) + ": a free-to-play plan must arrive");
        failures += expect(members.isFound && isSamePath(members, planEx(h, start, goal)),
                           std::string(what) + ": the bit-clear plan must equal ww_query_ex");
        return failures + checkFreeToPlayPlan(f, freeToPlay, what);
    }

    int checkGoals(const Fixture &f, CHandles &h)
    {
        const WwTile lumbridge{3222, 3218, 0};
        int failures = 0;
        failures += checkRefused(h, lumbridge, WwTile{3304, 3110, 0}, "test 3 south of Shantay");
        failures += checkArrives(f, h, lumbridge, WwTile{3303, 3120, 0}, "test 3 Shantay north");
        // The brief's test 4 named the Digsite, which the wiki says is free
        // (Archaeology, 2020). Fort Forinthry is the walk-only members ground
        // in the same area: Varrock walks into it through no transition.
        failures += checkRefused(h, lumbridge, WwTile{3305, 3553, 0}, "test 4 Fort Forinthry");
        failures += checkArrives(f, h, lumbridge, WwTile{3360, 3420, 0}, "test 4 Digsite (free)");
        return failures;
    }

    // ---- Test 5: positive controls -------------------------------------------

    int checkControls(const Fixture &f, CHandles &h)
    {
        struct Control
        {
            const char *name;
            WwTile start;
            WwTile goal;
        };
        const std::array<Control, 5> controls = {{
            {"test 5 Lumbridge->Varrock", {3222, 3218, 0}, {3212, 3428, 0}},
            {"test 5 Draynor->Edgeville", {3093, 3245, 0}, {3094, 3492, 0}},
            {"test 5 Falador->Taverley", {2965, 3380, 0}, {2895, 3450, 0}},
            {"test 5 Varrock->Wilderness", {3212, 3428, 0}, {3100, 3680, 0}},
            {"test 5 Burthorpe->Port Sarim", {2899, 3544, 0}, {3010, 3220, 0}},
        }};
        int failures = 0;
        for (const Control &c : controls)
        {
            failures += checkArrives(f, h, c.start, c.goal, c.name);
        }
        return failures;
    }

    // Exactly the eleven free lodestones survive the bit.
    int checkSeedableLodestones(const Fixture &f)
    {
        std::set<std::string> admitted;
        const auto txs = f.reader.transitions();
        for (uint32_t i = 0; i < txs.size(); ++i)
        {
            const bool isLodestone = static_cast<ww::data::TransitionKind>(txs[i].kind)
                                     == ww::data::TransitionKind::Lodestone;
            if (isLodestone && !f.isMembersOnly(i))
            {
                admitted.insert(f.labels[i]);
            }
        }
        std::string list;
        for (const std::string &name : admitted)
        {
            list += (list.empty() ? "" : ", ") + name;
        }
        std::printf("f2p: test 5 lodestones a free-to-play plan may seed (%zu): %s\n", admitted.size(),
                    list.c_str());
        return expect(admitted == kFreeLodestones, "lodestones: the seedable set must be the 13 free ones");
    }

    // ---- AreaSearch's prune is subsumed ---------------------------------------

    // An area wholly on members land needs no prune of its own: every baked
    // edge into it, and every teleport landing in it, has its destination on
    // members land and so is already members-only.
    int checkPruneSubsumed(const Fixture &f)
    {
        const auto classes = f.reader.areaLandClasses();
        const auto isMembersArea = [&](int32_t area)
        {
            return area >= 0 && static_cast<std::size_t>(area) < classes.size()
                && classes[static_cast<std::size_t>(area)] == static_cast<uint8_t>(ww::format::LandClass::Members);
        };
        int failures = 0;
        std::size_t edgesIn = 0;
        for (const ww::format::AreaEdgeRecord &e : f.reader.areaEdges())
        {
            if (isMembersArea(e.toArea))
            {
                ++edgesIn;
                failures += expect(f.isMembersOnly(e.transitionIndex),
                                   "prune: edge into members area " + std::to_string(e.toArea) + " via #"
                                       + std::to_string(e.transitionIndex) + " is admitted");
            }
        }
        ww::runtime::WorldView view(f.reader);
        std::size_t landings = 0;
        for (const uint32_t i : f.reader.globalOriginTransitions())
        {
            const auto &tx = f.reader.transitions()[i];
            if (isMembersArea(view.areaAt(tx.destX, tx.destY, tx.destPlane)))
            {
                ++landings;
                failures += expect(f.isMembersOnly(i), "prune: teleport #" + std::to_string(i)
                                                           + " landing in a members area is admitted");
            }
        }
        std::printf("f2p: AreaSearch prune subsumed: %zu edges and %zu teleports into wholly-members"
                    " areas, all members-only\n", edgesIn, landings);
        return failures;
    }

    // ---- The walk fence, on the real map ---------------------------------------

    bool isPathOnFreeLand(const ww::runtime::TilePath &path, const ww::format::F2pLand &land, int32_t plane)
    {
        return std::all_of(path.tiles.begin(), path.tiles.end(), [&](const ww::runtime::TilePoint &t)
                           { return land.isFreeToPlay(t.x, t.y, plane); });
    }

    // The step fence on the real map, with land made for the test: all of it
    // free but a 5x5 hole laid across the middle of the unfenced Lumbridge ->
    // Varrock walk. Fenced, the walk must go round the hole (open ground there
    // lets it) and never touch it. And the fence is one way: a walk that
    // starts in the hole may leave it.
    int checkWalkFence(const Fixture &f)
    {
        ww::runtime::WorldView view(f.reader);
        ww::runtime::TileSearch search(view);
        ww::runtime::TilePath open;
        if (!search.findPath(3222, 3218, 3212, 3428, 0, -1, open) || open.tiles.size() < 20)
        {
            return fail("fence: no unfenced Lumbridge -> Varrock walk to lay the hole across");
        }
        const ww::runtime::TilePoint mid = open.tiles[open.tiles.size() / 2];
        ww::format::F2pBoxRecord all{};
        all.maxX = 8191;
        all.maxY = 16383;
        all.planeMax = 3;
        ww::format::F2pBoxRecord hole = all;
        hole.minX = mid.x - 2;
        hole.minY = mid.y - 2;
        hole.maxX = mid.x + 2;
        hole.maxY = mid.y + 2;
        const ww::format::F2pLand land(std::span<const ww::format::F2pBoxRecord>(&all, 1),
                                       std::span<const ww::format::F2pBoxRecord>(&hole, 1));
        const ww::runtime::WalkFence fence{{}, &land};
        ww::runtime::TilePath fenced;
        ww::runtime::TilePath out;
        const bool isFenced = search.findPath(3222, 3218, 3212, 3428, 0, -1, fence, fenced);
        const bool isOut = search.findPath(mid.x, mid.y, 3212, 3428, 0, -1, fence, out);
        std::printf("f2p: walk fence: hole at (%d,%d) on the open walk (%zu tiles, cost %.1f);"
                    " fenced %s (%zu tiles, cost %.1f, %s the hole); from inside the hole %s\n",
                    mid.x, mid.y, open.tiles.size(), static_cast<double>(open.cost),
                    isFenced ? "found" : "none", fenced.tiles.size(), static_cast<double>(fenced.cost),
                    isFenced && isPathOnFreeLand(fenced, land, 0) ? "avoids" : "TOUCHES",
                    isOut ? "leaves" : "is STUCK");
        int failures = expect(isFenced && isPathOnFreeLand(fenced, land, 0),
                              "fence: the fenced walk must go round the hole");
        failures += expect(fenced.cost >= open.cost, "fence: a detour cannot be cheaper than the open walk");
        failures += expect(isOut, "fence: a walk starting on members land must be able to leave it");
        return failures;
    }

    // ---- Test 6: the Port Sarim -> Musa Point boat ----------------------------

    int checkMusaBoat(const Fixture &f, CHandles &h)
    {
        const auto txs = f.reader.transitions();
        std::vector<uint32_t> boats;
        for (uint32_t i = 0; i < f.reader.bakedTransitions(); ++i)
        {
            const auto &tx = txs[i];
            const bool isFromPortSarim = std::abs(tx.originX - 3028) <= 8 && std::abs(tx.originY - 3218) <= 8;
            const bool isToMusa = std::abs(tx.destX - 2955) <= 8 && std::abs(tx.destY - 3146) <= 8;
            if (isFromPortSarim && isToMusa && tx.originPlane == 0 && tx.destPlane == 0)
            {
                boats.push_back(i);
            }
        }
        if (boats.empty())
        {
            std::printf("f2p: test 6 boat SKIP (no Port Sarim -> Musa Point transition in this artifact;"
                        " it arrives with fix/port-sarim-musa-boat)\n");
            return 0;
        }
        const Planned p = plan(h, WwTile{3010, 3220, 0}, WwTile{2918, 3176, 0}, kFreeToPlay);
        const bool isBoatUsed = std::any_of(boats.begin(), boats.end(), [&](uint32_t i) { return usesTransition(p, i); });
        std::printf("f2p: test 6 Port Sarim -> Musa Point f2p=%s boat=%s\n", p.isFound ? "found" : "NOT FOUND",
                    isBoatUsed ? "used" : "not used");
        return expect(p.isFound && isBoatUsed, "boat: a free-to-play plan to Musa Point must take the boat")
             + checkFreeToPlayPlan(f, p, "test 6 boat");
    }

    // ---- An artifact without the section ---------------------------------------

    // No land: the bit still refuses the members networks, and nothing else
    // geographic (the documented old-artifact behaviour).
    int checkWithoutLand(const Fixture &f, CHandles &h)
    {
        int failures = 0;
        const auto txs = f.reader.transitions();
        for (uint32_t i = 0; i < txs.size(); ++i)
        {
            const bool isNetwork = ww::format::isMembersNetwork(f.categoryOf(i));
            if (isNetwork != f.isMembersOnly(i) && f.reader.membersOverrides()[i] < 0)
            {
                failures += fail("no land: #" + std::to_string(i) + " classified by geography");
            }
        }
        const Planned fort = plan(h, WwTile{3222, 3218, 0}, WwTile{3305, 3553, 0}, kFreeToPlay);
        failures += expect(fort.isFound, "no land: walks are not fenced, so Fort Forinthry still plans");
        std::printf("f2p: no F2pZones section: members networks refused, walks unfenced"
                    " (Fort Forinthry %s)\n", fort.isFound ? "plans" : "NOT FOUND");
        return failures;
    }

    int runAll(const Fixture &f, CHandles &h)
    {
        int failures = 0;
        failures += checkCharters(f, h);
        failures += checkTaverleyDungeon(f);
        failures += checkGoals(f, h);
        failures += checkControls(f, h);
        failures += checkSeedableLodestones(f);
        failures += checkPruneSubsumed(f);
        failures += checkWalkFence(f);
        failures += checkMusaBoat(f, h);
        return failures;
    }
}

int runF2pPlannerTests(const char *wwaPath, const std::filesystem::path &datasetDir)
{
    try
    {
        CHandles h(wwaPath, datasetDir.string());
        if (h.artifact == nullptr || h.pool == nullptr || !h.isTeleportsLoaded)
        {
            return fail(std::string("planner: cannot open the artifact or load teleports: ") + ww_last_error());
        }
        const Fixture f(wwaPath, datasetDir.string());
        if (!f.reader.hasF2pZones())
        {
            const int failures = checkWithoutLand(f, h);
            std::printf("f2p: planner SKIP (the artifact has no F2pZones section)\n");
            return failures;
        }
        const int failures = runAll(f, h);
        std::printf("f2p: planner %s\n", failures == 0 ? "PASS" : "FAIL");
        return failures;
    }
    catch (const std::exception &e)
    {
        return fail(std::string("planner: ") + e.what());
    }
}
