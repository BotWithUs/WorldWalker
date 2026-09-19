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
    constexpr Requirement kSpellsFiltered{RequirementKind::Varbit, kFilterVarbit, 1};

    // loadGlobalTeleports reads both files; an empty spell file keeps the
    // loader's "dataset not found" warning out of the report.
    const char *const kEmptySpellFixture = R"({ "teleports": [] })";

    // Book route configured. Lumbridge has a spell slot, Edgeville does not.
    // The option keys are left out so their default of 1 is exercised too.
    const char *const kConfiguredFixture = R"({
      "lodestones": {
        "config": {
          "open_interface": 1465, "open_component": 33,
          "select_interface": 1092,
          "spell_interface": 1461, "spell_component": 1,
          "filter_varbit": 50990,
          "open_wait": 6, "teleport_wait": 18
        },
        "destinations": [
          { "name": "Lumbridge", "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "spell_slot": 234, "requirements": { "varbit": { "id": 35, "value": 1 } } },
          { "name": "Edgeville", "x": 3067, "y": 3506, "plane": 0, "component": 15,
            "requirements": { "varbit": { "id": 33, "value": 1 } } }
        ]
      }
    })";

    // A config from before the book route: no filter_varbit, so a spell_slot
    // on a destination must not conjure a spell transition.
    const char *const kUnconfiguredFixture = R"({
      "lodestones": {
        "config": {
          "open_interface": 1465, "open_component": 33,
          "select_interface": 1092,
          "open_wait": 6, "teleport_wait": 18
        },
        "destinations": [
          { "name": "Lumbridge", "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "spell_slot": 234, "requirements": { "varbit": { "id": 35, "value": 1 } } }
        ]
      }
    })";

    // filter_varbit without the spell's interface: must throw, not cast from
    // interface 0.
    const char *const kMissingSpellInterfaceFixture = R"({
      "lodestones": {
        "config": {
          "open_interface": 1465, "open_component": 33,
          "select_interface": 1092,
          "spell_component": 1,
          "filter_varbit": 50990
        },
        "destinations": [
          { "x": 3233, "y": 3222, "plane": 0, "component": 17, "spell_slot": 234 }
        ]
      }
    })";

    // A negative slot is the click encoding's "no sub-component": must throw.
    const char *const kNegativeSlotFixture = R"({
      "lodestones": {
        "config": {
          "open_interface": 1465, "open_component": 33,
          "select_interface": 1092,
          "spell_interface": 1461, "spell_component": 1,
          "filter_varbit": 50990
        },
        "destinations": [
          { "x": 3233, "y": 3222, "plane": 0, "component": 17, "spell_slot": -1 }
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
          "spell_interface": 1461, "spell_component": 1,
          "filter_varbit": 50990,
          "open_wait": 6, "teleport_wait": 18
        },
        "destinations": [
          { "name": "Lumbridge", "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "spell_slot": 234 }
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

    // Configured book route: a destination with a slot splits into a map
    // transition gated on the filter being on and a spell transition gated on
    // it being off, both keeping the destination's own unlock gate. A
    // destination without a slot keeps the single, ungated map transition.
    int checkConfiguredSplit(const std::filesystem::path &dir)
    {
        const ww::data::LoadedDatasets loaded = loadFixture(dir, kConfiguredFixture);
        const std::vector<Transition> &txs = loaded.model.transitions;
        std::printf("lodestones: configured fixture -> %zu transitions (expect 3)\n",
                    txs.size());
        if (txs.size() != 3)
        {
            return fail("configured: expected Lumbridge map + spell and Edgeville map");
        }
        int failures = 0;
        failures += expectLodestone("configured: Lumbridge map", txs[0], kLumbridgeX, kLumbridgeY,
                                    {kLumbridgeUnlocked, kSpellsFiltered},
                                    {kOpenMap, kOpenWait, kPickLumbridge, kTeleportWait});
        failures += expectLodestone("configured: Lumbridge spell", txs[1], kLumbridgeX,
                                    kLumbridgeY, {kLumbridgeUnlocked, kSpellsShown},
                                    {kCastLumbridge, kTeleportWait});
        failures += expectLodestone("configured: Edgeville map (no slot)", txs[2], kEdgevilleX,
                                    kEdgevilleY, {kEdgevilleUnlocked},
                                    {kOpenMap, kOpenWait, kPickEdgeville, kTeleportWait});

        // A null capability snapshot (ww_query without one) admits both, and
        // the planner then takes the cheaper, so the cast must stay cheaper.
        const float spellCost = ww::data::computeCost(txs[1]);
        const float mapCost = ww::data::computeCost(txs[0]);
        std::printf("lodestones: spell cost=%.1f map cost=%.1f (expect spell < map)\n",
                    static_cast<double>(spellCost), static_cast<double>(mapCost));
        if (!(spellCost < mapCost))
        {
            failures += fail("configured: the book cast is not cheaper than the map");
        }
        return failures;
    }

    int checkUnconfiguredIsMapOnly(const std::filesystem::path &dir)
    {
        const ww::data::LoadedDatasets loaded = loadFixture(dir, kUnconfiguredFixture);
        const std::vector<Transition> &txs = loaded.model.transitions;
        std::printf("lodestones: unconfigured fixture -> %zu transitions (expect 1)\n",
                    txs.size());
        if (txs.size() != 1)
        {
            return fail("unconfigured: a spell_slot without filter_varbit changed the count");
        }
        return expectLodestone("unconfigured: Lumbridge map", txs[0], kLumbridgeX, kLumbridgeY,
                               {kLumbridgeUnlocked},
                               {kOpenMap, kOpenWait, kPickLumbridge, kTeleportWait});
    }

    int expectLoadThrows(const std::filesystem::path &dir, const char *label,
                         const char *fixture)
    {
        try
        {
            static_cast<void>(loadFixture(dir, fixture));
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
                   int32_t filterValue, uint32_t wantIndex, uint32_t otherIndex)
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
            return fail(filterValue == 0 ? "planner: filter off did not cast from the book"
                                         : "planner: filter on did not open the map");
        }
        return 0;
    }

    // The appended spell record is what the executor will run: its chain must
    // still be the book cast once encoded into the reader's pools.
    int expectAppendedSpell(const ww::format::ArtifactReader &reader, uint32_t spellIndex)
    {
        const ww::format::TransitionRecord &tx = reader.transitions()[spellIndex];
        const auto chain = reader.chainSteps();
        const bool isCast = tx.chainCount == 2
                         && chain[tx.chainStart].kind
                                == static_cast<uint8_t>(ChainStepKind::Click)
                         && chain[tx.chainStart].a == kCastLumbridge.a
                         && chain[tx.chainStart].b == kCastLumbridge.b
                         && chain[tx.chainStart].c == kCastLumbridge.c
                         && chain[tx.chainStart].d == kCastLumbridge.d;
        return isCast ? 0 : fail("planner: the appended spell record is not the book cast");
    }

    int runPlannerChecks(ww::format::ArtifactReader &reader, const std::filesystem::path &dir)
    {
        stageFixture(dir, kPlannerFixture);
        const std::size_t appended = ww::runtime::loadGlobalTeleportsInto(reader, dir.string());
        if (appended != 2)
        {
            return fail("planner: the fixture did not append exactly a map + spell pair");
        }
        const auto txs = reader.transitions();
        const uint32_t mapIndex = static_cast<uint32_t>(txs.size() - 2);
        const uint32_t spellIndex = mapIndex + 1;
        int failures = expectAppendedSpell(reader, spellIndex);

        ww::runtime::WorldView view(reader);
        const ww::format::TransitionRecord &spell = txs[spellIndex];
        const int32_t goalPlane = static_cast<int32_t>(spell.destPlane);
        PlannerQuery q{0, 0, spell.destX, spell.destY, goalPlane};
        const auto standable = [&](int32_t x, int32_t y)
        {
            return view.isStandable(x, y, kStartPlane);
        };
        if (view.areaAt(spell.destX, spell.destY, goalPlane) < 0
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
        failures += expectLead(assembler, q, 0, spellIndex, mapIndex);
        failures += expectLead(assembler, q, 1, mapIndex, spellIndex);
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
            int failures = checkConfiguredSplit(dir);
            failures += checkUnconfiguredIsMapOnly(dir);
            failures += expectLoadThrows(dir, "filter_varbit without spell_interface",
                                         kMissingSpellInterfaceFixture);
            failures += expectLoadThrows(dir, "negative spell_slot", kNegativeSlotFixture);
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
    std::printf("lodestones: ability-book vs lodestone-map routes\n");
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "wwcli_lodestone_fixtures";
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
