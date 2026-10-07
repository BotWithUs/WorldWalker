#include "cli/LodestoneTests.h"

#include "c_api/worldwalker_c.h"
#include "data/DatasetLoader.h"
#include "data/TransitionCost.h"
#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "runtime/AreaSearch.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/InstanceMap.h"
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
    // (1465:34), a pick on the lodestone map (1092:<component>), and the cast
    // from the Magic ability book's abilities layer (1461:1, sub = spell slot).
    constexpr ChainStep kOpenMap{ChainStepKind::Click, kComponentAction, 1, -1, (1465 << 16) | 34};
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
          "open_interface": 1465, "open_component": 34,
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
          "open_interface": 1465, "open_component": 34,
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
          "open_interface": 1465, "open_component": 34,
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

    // The minimum-value gates: `varbit_at_least` in its array spelling (the
    // second entry leaves `value` out, which means 1) and `varp_at_least` as
    // one object. The ids are the spirit tree ones, but the loader gives them
    // no meaning.
    const char *const kAtLeastGateFixture = R"({
      "lodestones": {
        "config": {
          "open_interface": 1465, "open_component": 34,
          "select_interface": 1092,
          "open_wait": 6, "teleport_wait": 18
        },
        "destinations": [
          { "name": "Lumbridge", "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "varbit": { "id": 35, "value": 1 } },
            "routes": [
              { "requirements": {
                  "varbit_at_least": [ { "id": 10479, "value": 3 }, { "id": 14042 } ],
                  "varp_at_least": { "id": 2661, "value": 9 } },
                "chain": [ { "click": [1461, 1, 1, 234] }, { "wait": 18 } ] }
            ] }
        ]
      }
    })";

    constexpr Requirement kPoisonWasteTree{RequirementKind::VarbitAtLeast, 10479, 3};
    constexpr Requirement kFirstResortDone{RequirementKind::VarbitAtLeast, 14042, 1};
    constexpr Requirement kTreeGnomeVillage{RequirementKind::VarpAtLeast, 2661, 9};

    // A bit gate: the key ring's stored-keys varp, bit 0 (the Brass key).
    // The ids are the real ones, but the loader gives them no meaning.
    const char *const kVarpBitGateFixture = R"({
      "lodestones": {
        "config": {
          "open_interface": 1465, "open_component": 34,
          "select_interface": 1092,
          "open_wait": 6, "teleport_wait": 18
        },
        "destinations": [
          { "name": "Lumbridge", "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "varbit": { "id": 35, "value": 1 } },
            "routes": [
              { "requirements": { "varp_bit": { "id": 2673, "bit": 0 } },
                "chain": [ { "click": [1461, 1, 1, 234] }, { "wait": 18 } ] }
            ] }
        ]
      }
    })";

    constexpr Requirement kBrassKeyOnRing{RequirementKind::VarpBit, 2673, 0};

    // A bit past the top of a 32-bit varp: must throw.
    const char *const kVarpBitOutOfRangeFixture = R"({
      "lodestones": {
        "config": { "open_interface": 1465, "open_component": 34, "select_interface": 1092 },
        "destinations": [
          { "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "varp_bit": { "id": 2673, "bit": 32 } } }
        ]
      }
    })";

    // A scalar where a minimum-value gate belongs: must throw.
    const char *const kScalarVarpAtLeastFixture = R"({
      "lodestones": {
        "config": { "open_interface": 1465, "open_component": 34, "select_interface": 1092 },
        "destinations": [
          { "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "varp_at_least": 2661 } }
        ]
      }
    })";

    // Two skills on one route, the way the Warriors' Guild door wants Attack
    // and Strength. The array used to be dropped and the route baked ungated.
    const char *const kSkillArrayFixture = R"({
      "lodestones": {
        "config": {
          "open_interface": 1465, "open_component": 34,
          "select_interface": 1092,
          "open_wait": 6, "teleport_wait": 18
        },
        "destinations": [
          { "name": "Lumbridge", "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "varbit": { "id": 35, "value": 1 } },
            "routes": [
              { "requirements": { "skill": [ { "id": 0, "level": 65 },
                                             { "id": 2, "level": 65 } ] },
                "chain": [ { "click": [1461, 1, 1, 234] }, { "wait": 18 } ] }
            ] }
        ]
      }
    })";

    constexpr Requirement kAttack65{RequirementKind::Skill, 0, 65};
    constexpr Requirement kStrength65{RequirementKind::Skill, 2, 65};

    // A bare level where a skill gate belongs: must throw.
    const char *const kScalarSkillFixture = R"({
      "lodestones": {
        "config": { "open_interface": 1465, "open_component": 34, "select_interface": 1092 },
        "destinations": [
          { "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "skill": 40 } }
        ]
      }
    })";

    // `routes` as an object rather than an array: must throw, not be ignored.
    const char *const kRoutesNotArrayFixture = R"({
      "lodestones": {
        "config": { "open_interface": 1465, "open_component": 34, "select_interface": 1092 },
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
        "config": { "open_interface": 1465, "open_component": 34, "select_interface": 1092 },
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
        "config": { "open_interface": 1465, "open_component": 34, "select_interface": 1092 },
        "destinations": [
          { "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "varbit": 35 } }
        ]
      }
    })";

    // The three shipped rows that carry an `extra` gate, copied from
    // item_teleports.json (destinations 15, 23 and 26) with their hotkeys left
    // out. Until the loader read `extra`, each loaded gated on its unlock
    // varbit alone.
    const char *const kExtraGateFixture = R"({
      "lodestones": {
        "config": {
          "open_interface": 1465, "open_component": 34,
          "select_interface": 1092,
          "open_wait": 6, "teleport_wait": 18
        },
        "destinations": [
          { "name": "Menaphos", "x": 3216, "y": 2717, "plane": 0, "component": 23,
            "requirements": { "varbit": { "id": 36173, "value": 1 },
                              "extra": { "varbit_id": 36140, "min_value": 100 } },
            "routes": [ { "name": "ability book",
                          "requirements": { "varbit": { "id": 50990, "value": 0 } },
                          "chain": [ { "click": [1461, 1, 1, 236] }, { "wait": 18 } ] } ] },
          { "name": "Tirannwn", "x": 2254, "y": 3150, "plane": 0, "component": 31,
            "requirements": { "varbit": { "id": 18528, "value": 1 },
                              "extra": { "varplayer_id": 2102, "min_value": 15 } },
            "routes": [ { "name": "ability book",
                          "requirements": { "varbit": { "id": 50990, "value": 0 } },
                          "chain": [ { "click": [1461, 1, 1, 242] }, { "wait": 18 } ] } ] },
          { "name": "Prifddinas", "x": 2208, "y": 3361, "plane": 1, "component": 34,
            "requirements": { "varbit": { "id": 24967, "value": 1 },
                              "extra": { "varbit_id": 23198, "min_value": 400 } },
            "routes": [ { "name": "ability book",
                          "requirements": { "varbit": { "id": 50990, "value": 0 } },
                          "chain": [ { "click": [1461, 1, 1, 239] }, { "wait": 18 } ] } ] }
        ]
      }
    })";

    struct ExtraGateRow
    {
        const char *name;
        int32_t x;
        int32_t y;
        Requirement unlock;
        Requirement extra;
    };

    constexpr ExtraGateRow kExtraGateRows[] = {
        {"Menaphos", 3216, 2717, {RequirementKind::Varbit, 36173, 1},
         {RequirementKind::VarbitAtLeast, 36140, 100}},
        {"Tirannwn", 2254, 3150, {RequirementKind::Varbit, 18528, 1},
         {RequirementKind::VarpAtLeast, 2102, 15}},
        {"Prifddinas", 2208, 3361, {RequirementKind::Varbit, 24967, 1},
         {RequirementKind::VarbitAtLeast, 23198, 400}},
    };

    // Malformed requirements, each of which must fail a load under Throw.
    const char *const kUnknownRequirementKeyFixture = R"({
      "lodestones": {
        "config": { "open_interface": 1465, "open_component": 34, "select_interface": 1092 },
        "destinations": [
          { "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "quest": { "id": 7, "value": 1 } } }
        ]
      }
    })";

    const char *const kUnknownExtraKeyFixture = R"({
      "lodestones": {
        "config": { "open_interface": 1465, "open_component": 34, "select_interface": 1092 },
        "destinations": [
          { "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "extra": { "varclient_id": 5, "min_value": 1 } } }
        ]
      }
    })";

    const char *const kExtraWithoutMinimumFixture = R"({
      "lodestones": {
        "config": { "open_interface": 1465, "open_component": 34, "select_interface": 1092 },
        "destinations": [
          { "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "extra": { "varbit_id": 36140 } } }
        ]
      }
    })";

    const char *const kExtraWithBothIdsFixture = R"({
      "lodestones": {
        "config": { "open_interface": 1465, "open_component": 34, "select_interface": 1092 },
        "destinations": [
          { "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "extra": { "varbit_id": 36140, "varplayer_id": 2102,
                                         "min_value": 1 } } }
        ]
      }
    })";

    const char *const kItemsAsObjectFixture = R"({
      "lodestones": {
        "config": { "open_interface": 1465, "open_component": 34, "select_interface": 1092 },
        "destinations": [
          { "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "items": { "id": 995, "count": 1 } } }
        ]
      }
    })";

    // A typo inside a gate object: loaded as `value` 0 (an exact match on 0)
    // before gate keys were checked.
    const char *const kGateKeyTypoFixture = R"({
      "lodestones": {
        "config": { "open_interface": 1465, "open_component": 34, "select_interface": 1092 },
        "destinations": [
          { "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "varbit": { "id": 35, "vaule": 1 } } }
        ]
      }
    })";

    // One good destination whose route has an unknown key, and one destination
    // with an unknown key of its own. Under ExcludeRow only the route and the
    // second destination go; under Throw the load fails.
    const char *const kOneBadRowFixture = R"({
      "lodestones": {
        "config": {
          "open_interface": 1465, "open_component": 34,
          "select_interface": 1092,
          "open_wait": 6, "teleport_wait": 18
        },
        "destinations": [
          { "name": "Lumbridge", "x": 3233, "y": 3222, "plane": 0, "component": 17,
            "requirements": { "varbit": { "id": 35, "value": 1 } },
            "routes": [ { "requirements": { "varbitt": { "id": 50990, "value": 0 } },
                          "chain": [ { "click": [1461, 1, 1, 234] }, { "wait": 18 } ] } ] },
          { "name": "Edgeville", "x": 3067, "y": 3506, "plane": 0, "component": 15,
            "requirements": { "quest": { "id": 7, "value": 1 } } }
        ]
      }
    })";

    // The planner fixture for an `extra` gate: Menaphos alone, map only, gated
    // as the shipped row is.
    const char *const kExtraPlannerFixture = R"({
      "lodestones": {
        "config": {
          "open_interface": 1465, "open_component": 34,
          "select_interface": 1092,
          "open_wait": 6, "teleport_wait": 18
        },
        "destinations": [
          { "name": "Menaphos", "x": 3216, "y": 2717, "plane": 0, "component": 23,
            "requirements": { "varbit": { "id": 36173, "value": 1 },
                              "extra": { "varbit_id": 36140, "min_value": 100 } } }
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
          "open_interface": 1465, "open_component": 34,
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

    // A varp holding `value` against the Tree Gnome Village gate, which asks
    // for at least 9.
    bool meetsTreeGnomeVillage(int32_t value)
    {
        ww::runtime::CapabilitySnapshot snapshot;
        snapshot.setVarp(kTreeGnomeVillage.id, value);
        const ww::format::RequirementRecord gate{
            static_cast<uint8_t>(kTreeGnomeVillage.kind), {}, kTreeGnomeVillage.id,
            kTreeGnomeVillage.amount};
        return snapshot.meets(gate);
    }

    // The minimum-value gates load in order, after the destination's own,
    // and a varp gate passes at and above its value but not below it.
    int checkAtLeastGates(const std::filesystem::path &dir)
    {
        const ww::data::LoadedDatasets loaded = loadFixture(dir, kAtLeastGateFixture);
        const std::vector<Transition> &txs = loaded.model.transitions;
        std::printf("lodestones: at-least fixture -> %zu transitions (expect 2)\n", txs.size());
        if (txs.size() != 2)
        {
            return fail("at-least gate: expected a book route and a map");
        }
        int failures = expectLodestone(
            "at-least gate: Lumbridge book", txs[0], kLumbridgeX, kLumbridgeY,
            {kLumbridgeUnlocked, kPoisonWasteTree, kFirstResortDone, kTreeGnomeVillage},
            {kCastLumbridge, kTeleportWait});
        const bool isBelowDenied = !meetsTreeGnomeVillage(8);
        const bool isAtAdmitted = meetsTreeGnomeVillage(9);
        const bool isAboveAdmitted = meetsTreeGnomeVillage(12);
        std::printf("lodestones: varp_at_least 9 vs 8/9/12 -> %d/%d/%d (expect 0/1/1)\n",
                    isBelowDenied ? 0 : 1, isAtAdmitted ? 1 : 0, isAboveAdmitted ? 1 : 0);
        if (!isBelowDenied || !isAtAdmitted || !isAboveAdmitted)
        {
            failures += fail("at-least gate: varp_at_least is not a minimum");
        }
        return failures;
    }

    // A varp holding `value` against the Brass-key-on-the-ring gate, bit 0.
    bool meetsBrassKeyOnRing(int32_t value)
    {
        ww::runtime::CapabilitySnapshot snapshot;
        snapshot.setVarp(kBrassKeyOnRing.id, value);
        const ww::format::RequirementRecord gate{
            static_cast<uint8_t>(kBrassKeyOnRing.kind), {}, kBrassKeyOnRing.id,
            kBrassKeyOnRing.amount};
        return snapshot.meets(gate);
    }

    // A bit gate loads as one VarpBit after the destination's own gate, and
    // passes exactly when its bit is set, whatever the other bits hold.
    int checkVarpBitGate(const std::filesystem::path &dir)
    {
        const ww::data::LoadedDatasets loaded = loadFixture(dir, kVarpBitGateFixture);
        const std::vector<Transition> &txs = loaded.model.transitions;
        std::printf("lodestones: varp-bit fixture -> %zu transitions (expect 2)\n", txs.size());
        if (txs.size() != 2)
        {
            return fail("varp bit gate: expected a book route and a map");
        }
        int failures = expectLodestone("varp bit gate: Lumbridge book", txs[0], kLumbridgeX,
                                       kLumbridgeY, {kLumbridgeUnlocked, kBrassKeyOnRing},
                                       {kCastLumbridge, kTeleportWait});
        const bool isClearDenied = !meetsBrassKeyOnRing(0);
        const bool isSetAdmitted = meetsBrassKeyOnRing(1);
        const bool isOtherBitDenied = !meetsBrassKeyOnRing(2);
        const bool isAmongOthersAdmitted = meetsBrassKeyOnRing(-1);
        std::printf("lodestones: varp_bit 0 vs 0/1/2/-1 -> %d/%d/%d/%d (expect 0/1/0/1)\n",
                    isClearDenied ? 0 : 1, isSetAdmitted ? 1 : 0, isOtherBitDenied ? 0 : 1,
                    isAmongOthersAdmitted ? 1 : 0);
        if (!isClearDenied || !isSetAdmitted || !isOtherBitDenied || !isAmongOthersAdmitted)
        {
            failures += fail("varp bit gate: does not test exactly its bit");
        }
        return failures;
    }

    // A skill array loads as one Skill per element, after the destination's
    // own gate, and both must pass.
    int checkSkillArrayGate(const std::filesystem::path &dir)
    {
        const ww::data::LoadedDatasets loaded = loadFixture(dir, kSkillArrayFixture);
        const std::vector<Transition> &txs = loaded.model.transitions;
        std::printf("lodestones: skill-array fixture -> %zu transitions (expect 2)\n",
                    txs.size());
        if (txs.size() != 2)
        {
            return fail("skill array: expected a book route and a map");
        }
        return expectLodestone("skill array: Lumbridge book", txs[0], kLumbridgeX, kLumbridgeY,
                               {kLumbridgeUnlocked, kAttack65, kStrength65},
                               {kCastLumbridge, kTeleportWait});
    }

    // `requirements` holds `want` somewhere in its list.
    bool hasRequirement(const std::vector<Requirement> &requirements, const Requirement &want)
    {
        for (const Requirement &r : requirements)
        {
            if (r.kind == want.kind && r.id == want.id && r.amount == want.amount)
            {
                return true;
            }
        }
        return false;
    }

    // Every Lodestone transition landing on `row` carries both its unlock and
    // its `extra` gate, and there are exactly `wantCount` of them.
    int expectExtraGated(const std::vector<Transition> &txs, const ExtraGateRow &row,
                         std::size_t wantCount, const char *source)
    {
        std::size_t landing = 0;
        std::size_t gated = 0;
        for (const Transition &t : txs)
        {
            if (t.kind != ww::data::TransitionKind::Lodestone || t.destX != row.x
                || t.destY != row.y)
            {
                continue;
            }
            ++landing;
            const bool isGated = hasRequirement(t.requirements, row.unlock)
                              && hasRequirement(t.requirements, row.extra);
            gated += isGated ? 1u : 0u;
            if (!isGated)
            {
                printTransition(row.name, t);
            }
        }
        std::printf("lodestones: %s %s -> %zu transitions, %zu carry the extra gate"
                    " (expect %zu/%zu)\n",
                    source, row.name, landing, gated, wantCount, wantCount);
        if (landing != wantCount || gated != wantCount)
        {
            return fail("extra gate: a lodestone loaded without its extra gate");
        }
        return 0;
    }

    // The three shipped `extra` shapes, through a fixture: each destination
    // yields its route and its map, both gated on unlock + extra.
    int checkExtraGates(const std::filesystem::path &dir)
    {
        const ww::data::LoadedDatasets loaded = loadFixture(dir, kExtraGateFixture);
        int failures = 0;
        for (const ExtraGateRow &row : kExtraGateRows)
        {
            failures += expectExtraGated(loaded.model.transitions, row, 2, "fixture");
        }
        return failures;
    }

    // The shipped datasets/ directory, found by walking up from the working
    // directory (wwcli runs from inside the checkout). Empty when no ancestor
    // holds datasets/item_teleports.json.
    std::filesystem::path findShippedDatasets()
    {
        std::error_code ec;
        std::filesystem::path at = std::filesystem::current_path(ec);
        while (!ec && !at.empty())
        {
            const std::filesystem::path candidate = at / "datasets";
            if (std::filesystem::exists(candidate / "item_teleports.json", ec))
            {
                return candidate;
            }
            if (at == at.parent_path())
            {
                break;
            }
            at = at.parent_path();
        }
        return {};
    }

    // The runtime teleport files as they ship, under Throw: the check that a
    // bad row fails here, before it can reach a data.zip, rather than being
    // excluded on a user's machine. The three `extra` lodestones must carry
    // their gates. A missing datasets/ fails rather than skips: a check that
    // quietly does not run is how `extra` went unread.
    int checkShippedTeleports()
    {
        const std::filesystem::path dir = findShippedDatasets();
        if (dir.empty())
        {
            return fail("shipped: datasets/item_teleports.json not found above the working"
                        " directory (run wwcli from inside the checkout)");
        }
        std::printf("lodestones: shipped datasets at %s\n", dir.string().c_str());
        const ww::data::LoadedDatasets loaded = ww::data::loadGlobalTeleports(dir.string());
        std::printf("lodestones: shipped -> %zu transitions from %zu files, %zu excluded"
                    " (expect 0)\n",
                    loaded.model.transitions.size(), loaded.filesFound,
                    loaded.excludedRows.size());
        int failures = loaded.excludedRows.empty() && loaded.filesFound == 2
            ? 0 : fail("shipped: the runtime teleport files did not load whole");
        for (const ExtraGateRow &row : kExtraGateRows)
        {
            failures += expectExtraGated(loaded.model.transitions, row, 2, "shipped");
        }
        return failures;
    }

    // Under ExcludeRow, the runtime policy: the bad route and the bad
    // destination are left out and named, and the rest still loads.
    int checkBadRowExcludedAtRuntime(const std::filesystem::path &dir)
    {
        stageFixture(dir, kOneBadRowFixture);
        const ww::data::LoadedDatasets loaded =
            ww::data::loadGlobalTeleports(dir.string(), ww::data::RowFaultPolicy::ExcludeRow);
        const std::vector<Transition> &txs = loaded.model.transitions;
        const std::vector<ww::data::ExcludedRow> &excluded = loaded.excludedRows;
        std::printf("lodestones: one-bad-row fixture under ExcludeRow -> %zu transitions"
                    " (expect 1), %zu excluded (expect 2)\n",
                    txs.size(), excluded.size());
        for (const ww::data::ExcludedRow &row : excluded)
        {
            std::printf("    excluded %s %s: %s\n", row.file.c_str(), row.row.c_str(),
                        row.reason.c_str());
        }
        int failures = 0;
        if (txs.size() != 1)
        {
            return fail("exclude row: expected the Lumbridge map alone to survive");
        }
        failures += expectLodestone("exclude row: Lumbridge map", txs[0], kLumbridgeX,
                                    kLumbridgeY, {kLumbridgeUnlocked},
                                    {kOpenMap, kOpenWait, kPickLumbridge, kTeleportWait});
        const bool isNamed = excluded.size() == 2
            && excluded[0].file == "item_teleports.json"
            && excluded[0].row == "lodestones.destinations[0].routes[0]"
            && excluded[0].reason.find("'varbitt'") != std::string::npos
            && excluded[1].row == "lodestones.destinations[1]"
            && excluded[1].reason.find("'quest'") != std::string::npos;
        if (!isNamed)
        {
            failures += fail("exclude row: the excluded rows are not named with their key");
        }
        return failures;
    }

    // The exact report ww_artifact_load_teleports publishes for the
    // one-bad-row fixture, spelled out rather than rebuilt from the rows.
    const char *const kOneBadRowReport =
        "ww_artifact_load_teleports: 2 teleport row(s) excluded: "
        "item_teleports.json lodestones.destinations[0].routes[0]: "
        "requirements: unknown key 'varbitt'; "
        "item_teleports.json lodestones.destinations[1]: "
        "requirements: unknown key 'quest'";

    // describeExcludedRows: empty for none, every row for a few, and a
    // bounded line that counts the rest for many.
    int checkExcludedRowReport()
    {
        int failures = 0;
        if (!ww::runtime::describeExcludedRows({}).empty())
        {
            failures += fail("load report: no excluded rows did not describe as empty");
        }
        std::vector<ww::data::ExcludedRow> rows;
        const std::size_t total = ww::runtime::kMaxDescribedExcludedRows + 4;
        for (std::size_t i = 0; i < total; ++i)
        {
            rows.push_back({"item_teleports.json", "teleports[" + std::to_string(i) + "]",
                            "requirements: unknown key 'k'"});
        }
        const std::string many = ww::runtime::describeExcludedRows(rows);
        const std::string lastNamed =
            "teleports[" + std::to_string(ww::runtime::kMaxDescribedExcludedRows - 1) + "]";
        const std::string firstUnnamed =
            "teleports[" + std::to_string(ww::runtime::kMaxDescribedExcludedRows) + "]";
        std::printf("lodestones: report for %zu excluded rows is %zu bytes\n", total,
                    many.size());
        const bool isBounded = many.starts_with(std::to_string(total) + " teleport row(s)")
                            && many.find(lastNamed) != std::string::npos
                            && many.find(firstUnnamed) == std::string::npos
                            && many.ends_with("; ... and 4 more");
        if (!isBounded)
        {
            std::printf("    got: %s\n", many.c_str());
            failures += fail("load report: many excluded rows are not named up to the cap"
                             " and counted after it");
        }
        return failures;
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

    // Violet is Blue leaves the player in an instanced Yeti Town at this tile,
    // far outside any baked square; the only way on is a teleport.
    constexpr int32_t kOffMapX = 10338;
    constexpr int32_t kOffMapY = 1632;

    // A plan from a start the walker has no map for must lead with one of the
    // fixture's lodestone records and end on the goal.
    int expectTeleportOut(ww::runtime::PathAssembler &assembler, const PlannerQuery &q,
                          int32_t startX, int32_t startY, uint32_t routeIndex,
                          uint32_t mapIndex, const char *label)
    {
        ww::runtime::CapabilitySnapshot snapshot;
        snapshot.setVarbit(kFilterVarbit, 0);
        ww::runtime::Plan plan;
        const bool isPlanned = assembler.assemble(startX, startY, kStartPlane, q.goalX, q.goalY,
                                                  q.goalPlane, &snapshot, plan);
        const int64_t lead = isPlanned ? leadTransition(plan) : -1;
        const bool isLodestoneLead = lead == static_cast<int64_t>(routeIndex)
                                  || lead == static_cast<int64_t>(mapIndex);
        std::printf("lodestones: %s from (%d,%d) planned=%d lead=tx%lld (expect tx%u or tx%u)"
                    " steps=%zu cost=%.1f\n",
                    label, startX, startY, isPlanned ? 1 : 0, static_cast<long long>(lead),
                    routeIndex, mapIndex, plan.steps.size(), static_cast<double>(plan.cost));
        return (isPlanned && isLodestoneLead) ? 0 : fail(label);
    }

    // The same unmapped start in combat: nothing can be cast, so no plan.
    int expectNoPlanOffMapInCombat(ww::runtime::PathAssembler &assembler, const PlannerQuery &q)
    {
        ww::runtime::CapabilitySnapshot snapshot;
        snapshot.setVarbit(kFilterVarbit, 0);
        snapshot.setVarbit(ww::runtime::kInCombatVarbitId, 1);
        ww::runtime::Plan plan;
        const bool isPlanned = assembler.assemble(kOffMapX, kOffMapY, kStartPlane, q.goalX,
                                                  q.goalY, q.goalPlane, &snapshot, plan);
        std::printf("lodestones: off-map in combat planned=%d steps=%zu (expect 0, 0)\n",
                    isPlanned ? 1 : 0, plan.steps.size());
        return (!isPlanned && plan.steps.empty())
            ? 0 : fail("planner: an off-map start in combat still produced a plan");
    }

    // An instance whose one chunk is a copy of the Varrock start's chunk, laid
    // at the off-map tile's mapsquare. The goal lies outside it, so the plan
    // must teleport out, and the view must still resolve through the instance
    // afterwards.
    int expectTeleportOutOfInstance(ww::runtime::WorldView &view,
                                    ww::runtime::PathAssembler &assembler, const PlannerQuery &q,
                                    uint32_t routeIndex, uint32_t mapIndex)
    {
        constexpr int32_t kChunkShift = 3;
        constexpr int32_t kChunkMask = 7;
        constexpr int32_t kSquareShift = 6;
        const int32_t squareX = kOffMapX >> kSquareShift;
        const int32_t squareY = kOffMapY >> kSquareShift;
        std::vector<int32_t> grid(4, ww::runtime::InstanceMap::kNoChunk);
        grid[0] = ((q.startX >> kChunkShift) << 14) | ((q.startY >> kChunkShift) << 3);
        ww::runtime::InstanceMap map;
        map.assign(squareX, squareY, 1, 1, grid.data(), grid.size());
        const int32_t startX = (squareX << kSquareShift) + (q.startX & kChunkMask);
        const int32_t startY = (squareY << kSquareShift) + (q.startY & kChunkMask);
        view.setInstance(&map);
        int failures = 0;
        if (!view.isStandable(startX, startY, kStartPlane))
        {
            failures += fail("planner: the instance start tile is not standable");
        }
        failures += expectTeleportOut(assembler, q, startX, startY, routeIndex, mapIndex,
                                      "instance start");
        if (!view.isInstanced() || view.instanceMap() != &map)
        {
            failures += fail("planner: planning out of an instance left the instance removed");
        }
        view.setInstance(nullptr);
        return failures;
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
        failures += expectTeleportOut(assembler, q, kOffMapX, kOffMapY, routeIndex, mapIndex,
                                      "off-map start");
        failures += expectNoPlanOffMapInCombat(assembler, q);
        failures += expectTeleportOutOfInstance(view, assembler, q, routeIndex, mapIndex);
        return failures;
    }

    // Plan Varrock -> Menaphos with the unlock set and the `extra` varbit at
    // `extraValue`. True when the plan uses the appended lodestone.
    bool plansMenaphosLodestone(ww::runtime::PathAssembler &assembler, const PlannerQuery &q,
                                int32_t extraValue, uint32_t lodestoneIndex)
    {
        ww::runtime::CapabilitySnapshot snapshot;
        snapshot.setVarbit(kExtraGateRows[0].unlock.id, 1);
        snapshot.setVarbit(kExtraGateRows[0].extra.id, extraValue);
        ww::runtime::Plan plan;
        const bool isPlanned = assembler.assemble(q.startX, q.startY, kStartPlane, q.goalX,
                                                  q.goalY, q.goalPlane, &snapshot, plan);
        const bool isUsed = isPlanned && planUses(plan, lodestoneIndex);
        std::printf("lodestones: planner extra varbit=%d planned=%d lead=tx%lld uses=%d"
                    " cost=%.1f\n",
                    extraValue, isPlanned ? 1 : 0,
                    static_cast<long long>(isPlanned ? leadTransition(plan) : -1),
                    isUsed ? 1 : 0, static_cast<double>(plan.cost));
        return isUsed;
    }

    // An account below the `extra` minimum must not be routed through the
    // lodestone; one at it must be.
    int runExtraGatePlannerChecks(ww::format::ArtifactReader &reader,
                                  const std::filesystem::path &dir)
    {
        stageFixture(dir, kExtraPlannerFixture);
        if (ww::runtime::loadGlobalTeleportsInto(reader, dir.string()) != 1)
        {
            return fail("planner extra: the fixture did not append exactly one map record");
        }
        const auto txs = reader.transitions();
        const uint32_t lodestoneIndex = static_cast<uint32_t>(txs.size() - 1);
        const ww::format::TransitionRecord &lode = txs[lodestoneIndex];
        ww::runtime::WorldView view(reader);
        PlannerQuery q{0, 0, lode.destX, lode.destY, static_cast<int32_t>(lode.destPlane)};
        const auto standable = [&](int32_t x, int32_t y)
        {
            return view.isStandable(x, y, kStartPlane);
        };
        if (view.areaAt(lode.destX, lode.destY, q.goalPlane) < 0
            || !ww::runtime::findNearestTile(kStartX, kStartY, kStartSnapRadius, true, standable,
                                             kStartX, kStartY, q.startX, q.startY))
        {
            return fail("planner extra: artifact has no Menaphos landing or Varrock start");
        }
        ww::runtime::AreaSearch areaSearch(reader);
        ww::runtime::TileSearch tileSearch(view);
        ww::runtime::PathAssembler assembler(reader, view, areaSearch, tileSearch);
        const int32_t minimum = kExtraGateRows[0].extra.amount;
        int failures = 0;
        if (plansMenaphosLodestone(assembler, q, minimum - 1, lodestoneIndex))
        {
            failures += fail("planner extra: an account below the minimum took the lodestone");
        }
        if (!plansMenaphosLodestone(assembler, q, minimum, lodestoneIndex))
        {
            failures += fail("planner extra: an account at the minimum did not take it");
        }
        return failures;
    }

    // Leave a stale error on this thread, the way an unrelated failed call
    // would, so a load that forgot to reset ww_last_error is caught.
    void primeStaleError()
    {
        static_cast<void>(ww_artifact_open(nullptr));
    }

    // One ww_artifact_load_teleports call on `fixture`: its result and the
    // ww_last_error text it leaves.
    struct LoadReport
    {
        ww_result result{};
        std::string text;
    };

    LoadReport loadThroughCApi(ww_artifact *artifact, const std::filesystem::path &dir,
                               const char *fixture)
    {
        stageFixture(dir, fixture);
        primeStaleError();
        LoadReport report;
        report.result = ww_artifact_load_teleports(artifact, dir.string().c_str());
        report.text = ww_last_error();
        std::printf("lodestones: C ABI load rc=%d last_error=\"%s\"\n",
                    static_cast<int>(report.result), report.text.c_str());
        return report;
    }

    // Through the C ABI: excluded rows come back as WW_OK plus a report naming
    // each one, a clean load as WW_OK plus "", and a malformed file as an
    // error. The excluded rows' transitions are absent from the reader.
    int checkLoadReportThroughCApi(const char *artifactPath, ww::format::ArtifactReader &reader,
                                   const std::filesystem::path &dir)
    {
        ww_artifact *artifact = ww_artifact_open(artifactPath);
        if (artifact == nullptr)
        {
            return fail("load report: ww_artifact_open failed");
        }
        int failures = 0;
        const LoadReport bad = loadThroughCApi(artifact, dir, kOneBadRowFixture);
        if (bad.result != WW_OK || bad.text != kOneBadRowReport)
        {
            std::printf("    want: %s\n", kOneBadRowReport);
            failures += fail("load report: excluded rows did not come back as WW_OK + report");
        }
        const LoadReport clean = loadThroughCApi(artifact, dir, kNoRoutesFixture);
        if (clean.result != WW_OK || !clean.text.empty())
        {
            failures += fail("load report: a clean load left a non-empty ww_last_error");
        }
        const LoadReport broken = loadThroughCApi(artifact, dir, "{ \"lodestones\": ");
        if (broken.result == WW_OK || !broken.text.starts_with("ww_artifact_load_teleports: ")
            || broken.text.find("excluded") != std::string::npos)
        {
            failures += fail("load report: malformed JSON did not fail the load");
        }
        ww_artifact_close(artifact);

        // The same fixture through the runtime seam the C ABI wraps: only the
        // Lumbridge map is appended, and both bad rows are handed back.
        stageFixture(dir, kOneBadRowFixture);
        std::vector<ww::data::ExcludedRow> excluded;
        const std::size_t appended =
            ww::runtime::loadGlobalTeleportsInto(reader, dir.string(), &excluded);
        std::printf("lodestones: runtime load appended=%zu (expect 1) excluded=%zu (expect 2)\n",
                    appended, excluded.size());
        if (appended != 1 || excluded.size() != 2)
        {
            failures += fail("load report: the runtime load kept a bad row or lost a good one");
        }
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
            // Each appends its own fixture; loadGlobalTeleportsInto replaces
            // the previous set, so they do not see each other's records.
            int failures = runPlannerChecks(reader, dir);
            failures += runExtraGatePlannerChecks(reader, dir);
            return failures + checkLoadReportThroughCApi(artifactPath, reader, dir);
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
            failures += checkAtLeastGates(dir);
            failures += checkVarpBitGate(dir);
            failures += checkSkillArrayGate(dir);
            failures += expectLoadThrows(dir, "a scalar skill gate", kScalarSkillFixture);
            failures += expectLoadThrows(dir, "a varp_bit gate past bit 31",
                                         kVarpBitOutOfRangeFixture);
            failures += expectLoadThrows(dir, "routes as an object", kRoutesNotArrayFixture);
            failures += expectLoadThrows(dir, "a route with no recognised step",
                                         kRouteWithoutChainFixture);
            failures += expectLoadThrows(dir, "a scalar varbit gate", kScalarVarbitFixture);
            failures += expectLoadThrows(dir, "a scalar varp_at_least gate",
                                         kScalarVarpAtLeastFixture);
            failures += checkExtraGates(dir);
            failures += checkShippedTeleports();
            failures += checkBadRowExcludedAtRuntime(dir);
            failures += checkExcludedRowReport();
            failures += expectLoadThrows(dir, "the same bad rows under Throw", kOneBadRowFixture);
            failures += expectLoadThrows(dir, "an unknown requirements key",
                                         kUnknownRequirementKeyFixture);
            failures += expectLoadThrows(dir, "an unknown extra key", kUnknownExtraKeyFixture);
            failures += expectLoadThrows(dir, "an extra without min_value",
                                         kExtraWithoutMinimumFixture);
            failures += expectLoadThrows(dir, "an extra naming a varbit and a varp",
                                         kExtraWithBothIdsFixture);
            failures += expectLoadThrows(dir, "items as an object", kItemsAsObjectFixture);
            failures += expectLoadThrows(dir, "a typo inside a gate object", kGateKeyTypoFixture);
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
