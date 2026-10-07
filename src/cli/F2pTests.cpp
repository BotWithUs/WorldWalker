#include "cli/F2pTests.h"

#include "cli/F2pPlannerTests.h"

#include "c_api/worldwalker_c.h"
#include "data/DatasetLoader.h"
#include "data/F2pZones.h"
#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "format/F2pLand.h"
#include "format/MoveCategory.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/RuntimeTeleports.h"
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
#include <fstream>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using ww::format::F2pBoxRecord;
    using ww::format::F2pLand;
    using ww::format::LandClass;
    using ww::format::MoveCategory;

    // Cache enum 12260 CHEEVO_LODESTONE_FREE, by the names item_teleports.json
    // uses. The only lodestones a free-to-play plan may seed.
    const std::set<std::string> kFreeLodestones = {
        "Al Kharid", "Ashdale", "Burthorpe", "Draynor Village", "Edgeville", "Falador",
        "Lumbridge", "Port Sarim", "Taverley", "Varrock", "Wilderness Crater",
    };
    // Cache enum 12261 CHEEVO_LODESTONE_MEMBERS.
    const std::set<std::string> kMembersLodestones = {
        "Ardougne", "Bandit Camp", "Canifis", "Catherby", "Eagles' Peak", "Fremennik Province",
        "Karamja", "Lunar Isle", "Oo'glog", "Prifddinas", "Seers' Village", "Tirannwn",
        "Yanille", "Menaphos", "Anachronia",
    };

    int fail(const std::string &message)
    {
        std::printf("f2p: FAIL %s\n", message.c_str());
        return 1;
    }

    int expect(bool isOk, const std::string &message)
    {
        if (isOk)
        {
            return 0;
        }
        return fail(message);
    }

    F2pBoxRecord box(int32_t minX, int32_t minY, int32_t maxX, int32_t maxY, uint8_t planeMin,
                     uint8_t planeMax)
    {
        F2pBoxRecord b{};
        b.minX = minX;
        b.minY = minY;
        b.maxX = maxX;
        b.maxY = maxY;
        b.planeMin = planeMin;
        b.planeMax = planeMax;
        return b;
    }

    std::string tileText(int32_t x, int32_t y, int32_t plane)
    {
        return "(" + std::to_string(x) + "," + std::to_string(y) + ",p" + std::to_string(plane) + ")";
    }

    // ---- Land geometry ------------------------------------------------------

    bool isFreeByBrute(std::span<const F2pBoxRecord> zones, std::span<const F2pBoxRecord> holes,
                       int32_t x, int32_t y, int32_t plane)
    {
        const auto inside = [&](const F2pBoxRecord &b)
        {
            return plane >= b.planeMin && plane <= b.planeMax && x >= b.minX && x <= b.maxX
                && y >= b.minY && y <= b.maxY;
        };
        return std::any_of(zones.begin(), zones.end(), inside)
            && std::none_of(holes.begin(), holes.end(), inside);
    }

    // The per-square index must answer every tile exactly as the boxes do.
    int checkLandAgainstBrute(const F2pLand &land)
    {
        std::mt19937 rng(20261007u);
        std::uniform_int_distribution<int32_t> coord(-8, 400);
        std::uniform_int_distribution<int32_t> plane(0, 3);
        int failures = 0;
        for (int i = 0; i < 20000 && failures < 5; ++i)
        {
            const int32_t x = coord(rng);
            const int32_t y = coord(rng);
            const int32_t p = plane(rng);
            if (land.isFreeToPlay(x, y, p) != isFreeByBrute(land.zones(), land.holes(), x, y, p))
            {
                failures += fail("land: index disagrees with the boxes at " + tileText(x, y, p));
            }
        }
        return failures;
    }

    int checkLandGeometry()
    {
        const std::array<F2pBoxRecord, 2> zones = {box(0, 0, 99, 99, 0, 3),
                                                   box(192, 0, 255, 63, 0, 0)};
        const std::array<F2pBoxRecord, 1> holes = {box(10, 10, 19, 19, 0, 0)};
        const F2pLand land(zones, holes);
        int failures = 0;
        failures += expect(land.isFreeToPlay(5, 5, 0), "land: (5,5,0) should be free");
        failures += expect(!land.isFreeToPlay(15, 15, 0), "land: the hole should be members");
        failures += expect(land.isFreeToPlay(15, 15, 1), "land: the hole is plane 0 only");
        failures += expect(!land.isFreeToPlay(150, 50, 0), "land: between zones is members");
        failures += expect(land.isFreeToPlay(255, 63, 0), "land: a zone's max corner is inside");
        failures += expect(!land.isFreeToPlay(256, 63, 0), "land: one past the max is outside");
        failures += expect(!land.isFreeToPlay(192, 0, 1), "land: a plane-0 zone is not plane 1");
        failures += expect(!land.isFreeToPlay(-1, 5, 0), "land: a negative tile is members");
        failures += expect(land.classify(192, 0, 255, 63, 0) == LandClass::FreeToPlay,
                           "land: a square one zone covers is FreeToPlay");
        failures += expect(land.classify(0, 0, 63, 63, 0) == LandClass::Mixed,
                           "land: a square holding a hole is Mixed");
        failures += expect(land.classify(20, 20, 30, 30, 1) == LandClass::FreeToPlay,
                           "land: plane 1 has no hole");
        failures += expect(land.classify(128, 128, 191, 191, 0) == LandClass::Members,
                           "land: a square no zone touches is Members");
        failures += expect(land.classify(64, 0, 127, 63, 1) == LandClass::Mixed,
                           "land: a square a zone edge crosses is Mixed");
        failures += checkLandAgainstBrute(land);
        const F2pLand none;
        failures += expect(!none.isFreeToPlay(5, 5, 0), "land: no zones means no free land");
        std::printf("f2p: land geometry %s\n", failures == 0 ? "PASS" : "FAIL");
        return failures;
    }

    // ---- The members rule ---------------------------------------------------

    ww::format::TransitionRecord transition(int32_t ox, int32_t oy, int32_t dx, int32_t dy,
                                            bool isGlobal)
    {
        ww::format::TransitionRecord t{};
        t.flags = isGlobal ? ww::format::kTransitionFlagGlobalOrigin : 0u;
        t.originX = ox;
        t.originY = oy;
        t.destX = dx;
        t.destY = dy;
        return t;
    }

    int checkMembersRule()
    {
        const std::array<F2pBoxRecord, 1> zones = {box(0, 0, 99, 99, 0, 3)};
        const F2pLand land(zones, {});
        using ww::format::isMembersOnly;
        const auto inside = transition(10, 10, 20, 20, false);
        const auto toOutside = transition(10, 10, 500, 20, false);
        const auto fromOutside = transition(500, 10, 20, 20, false);
        const auto globalIn = transition(500, 500, 20, 20, true);
        int failures = 0;
        failures += expect(!isMembersOnly(inside, MoveCategory::Transports, -1, &land),
                           "rule: free land both ends is free");
        failures += expect(isMembersOnly(inside, MoveCategory::Charters, -1, &land),
                           "rule: a charter is members anywhere");
        failures += expect(isMembersOnly(inside, MoveCategory::FairyRings, -1, nullptr),
                           "rule: a fairy ring is members with no land");
        failures += expect(!isMembersOnly(inside, MoveCategory::Charters, 0, &land),
                           "rule: \"members\": false beats the network rule");
        failures += expect(isMembersOnly(inside, MoveCategory::Doors, 1, &land),
                           "rule: \"members\": true beats geography");
        failures += expect(isMembersOnly(toOutside, MoveCategory::Doors, -1, &land),
                           "rule: a members destination is members");
        failures += expect(isMembersOnly(fromOutside, MoveCategory::Doors, -1, &land),
                           "rule: a members origin is members");
        failures += expect(!isMembersOnly(globalIn, MoveCategory::Teleports, -1, &land),
                           "rule: a global teleport's origin is not tested");
        failures += expect(!isMembersOnly(toOutside, MoveCategory::Doors, -1, nullptr),
                           "rule: no land, no geography");
        std::printf("f2p: members rule %s\n", failures == 0 ? "PASS" : "FAIL");
        return failures;
    }

    // ---- The bit in a snapshot ----------------------------------------------

    int checkSnapshotBit()
    {
        const std::array<uint8_t, 3> categories = {
            static_cast<uint8_t>(MoveCategory::Transports),
            static_cast<uint8_t>(MoveCategory::Charters),
            static_cast<uint8_t>(MoveCategory::Doors)};
        const std::array<uint8_t, 3> membersOnly = {0u, 1u, 1u};
        ww::runtime::CapabilitySnapshot s;
        int failures = 0;
        s.disableMoves(0u, categories, membersOnly);
        failures += expect(!s.isTransitionExcluded(1) && !s.isRestrictedToFreeToPlay(),
                           "snapshot: mask 0 refuses nothing");
        s.disableMoves(ww::format::kRestrictFreeToPlay, categories, membersOnly);
        failures += expect(s.isRestrictedToFreeToPlay() && !s.isTransitionExcluded(0)
                               && s.isTransitionExcluded(1) && s.isTransitionExcluded(2),
                           "snapshot: the bit refuses exactly the members-only transitions");
        s.disableMoves(1u << static_cast<uint32_t>(MoveCategory::Doors), categories, membersOnly);
        failures += expect(!s.isTransitionExcluded(1) && s.isTransitionExcluded(2),
                           "snapshot: a category bit alone is unchanged by membersOnly");
        failures += expect(WW_RESTRICT_FREE_TO_PLAY == ww::format::kRestrictFreeToPlay
                               && (WW_RESTRICT_FREE_TO_PLAY >> WW_MOVE_COUNT) != 0u,
                           "snapshot: the bit sits clear of every WW_MOVE_* category");
        std::printf("f2p: snapshot bit %s\n", failures == 0 ? "PASS" : "FAIL");
        return failures;
    }

    // ---- The loader -----------------------------------------------------------

    void writeFile(const std::filesystem::path &path, std::string_view text)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    constexpr std::string_view kSpellFixture = R"({"teleports": [
      {"name": "a", "members": true,  "dest_x": 1, "dest_y": 1, "dest_plane": 0, "chain": [{"wait": 1}]},
      {"name": "b", "members": false, "dest_x": 2, "dest_y": 2, "dest_plane": 0, "chain": [{"wait": 1}]},
      {"name": "c",                   "dest_x": 3, "dest_y": 3, "dest_plane": 0, "chain": [{"wait": 1}]},
      {"name": "d", "members": "yes", "dest_x": 4, "dest_y": 4, "dest_plane": 0, "chain": [{"wait": 1}]}
    ]})";

    constexpr std::string_view kLodestoneFixture = R"({"lodestones": {
      "config": {"open_interface": 1465, "open_component": 34, "select_interface": 1092},
      "destinations": [{"name": "L", "members": true, "x": 5, "y": 5, "plane": 0, "component": 9,
                        "routes": [{"chain": [{"wait": 1}]}]}]}})";

    int checkLoaderFlags(const std::filesystem::path &dir)
    {
        writeFile(dir / "spell_teleports.json", kSpellFixture);
        writeFile(dir / "item_teleports.json", kLodestoneFixture);
        int failures = 0;
        bool isThrown = false;
        try
        {
            (void)ww::data::loadGlobalTeleports(dir.string(), ww::data::RowFaultPolicy::Throw);
        }
        catch (const std::exception &)
        {
            isThrown = true;
        }
        failures += expect(isThrown, "loader: a non-boolean \"members\" fails a Throw load");
        const ww::data::LoadedDatasets loaded =
            ww::data::loadGlobalTeleports(dir.string(), ww::data::RowFaultPolicy::ExcludeRow);
        const auto &t = loaded.model.transitions;
        // Spells a, b, c, then the lodestone's route and its map chain.
        const bool isShaped = t.size() == 5 && loaded.excludedRows.size() == 1;
        failures += expect(isShaped, "loader: expected 5 transitions and 1 excluded row");
        if (isShaped)
        {
            failures += expect(t[0].membersOverride == 1 && t[1].membersOverride == 0
                                   && t[2].membersOverride == -1,
                               "loader: true / false / absent read as 1 / 0 / -1");
            failures += expect(t[3].membersOverride == 1 && t[4].membersOverride == 1,
                               "loader: a lodestone's routes inherit its flag");
        }
        return failures;
    }

    int checkZonesLoader(const std::filesystem::path &dir)
    {
        std::filesystem::remove(dir / "spell_teleports.json");
        std::filesystem::remove(dir / "item_teleports.json");
        writeFile(dir / "f2p_zones.json",
                  R"({"zones": [{"name": "z", "source": "s", "min_x": 1, "min_y": 2,
                     "max_x": 3, "max_y": 4, "plane_min": 0, "plane_max": 3}],
                     "members_holes": [{"name": "h", "source": "s", "min_x": 2, "min_y": 2,
                     "max_x": 2, "max_y": 2}]})");
        const ww::data::LoadedDatasets loaded = ww::data::loadDatasets(dir.string());
        int failures = expect(loaded.f2pZones.zones.size() == 1 && loaded.f2pZones.holes.size() == 1
                                  && loaded.f2pZones.zones[0].planeMax == 3,
                              "loader: f2p_zones.json zones and holes");
        writeFile(dir / "f2p_zones.json",
                  R"({"zones": [{"name": "z", "min_x": 1, "min_y": 2, "max_x": 3, "max_y": 4}]})");
        bool isThrown = false;
        try
        {
            (void)ww::data::loadDatasets(dir.string());
        }
        catch (const std::exception &)
        {
            isThrown = true;
        }
        failures += expect(isThrown, "loader: a zone box without a source is refused");
        return failures;
    }

    int checkLoader()
    {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec)
            / ("wwcli-f2p-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            return fail("loader: cannot create a fixture directory");
        }
        int failures = 0;
        try
        {
            failures += checkLoaderFlags(dir);
            failures += checkZonesLoader(dir);
        }
        catch (const std::exception &e)
        {
            failures += fail(std::string("loader: unexpected exception: ") + e.what());
        }
        std::filesystem::remove_all(dir, ec);
        std::printf("f2p: loader %s\n", failures == 0 ? "PASS" : "FAIL");
        return failures;
    }

    // ---- The shipped datasets -------------------------------------------------

    std::filesystem::path findShippedDatasets()
    {
        std::error_code ec;
        std::filesystem::path at = std::filesystem::current_path(ec);
        while (!ec && !at.empty())
        {
            if (std::filesystem::exists(at / "datasets" / "f2p_zones.json", ec))
            {
                return at / "datasets";
            }
            if (at == at.parent_path())
            {
                break;
            }
            at = at.parent_path();
        }
        return {};
    }

    F2pLand landOf(const ww::data::F2pZonesModel &model)
    {
        const auto encode = [](const ww::data::F2pBox &b)
        {
            return box(b.minX, b.minY, b.maxX, b.maxY, b.planeMin, b.planeMax);
        };
        std::vector<F2pBoxRecord> zones;
        std::vector<F2pBoxRecord> holes;
        std::transform(model.zones.begin(), model.zones.end(), std::back_inserter(zones), encode);
        std::transform(model.holes.begin(), model.holes.end(), std::back_inserter(holes), encode);
        return F2pLand(zones, holes);
    }

    struct TestTile
    {
        const char *name;
        int32_t x;
        int32_t y;
        int32_t plane;
        bool isFree;
    };

    // The brief's tiles, and a few the routes it names must cross.
    constexpr std::array<TestTile, 25> kTestTiles = {{
        {"Lumbridge", 3222, 3218, 0, true},
        {"Varrock", 3212, 3428, 0, true},
        {"Draynor", 3093, 3245, 0, true},
        {"Edgeville", 3094, 3492, 0, true},
        {"Falador", 2965, 3380, 0, true},
        {"Taverley", 2895, 3450, 0, true},
        {"Taverley Dungeon entrance", 2879, 3459, 0, true},
        {"Burthorpe", 2899, 3544, 0, true},
        {"Port Sarim", 3010, 3220, 0, true},
        {"Wilderness", 3100, 3680, 0, true},
        {"Shantay Pass north", 3303, 3120, 0, true},
        {"Musa Point", 2918, 3176, 0, true},
        {"south of Shantay Pass", 3304, 3110, 0, false},
        {"Digsite (free since Archaeology, 2020)", 3360, 3420, 0, true},
        {"Fort Forinthry courtyard", 3305, 3553, 0, false},
        {"Fort Forinthry lodestone", 3298, 3526, 0, false},
        {"Lumbridge Swamp Caves", 3170, 9570, 0, false},
        {"Entrana", 2838, 3360, 0, false},
        {"Brimhaven", 2760, 3238, 0, false},
        {"Taverley Dungeon", 2875, 9880, 0, false},
        {"Catherby", 2810, 3440, 0, false},
        {"Death Plateau", 2865, 3590, 0, false},
        {"Canifis", 3500, 3490, 0, false},
        {"Ardougne", 2660, 3305, 0, false},
        {"White Wolf Mountain", 2848, 3498, 0, false},
    }};

    int checkShippedZones(const F2pLand &land)
    {
        int failures = 0;
        for (const TestTile &t : kTestTiles)
        {
            const bool isFree = land.isFreeToPlay(t.x, t.y, t.plane);
            failures += expect(isFree == t.isFree, std::string("shipped zones: ") + t.name + " "
                                                       + tileText(t.x, t.y, t.plane) + " should be "
                                                       + (t.isFree ? "free" : "members"));
        }
        return failures;
    }

    // Lodestones carry the cache enums' verdict, and a free one lands on
    // free land. Spells the cache marks members carry the flag even where
    // they land on free land (the generator's whole reason to exist).
    int checkShippedTeleports(const ww::data::LoadedDatasets &globals, const F2pLand &land)
    {
        int failures = 0;
        std::size_t freeLodestones = 0;
        const std::set<std::string> membersSpells = {"Lassar Teleport", "Dareeyak Teleport",
                                                     "Ghorrock Teleport", "House Teleport",
                                                     "Anachronia Teleport"};
        const std::set<std::string> freeSpells = {"Varrock Teleport", "Lumbridge Teleport",
                                                  "Falador Teleport"};
        for (const ww::data::Transition &t : globals.model.transitions)
        {
            const bool isLodestone = t.kind == ww::data::TransitionKind::Lodestone;
            if (isLodestone && kFreeLodestones.contains(t.label))
            {
                ++freeLodestones;
                failures += expect(t.membersOverride == 0 && land.isFreeToPlay(t.destX, t.destY, t.destPlane),
                                   "shipped: free lodestone " + t.label + " must be flagged free and land free");
            }
            else if (isLodestone)
            {
                failures += expect(t.membersOverride >= 0, "shipped: lodestone " + t.label + " needs a flag");
                failures += expect(!kMembersLodestones.contains(t.label) || t.membersOverride == 1,
                                   "shipped: members lodestone " + t.label + " must be flagged members");
            }
            failures += expect(!membersSpells.contains(t.label) || t.membersOverride == 1,
                               "shipped: " + t.label + " is a members spell");
            failures += expect(!freeSpells.contains(t.label) || t.membersOverride < 0,
                               "shipped: " + t.label + " is a free spell and carries no flag");
        }
        failures += expect(freeLodestones >= kFreeLodestones.size(),
                           "shipped: every free lodestone is in item_teleports.json");
        return failures;
    }

    int checkShippedDatasets(const std::filesystem::path &dir)
    {
        if (dir.empty())
        {
            return fail("shipped: datasets/f2p_zones.json not found above the working directory");
        }
        int failures = 0;
        try
        {
            const ww::data::LoadedDatasets bake = ww::data::loadDatasets(dir.string());
            const F2pLand land = landOf(bake.f2pZones);
            failures += checkShippedZones(land);
            failures += checkShippedTeleports(ww::data::loadGlobalTeleports(dir.string()), land);
            std::printf("f2p: shipped datasets %zu zones, %zu holes\n", bake.f2pZones.zones.size(),
                        bake.f2pZones.holes.size());
        }
        catch (const std::exception &e)
        {
            failures += fail(std::string("shipped: ") + e.what());
        }
        std::printf("f2p: shipped datasets %s\n", failures == 0 ? "PASS" : "FAIL");
        return failures;
    }
}

int runF2pTests(const char *wwaPath, const char *datasetDir)
{
    int failures = 0;
    failures += checkLandGeometry();
    failures += checkMembersRule();
    failures += checkSnapshotBit();
    failures += checkLoader();
    const std::filesystem::path dir =
        datasetDir != nullptr ? std::filesystem::path(datasetDir) : findShippedDatasets();
    failures += checkShippedDatasets(dir);
    if (wwaPath == nullptr)
    {
        std::printf("f2p: planner SKIP (no artifact given)\n");
    }
    else
    {
        failures += runF2pPlannerTests(wwaPath, dir);
    }
    std::printf("f2p: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
