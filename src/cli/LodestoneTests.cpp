#include "cli/LodestoneTests.h"

#include "data/DatasetLoader.h"
#include "data/TransitionCost.h"
#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "runtime/AreaSearch.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/PathAssembler.h"
#include "runtime/RuntimeTeleports.h"
#include "runtime/TeleportPolicy.h"
#include "runtime/TileScan.h"
#include "runtime/TileSearch.h"
#include "runtime/WorldView.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    using ww::data::ChainStep;
    using ww::data::ChainStepKind;
    using ww::data::Requirement;
    using ww::data::RequirementKind;
    using ww::data::Transition;

    constexpr int32_t kComponentAction = 57;
    constexpr int32_t kFilterVarbit = 50990;
    // A second gate on the same route, to exercise the array spelling of
    // `requirements.varbit`. Any id does — the loader gives it no meaning.
    constexpr int32_t kCombinedBookVarbit = 3170;
    constexpr int32_t kLumbridgeUnlockVarbit = 35;
    constexpr int32_t kEdgevilleUnlockVarbit = 33;
    constexpr int32_t kLumbridgeX = 3233;
    constexpr int32_t kLumbridgeY = 3222;
    constexpr int32_t kEdgevilleX = 3067;
    constexpr int32_t kEdgevilleY = 3506;

    // The fixtures' chains, spelled out: the map opener on the minimap
    // (1465:33), a pick on the lodestone map (1092:<component>), and the cast
    // from the Magic ability book's abilities layer (1461:1, sub = spell slot).
    constexpr ChainStep kOpenMap{ChainStepKind::Click, kComponentAction, 1, -1, (1465 << 16) | 33};
    constexpr ChainStep kOpenWait{ChainStepKind::Wait, 6};
    constexpr ChainStep kTeleportWait{ChainStepKind::Wait, 18};
    constexpr ChainStep kPickLumbridge{ChainStepKind::Click, kComponentAction, 1, -1,
                                       (1092 << 16) | 17};
    constexpr ChainStep kPickEdgeville{ChainStepKind::Click, kComponentAction, 1, -1,
                                       (1092 << 16) | 15};
    constexpr ChainStep kCastLumbridge{ChainStepKind::Click, kComponentAction, 1, 234,
                                       (1461 << 16) | 1};

    constexpr Requirement kLumbridgeUnlocked{RequirementKind::Varbit, kLumbridgeUnlockVarbit, 1};
    constexpr Requirement kEdgevilleUnlocked{RequirementKind::Varbit, kEdgevilleUnlockVarbit, 1};
    constexpr Requirement kSpellsShown{RequirementKind::Varbit, kFilterVarbit, 0};
    constexpr Requirement kCombinedBook{RequirementKind::Varbit, kCombinedBookVarbit, 1};

    // loadGlobalTeleports reads both files; an empty spell file keeps the
    // loader's "dataset not found" warning out of the report.
    const char *const kEmptySpellFixture = R"({ "teleports": [] })";

    // Lumbridge carries the ability-book route, Edgeville carries none. The
    // option keys are left out so their default of 1 is exercised too.
    const char *const kRoutedFixture = R"({
      "lodestones": {
        "config": {
          "open_interface": 1465, "open_component": 33,
          "select_interface": 1092,
          "open_wait": 6, "teleport_wait": 18
        },
        "destinations": [
          { "name": "Lumbridge", "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "varbit": { "id": 35, "value": 1 } },
            "routes": [
              { "name": "ability book",
                "requirements": { "varbit": { "id": 50990, "value": 0 } },
                "chain": [ { "click": [1461, 1, 1, 234] }, { "wait": 18 } ] }
            ] },
          { "name": "Edgeville", "x": 3067, "y": 3506, "plane": 0, "component": 15,
            "requirements": { "varbit": { "id": 33, "value": 1 } } }
        ]
      }
    })";

    // No routes: the destination is the map chain and nothing else, exactly as
    // before routes existed.
    const char *const kNoRoutesFixture = R"({
      "lodestones": {
        "config": {
          "open_interface": 1465, "open_component": 33,
          "select_interface": 1092,
          "open_wait": 6, "teleport_wait": 18
        },
        "destinations": [
          { "name": "Lumbridge", "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "varbit": { "id": 35, "value": 1 } } }
        ]
      }
    })";

    // The array spelling of `requirements.varbit`: two gates on one route, on
    // top of the destination's own unlock.
    const char *const kArrayGateFixture = R"({
      "lodestones": {
        "config": {
          "open_interface": 1465, "open_component": 33,
          "select_interface": 1092,
          "open_wait": 6, "teleport_wait": 18
        },
        "destinations": [
          { "name": "Lumbridge", "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "varbit": { "id": 35, "value": 1 } },
            "routes": [
              { "requirements": { "varbit": [ { "id": 50990, "value": 0 },
                                              { "id": 3170, "value": 1 } ] },
                "chain": [ { "click": [1461, 1, 1, 234] }, { "wait": 18 } ] }
            ] }
        ]
      }
    })";

    // `routes` as an object rather than an array: must throw, not be ignored.
    const char *const kRoutesNotArrayFixture = R"({
      "lodestones": {
        "config": { "open_interface": 1465, "open_component": 33, "select_interface": 1092 },
        "destinations": [
          { "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "routes": { "chain": [ { "click": [1461, 1, 1, 234] } ] } }
        ]
      }
    })";

    // A route whose only step key is a typo. parseChain skips steps it does not
    // recognise, so this would otherwise be an edge that teleports nowhere.
    const char *const kRouteWithoutChainFixture = R"({
      "lodestones": {
        "config": { "open_interface": 1465, "open_component": 33, "select_interface": 1092 },
        "destinations": [
          { "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "routes": [ { "chain": [ { "clik": [1461, 1, 1, 234] } ] } ] }
        ]
      }
    })";

    // A scalar where a var gate belongs: must throw rather than leave the
    // transition ungated.
    const char *const kScalarVarbitFixture = R"({
      "lodestones": {
        "config": { "open_interface": 1465, "open_component": 33, "select_interface": 1092 },
        "destinations": [
          { "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "varbit": 35 } }
        ]
      }
    })";

    // The planner fixture: Lumbridge alone and carrying no unlock requirement,
    // so a snapshot holding only the filter varbit decides between the two
    // routes. The bake drops global teleports, but should an artifact still
    // carry a baked Lumbridge lodestone it would be gated on unlock varbit 35,
    // and leaving 35 unset keeps it out of the race.
    const char *const kPlannerFixture = R"({
      "lodestones": {
        "config": {
          "open_interface": 1465, "open_component": 33,
          "select_interface": 1092,
          "open_wait": 6, "teleport_wait": 18
        },
        "destinations": [
          { "name": "Lumbridge", "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "routes": [
              { "name": "ability book",
                "requirements": { "varbit": { "id": 50990, "value": 0 } },
                "chain": [ { "click": [1461, 1, 1, 234] }, { "wait": 18 } ] }
            ] }
        ]
      }
    })";

    // Varrock's west bank: a teleport-allowed overworld tile a couple of
    // hundred tiles from Lumbridge, so either route beats walking outright.
    constexpr int32_t kStartX = 3185;
    constexpr int32_t kStartY = 3436;
    constexpr int32_t kStartPlane = 0;
    constexpr int32_t kStartSnapRadius = 5;

    int fail(const char *what)
    {
        std::printf("  FAIL: %s\n", what);
        return 1;
    }

    void writeText(const std::filesystem::path &path, const char *text)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << text;
        stream.close();
        if (!stream)
        {
            throw std::runtime_error("could not write fixture " + path.string());
        }
    }

    // Stage `fixture` as the directory's item_teleports.json. The loader reads
    // fixed file names, so every fixture reuses the one directory.
    void stageFixture(const std::filesystem::path &dir, const char *fixture)
    {
        writeText(dir / "spell_teleports.json", kEmptySpellFixture);
        writeText(dir / "item_teleports.json", fixture);
    }

    ww::data::LoadedDatasets loadFixture(const std::filesystem::path &dir, const char *fixture)
    {
        stageFixture(dir, fixture);
        return ww::data::loadGlobalTeleports(dir.string());
    }

    // Unique per run: parallel `wwcli lodestones` invocations on one machine
    // used to overwrite each other's fixtures and remove_all the other's
    // directory mid-run, which reads as a loader bug.
    std::filesystem::path fixtureDir()
    {
        std::random_device entropy;
        char name[48];
        std::snprintf(name, sizeof(name), "wwcli_lodestone_fixtures_%08x%08x",
                      static_cast<unsigned>(entropy()), static_cast<unsigned>(entropy()));
        return std::filesystem::temp_directory_path() / name;
    }

    bool sameStep(const ChainStep &got, const ChainStep &want)
    {
        return got.kind == want.kind && got.a == want.a && got.b == want.b && got.c == want.c
            && got.d == want.d && got.e == want.e && got.f == want.f && got.g == want.g
            && got.h == want.h && got.i == want.i;
    }

    bool sameChain(const std::vector<ChainStep> &got, std::initializer_list<ChainStep> want)
    {
        if (got.size() != want.size())
        {
            return false;
        }
        std::size_t i = 0;
        for (const ChainStep &step : want)
        {
            if (!sameStep(got[i], step))
            {
                return false;
            }
            ++i;
        }
        return true;
    }

    bool sameRequirements(const std::vector<Requirement> &got,
                          std::initializer_list<Requirement> want)
    {
        if (got.size() != want.size())
        {
            return false;
        }
        std::size_t i = 0;
        for (const Requirement &req : want)
        {
            if (got[i].kind != req.kind || got[i].id != req.id || got[i].amount != req.amount)
            {
                return false;
            }
            ++i;
        }
        return true;
    }

    void printTransition(const char *label, const Transition &t)
    {
        std::printf("    %s: kind=%u global=%d dest=(%d,%d,p%u)\n", label,
                    static_cast<unsigned>(t.kind), t.isGlobalOrigin ? 1 : 0, t.destX, t.destY,
                    static_cast<unsigned>(t.destPlane));
        for (const Requirement &r : t.requirements)
        {
            std::printf("      req kind=%u id=%d amount=%d\n", static_cast<unsigned>(r.kind),
                        r.id, r.amount);
        }
        for (const ChainStep &s : t.chain)
        {
            std::printf("      step kind=%u a=%d b=%d c=%d d=0x%08x\n",
                        static_cast<unsigned>(s.kind), s.a, s.b, s.c,
                        static_cast<unsigned>(s.d));
        }
    }

    // One loaded transition against its expected shape. Prints the transition
    // when it does not match, so the report shows what the loader produced.
    int expectLodestone(const char *label, const Transition &t, int32_t destX, int32_t destY,
                        std::initializer_list<Requirement> requirements,
                        std::initializer_list<ChainStep> chain)
    {
        const bool isMatch = t.kind == ww::data::TransitionKind::Lodestone && t.isGlobalOrigin
                          && t.destX == destX && t.destY == destY && t.destPlane == 0
                          && sameRequirements(t.requirements, requirements)
                          && sameChain(t.chain, chain);
        if (isMatch)
        {
            return 0;
        }
        printTransition(label, t);
        return fail(label);
    }

    // A destination with a route yields the route and the map, in that order.
    // The route keeps the destination's unlock gate and adds its own; the map
    // keeps the unlock gate and nothing else — it is the fallback, so it must
    // never be gated against a route.
    int checkRoutedDestination(const std::filesystem::path &dir)
    {
        const ww::data::LoadedDatasets loaded = loadFixture(dir, kRoutedFixture);
        const std::vector<Transition> &txs = loaded.model.transitions;
        std::printf("lodestones: routed fixture -> %zu transitions (expect 3)\n", txs.size());
        if (txs.size() != 3)
        {
            return fail("routed: expected Lumbridge book + map and Edgeville map");
        }
        int failures = 0;
        failures += expectLodestone("routed: Lumbridge book", txs[0], kLumbridgeX, kLumbridgeY,
                                    {kLumbridgeUnlocked, kSpellsShown},
                                    {kCastLumbridge, kTeleportWait});
        failures += expectLodestone("routed: Lumbridge map (must stay ungated)", txs[1],
                                    kLumbridgeX, kLumbridgeY, {kLumbridgeUnlocked},
                                    {kOpenMap, kOpenWait, kPickLumbridge, kTeleportWait});
        failures += expectLodestone("routed: Edgeville map (no routes)", txs[2], kEdgevilleX,
                                    kEdgevilleY, {kEdgevilleUnlocked},
                                    {kOpenMap, kOpenWait, kPickEdgeville, kTeleportWait});

        // A null capability snapshot (ww_query without one) admits both, and
        // the planner then takes the cheaper, so the cast must stay cheaper.
        const float bookCost = ww::data::computeCost(txs[0]);
        const float mapCost = ww::data::computeCost(txs[1]);
        std::printf("lodestones: book cost=%.1f map cost=%.1f (expect book < map)\n",
                    static_cast<double>(bookCost), static_cast<double>(mapCost));
        if (!(bookCost < mapCost))
        {
            failures += fail("routed: the book cast is not cheaper than the map");
        }
        return failures;
    }

    int checkNoRoutesIsMapOnly(const std::filesystem::path &dir)
    {
        const ww::data::LoadedDatasets loaded = loadFixture(dir, kNoRoutesFixture);
        const std::vector<Transition> &txs = loaded.model.transitions;
        std::printf("lodestones: routeless fixture -> %zu transitions (expect 1)\n", txs.size());
        if (txs.size() != 1)
        {
            return fail("routeless: a destination without routes is not the map alone");
        }
        return expectLodestone("routeless: Lumbridge map", txs[0], kLumbridgeX, kLumbridgeY,
                               {kLumbridgeUnlocked},
                               {kOpenMap, kOpenWait, kPickLumbridge, kTeleportWait});
    }

    // The array spelling of a var gate: both entries land, in order, after the
    // destination's own gate.
    int checkArrayVarGate(const std::filesystem::path &dir)
    {
        const ww::data::LoadedDatasets loaded = loadFixture(dir, kArrayGateFixture);
        const std::vector<Transition> &txs = loaded.model.transitions;
        std::printf("lodestones: array-gate fixture -> %zu transitions (expect 2)\n", txs.size());
        if (txs.size() != 2)
        {
            return fail("array gate: expected a book route and a map");
        }
        return expectLodestone("array gate: Lumbridge book", txs[0], kLumbridgeX, kLumbridgeY,
                               {kLumbridgeUnlocked, kSpellsShown, kCombinedBook},
                               {kCastLumbridge, kTeleportWait});
    }

    // Staging happens outside the try: writeText throws too, and a read-only
    // temp directory used to satisfy every one of these without the loader
    // ever running.
    int expectLoadThrows(const std::filesystem::path &dir, const char *label,
                         const char *fixture)
    {
        stageFixture(dir, fixture);
        try
        {
            static_cast<void>(ww::data::loadGlobalTeleports(dir.string()));
        }
        catch (const std::exception &e)
        {
            std::printf("lodestones: %s threw as expected: %s\n", label, e.what());
            return 0;
        }
        return fail(label);
    }

    // Transition index of `plan`'s leading step, or -1 when it does not lead
    // with a transition.
    int64_t leadTransition(const ww::runtime::Plan &plan)
    {
        if (plan.steps.empty() || plan.steps.front().kind != ww::runtime::StepKind::Transition)
        {
            return -1;
        }
        return static_cast<int64_t>(plan.steps.front().transitionIndex);
    }

    bool planUses(const ww::runtime::Plan &plan, uint32_t transitionIndex)
    {
        for (const ww::runtime::Step &s : plan.steps)
        {
            if (s.kind == ww::runtime::StepKind::Transition && s.transitionIndex == transitionIndex)
            {
                return true;
            }
        }
        return false;
    }

    struct PlannerQuery
    {
        int32_t startX{};
        int32_t startY{};
        int32_t goalX{};
        int32_t goalY{};
        int32_t goalPlane{};
    };

    // Plan the query with only the filter varbit set, and require the plan to
    // lead with `wantIndex` and never touch `otherIndex`.
    int expectLead(ww::runtime::PathAssembler &assembler, const PlannerQuery &q,
                   int32_t filterValue, uint32_t wantIndex, uint32_t otherIndex,
                   const char *label)
    {
        ww::runtime::CapabilitySnapshot snapshot;
        snapshot.setVarbit(kFilterVarbit, filterValue);
        ww::runtime::Plan plan;
        const bool isPlanned = assembler.assemble(q.startX, q.startY, kStartPlane, q.goalX,
                                                  q.goalY, q.goalPlane, &snapshot, plan);
        const int64_t lead = isPlanned ? leadTransition(plan) : -1;
        std::printf("lodestones: planner filter=%d planned=%d lead=tx%lld (expect tx%u)"
                    " cost=%.1f steps=%zu\n",
                    filterValue, isPlanned ? 1 : 0, static_cast<long long>(lead), wantIndex,
                    static_cast<double>(plan.cost), plan.steps.size());
        if (!isPlanned || lead != static_cast<int64_t>(wantIndex) || planUses(plan, otherIndex))
        {
            return fail(label);
        }
        return 0;
    }

    // In combat the game refuses every teleport, so the same query that casts
    // from the book out of combat must plan neither lodestone record.
    int expectNoTeleportInCombat(ww::runtime::PathAssembler &assembler, const PlannerQuery &q,
                                 uint32_t routeIndex, uint32_t mapIndex)
    {
        ww::runtime::CapabilitySnapshot snapshot;
        snapshot.setVarbit(kFilterVarbit, 0);
        snapshot.setVarbit(ww::runtime::kInCombatVarbitId, 1);
        ww::runtime::Plan plan;
        const bool isPlanned = assembler.assemble(q.startX, q.startY, kStartPlane, q.goalX,
                                                  q.goalY, q.goalPlane, &snapshot, plan);
        const bool isTeleporting = planUses(plan, routeIndex) || planUses(plan, mapIndex);
        std::printf("lodestones: planner in combat planned=%d teleports=%d (expect 0)"
                    " cost=%.1f steps=%zu\n",
                    isPlanned ? 1 : 0, isTeleporting ? 1 : 0, static_cast<double>(plan.cost),
                    plan.steps.size());
        if (isTeleporting)
        {
            return fail("planner: in combat still planned a lodestone");
        }
        return 0;
    }

    // The appended route record is what the executor will run: its chain must
    // still be the book cast once encoded into the reader's pools.
    int expectAppendedCast(const ww::format::ArtifactReader &reader, uint32_t routeIndex)
    {
        const ww::format::TransitionRecord &tx = reader.transitions()[routeIndex];
        const auto chain = reader.chainSteps();
        const bool isCast = tx.chainCount == 2
                         && chain[tx.chainStart].kind
                                == static_cast<uint8_t>(ChainStepKind::Click)
                         && chain[tx.chainStart].a == kCastLumbridge.a
                         && chain[tx.chainStart].b == kCastLumbridge.b
                         && chain[tx.chainStart].c == kCastLumbridge.c
                         && chain[tx.chainStart].d == kCastLumbridge.d;
        return isCast ? 0 : fail("planner: the appended route record is not the book cast");
    }

    int runPlannerChecks(ww::format::ArtifactReader &reader, const std::filesystem::path &dir)
    {
        stageFixture(dir, kPlannerFixture);
        const std::size_t appended = ww::runtime::loadGlobalTeleportsInto(reader, dir.string());
        if (appended != 2)
        {
            return fail("planner: the fixture did not append exactly a route + map pair");
        }
        const auto txs = reader.transitions();
        const uint32_t routeIndex = static_cast<uint32_t>(txs.size() - 2);
        const uint32_t mapIndex = routeIndex + 1;
        int failures = expectAppendedCast(reader, routeIndex);

        ww::runtime::WorldView view(reader);
        const ww::format::TransitionRecord &route = txs[routeIndex];
        const int32_t goalPlane = static_cast<int32_t>(route.destPlane);
        PlannerQuery q{0, 0, route.destX, route.destY, goalPlane};
        const auto standable = [&](int32_t x, int32_t y)
        {
            return view.isStandable(x, y, kStartPlane);
        };
        if (view.areaAt(route.destX, route.destY, goalPlane) < 0
            || !ww::runtime::findNearestTile(kStartX, kStartY, kStartSnapRadius, true, standable,
                                             kStartX, kStartY, q.startX, q.startY))
        {
            std::printf("lodestones: planner SKIP  artifact has no Lumbridge landing or"
                        " Varrock start\n");
            return failures;
        }
        ww::runtime::AreaSearch areaSearch(reader);
        ww::runtime::TileSearch tileSearch(view);
        ww::runtime::PathAssembler assembler(reader, view, areaSearch, tileSearch);
        failures += expectLead(assembler, q, 0, routeIndex, mapIndex,
                               "planner: filter off did not cast from the book");
        failures += expectLead(assembler, q, 1, mapIndex, routeIndex,
                               "planner: filter on did not open the map");
        // The reason the map carries no route gate: a filter value neither
        // route describes must still leave the destination reachable, not drop
        // it out of the graph.
        failures += expectLead(assembler, q, 7, mapIndex, routeIndex,
                               "planner: an unexpected filter value lost the lodestone");
        failures += expectNoTeleportInCombat(assembler, q, routeIndex, mapIndex);
        return failures;
    }

    int checkPlanner(const char *artifactPath, const std::filesystem::path &dir)
    {
        if (artifactPath == nullptr)
        {
            std::printf("lodestones: planner SKIP  pass an artifact to plan against\n");
            return 0;
        }
        try
        {
            ww::format::ArtifactReader reader(artifactPath);
            return runPlannerChecks(reader, dir);
        }
        catch (const std::exception &e)
        {
            std::printf("  FAIL: planner threw: %s\n", e.what());
            return 1;
        }
    }

    int runLoaderChecks(const std::filesystem::path &dir)
    {
        try
        {
            int failures = checkRoutedDestination(dir);
            failures += checkNoRoutesIsMapOnly(dir);
            failures += checkArrayVarGate(dir);
            failures += expectLoadThrows(dir, "routes as an object", kRoutesNotArrayFixture);
            failures += expectLoadThrows(dir, "a route with no recognised step",
                                         kRouteWithoutChainFixture);
            failures += expectLoadThrows(dir, "a scalar varbit gate", kScalarVarbitFixture);
            return failures;
        }
        catch (const std::exception &e)
        {
            std::printf("  FAIL: loader threw: %s\n", e.what());
            return 1;
        }
    }
}

int runLodestoneTests(const char *artifactPath)
{
    std::printf("lodestones: dataset routes vs the lodestone map\n");
    const std::filesystem::path dir = fixtureDir();
    std::error_code ignored;
    std::filesystem::create_directories(dir, ignored);

    int failures = runLoaderChecks(dir);
    failures += checkPlanner(artifactPath, dir);

    std::filesystem::remove_all(dir, ignored);
    if (failures == 0)
    {
        std::printf("lodestones: all checks passed\n");
        return 0;
    }
    std::printf("lodestones: %d check(s) FAILED\n", failures);
    return 1;
}
