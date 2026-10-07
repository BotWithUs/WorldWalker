#include "cli/DescribeTests.h"

#include "c_api/worldwalker_c.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "runtime/RuntimeTeleports.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <future>
#include <map>
#include <iterator>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

// The expected side of every comparison here is read from the dataset JSON by
// the oracle below, which is written from the dataset format's documentation
// (format/Artifact.h, data/Transitions.h) and deliberately shares no code with
// data/DatasetLoader.cpp, the parser that baked the artifact. A test that
// reused the loader would agree with any mistake the loader makes.
namespace
{
    using json = nlohmann::json;

    using Req  = std::array<int32_t, 3>;   // kind, id, amount
    using Step = std::array<int32_t, 10>;  // kind, a..i

    // Host action id of a component click; the dataset's `click` shorthand
    // bakes to it (ActionTypes.COMPONENT on the host side).
    constexpr int32_t kComponentAction = 57;
    constexpr int32_t kDefaultNpcRadius = 8;

    // The comparable form of one transition: what the JSON row says, and what
    // ww_transition_describe returned, converted to the same shape.
    struct Shape
    {
        int32_t origin{};
        int32_t kind{};
        WwTile  originTile{};
        WwTile  destTile{};
        int32_t objectId{-1};
        int32_t npcTypeMin{-1};
        int32_t npcTypeMax{-1};
        int32_t npcSearchRadius{-1};
        int32_t optionIndex{-1};
        int32_t shape{};
        int32_t rotation{};
        int32_t isRuntimeLoaded{};
        std::vector<Req>  requirements;  // sorted: order is not part of the contract
        std::vector<Step> chain;
    };

    struct DatasetRow
    {
        std::string source;    // "transport_links[949] Charter ship - Brimhaven"
        Shape       expected;
        std::string oracleError;  // non-empty: the row holds something the oracle cannot express
        std::string loaderGap;    // non-empty: the row states a gate the loader does not read
        bool        hasNoLoc{false};  // object_id 0 and no NPC: the bake drops it (NoLoc)
    };

    // How far the bake (TransitionBuilder kSnapRadius) and the runtime
    // teleport loader (kDestSnapRadius) may move a destination onto a
    // standable tile. The description reports the snapped tile, the real
    // landing spot; checkEveryRecord pins it to the record exactly.
    constexpr int32_t kDestSnapRadius = 5;

    // Requirement keys present in the datasets that data/DatasetLoader.cpp
    // has never parsed, so no artifact carries them. Reported, not failed:
    // the description is faithful to the artifact, and the gap is upstream.
    constexpr const char *kLoaderGapKeys[] = {"extra"};

    // ---- Oracle: JSON -> Shape ---------------------------------------------

    int32_t intOr(const json &node, const char *key, int32_t fallback)
    {
        return node.contains(key) ? node.at(key).get<int32_t>() : fallback;
    }

    int32_t at(const json &arr, std::size_t i, int32_t fallback)
    {
        return i < arr.size() ? arr[i].get<int32_t>() : fallback;
    }

    int32_t packComponent(int32_t iface, int32_t comp)
    {
        return static_cast<int32_t>((static_cast<uint32_t>(iface) << 16) | static_cast<uint32_t>(comp));
    }

    // Each gate key: the requirement kind, the field holding the amount, and
    // the amount a bare `{id}` means.
    struct GateKey
    {
        const char *key;
        int32_t     kind;
        const char *amountKey;
        int32_t     amountDefault;
    };

    constexpr GateKey kGateKeys[] = {
        {"skill",           WW_REQUIREMENT_SKILL,           "level", 0},
        {"varbit",          WW_REQUIREMENT_VARBIT,          "value", 0},
        {"varbit_at_least", WW_REQUIREMENT_VARBIT_AT_LEAST, "value", 1},
        {"varp",            WW_REQUIREMENT_VARP,            "value", 0},
        {"varp_at_least",   WW_REQUIREMENT_VARP_AT_LEAST,   "value", 1},
        {"varp_bit",        WW_REQUIREMENT_VARP_BIT,        "bit",   0},
    };

    // Appends `node`'s requirements. Any key the oracle does not know is an
    // error: a requirement the dataset states but the description lacks is
    // exactly the failure this test exists to catch.
    void readRequirements(const json &node, std::vector<Req> &out, DatasetRow &ioRow)
    {
        if (!node.contains("requirements"))
        {
            return;
        }
        std::string &outError = ioRow.oracleError;
        for (const auto &[key, value] : node.at("requirements").items())
        {
            const bool isLoaderGap = std::any_of(std::begin(kLoaderGapKeys), std::end(kLoaderGapKeys),
                                                 [&key](const char *k) { return key == k; });
            if (isLoaderGap)
            {
                ioRow.loaderGap = "'" + key + "': " + value.dump();
                continue;
            }
            if (key == "items")
            {
                for (const json &item : value)
                {
                    out.push_back({WW_REQUIREMENT_ITEM, item.at("id").get<int32_t>(), intOr(item, "count", 1)});
                }
                continue;
            }
            const auto gate = std::find_if(std::begin(kGateKeys), std::end(kGateKeys),
                                           [&key](const GateKey &g) { return key == g.key; });
            if (gate == std::end(kGateKeys))
            {
                outError = "unknown requirement key '" + key + "'";
                continue;
            }
            const json list = value.is_array() ? value : json::array({value});
            for (const json &v : list)
            {
                out.push_back({gate->kind, v.at("id").get<int32_t>(), intOr(v, gate->amountKey, gate->amountDefault)});
            }
        }
    }

    Step clickItemStep(const json &ci)
    {
        const json worn = ci.value("worn", json::array());
        const json pack = ci.value("backpack", json::array());
        return {WW_CHAIN_STEP_CLICK_ITEM,
                at(worn, 0, 0), at(worn, 1, 0), at(worn, 2, 1), at(worn, 3, -1),
                at(pack, 0, 0), at(pack, 1, 0), at(pack, 2, 1), at(pack, 3, -1),
                ci.value("backpack_special", false) ? 1 : 0};
    }

    Step dialogueSelectStep(const json &ds)
    {
        return {WW_CHAIN_STEP_DIALOGUE_SELECT,
                intOr(ds, "interface", 720), intOr(ds, "index", 0), intOr(ds, "per_page", 9),
                intOr(ds, "next_comp", 44), intOr(ds, "wait_ticks", 3), 0, 0, 0, 0};
    }

    // One dataset chain step. Unknown shapes are errors, for the same reason
    // unknown requirement keys are.
    bool readStep(const json &step, Step &outStep)
    {
        if (step.contains("click"))
        {
            const json &c = step.at("click");
            outStep = {WW_CHAIN_STEP_CLICK, kComponentAction, at(c, 2, 0), at(c, 3, -1),
                       packComponent(at(c, 0, 0), at(c, 1, 0)), 0, 0, 0, 0, 0};
            return true;
        }
        if (step.contains("action"))
        {
            const json &a = step.at("action");
            outStep = {WW_CHAIN_STEP_CLICK, at(a, 0, 0), at(a, 1, 0), at(a, 2, 0), at(a, 3, 0), 0, 0, 0, 0, 0};
            return true;
        }
        if (step.contains("wait"))
        {
            outStep = {WW_CHAIN_STEP_WAIT, step.at("wait").get<int32_t>(), 0, 0, 0, 0, 0, 0, 0, 0};
            return true;
        }
        if (step.contains("wait_interface"))
        {
            outStep = {WW_CHAIN_STEP_WAIT_INTERFACE, step.at("wait_interface").get<int32_t>(), 0, 0, 0, 0, 0, 0, 0, 0};
            return true;
        }
        if (step.contains("dialogue_select"))
        {
            outStep = dialogueSelectStep(step.at("dialogue_select"));
            return true;
        }
        if (step.contains("click_item"))
        {
            outStep = clickItemStep(step.at("click_item"));
            return true;
        }
        return false;
    }

    void readChain(const json &node, std::vector<Step> &out, std::string &outError)
    {
        if (!node.contains("chain"))
        {
            return;
        }
        for (const json &step : node.at("chain"))
        {
            Step s{};
            if (!readStep(step, s))
            {
                outError = "unknown chain step " + step.dump();
                continue;
            }
            out.push_back(s);
        }
    }

    WwTile tileOf(const json &row, const char *x, const char *y, const char *plane)
    {
        return WwTile{row.at(x).get<int32_t>(), row.at(y).get<int32_t>(), intOr(row, plane, 0)};
    }

    // A transport_links row: a loc, or an NPC when the row names one.
    DatasetRow transportRow(const json &row, std::size_t index)
    {
        DatasetRow out;
        out.source = "transport_links[" + std::to_string(index) + "] " + row.value("name", std::string{});
        Shape &e = out.expected;
        e.kind = WW_TRANSITION_KIND_TRANSPORT;
        e.originTile = tileOf(row, "x", "y", "plane");
        e.destTile = tileOf(row, "dest_x", "dest_y", "dest_plane");
        readRequirements(row, e.requirements, out);
        readChain(row, e.chain, out.oracleError);
        e.optionIndex = intOr(row, "option_index", 0);
        if (!row.contains("npc"))
        {
            e.origin = WW_TRANSITION_ORIGIN_LOC;
            e.objectId = intOr(row, "object_id", -1);
            e.shape = intOr(row, "shape", 0);
            e.rotation = intOr(row, "rotation", 0);
            out.hasNoLoc = e.objectId == 0;
            return out;
        }
        // An NPC origin is clicked through a leading ClickNpc step centred on
        // the row's tile: a=option, b..d=centre, e=radius, f..g=type range.
        const json &npc = row.at("npc");
        e.origin = WW_TRANSITION_ORIGIN_NPC;
        e.npcTypeMin = npc.at("first_id").get<int32_t>();
        e.npcTypeMax = intOr(npc, "last_id", e.npcTypeMin);
        e.npcSearchRadius = intOr(npc, "radius", kDefaultNpcRadius);
        e.chain.insert(e.chain.begin(),
                       Step{WW_CHAIN_STEP_CLICK_NPC, e.optionIndex, e.originTile.x, e.originTile.y,
                            e.originTile.plane, e.npcSearchRadius, e.npcTypeMin, e.npcTypeMax, 0, 0});
        return out;
    }

    // A global teleport row (spell or item): no origin, runtime-loaded.
    DatasetRow globalRow(const json &row, const std::string &source, int32_t kind)
    {
        DatasetRow out;
        out.source = source + " " + row.value("name", std::string{});
        Shape &e = out.expected;
        e.origin = WW_TRANSITION_ORIGIN_GLOBAL;
        e.kind = kind;
        e.isRuntimeLoaded = 1;
        e.destTile = tileOf(row, "dest_x", "dest_y", "dest_plane");
        readRequirements(row, e.requirements, out);
        readChain(row, e.chain, out.oracleError);
        return out;
    }

    // The lodestone map chain the dataset's `config` block describes: open the
    // map, wait, click the destination's component, wait.
    std::vector<Step> lodestoneMapChain(const json &cfg, const json &dest)
    {
        const int32_t sub = intOr(dest, "sub_component", intOr(cfg, "select_sub_component", -1));
        return {
            Step{WW_CHAIN_STEP_CLICK, kComponentAction, intOr(cfg, "open_option", 1), -1,
                 packComponent(cfg.at("open_interface").get<int32_t>(), cfg.at("open_component").get<int32_t>()),
                 0, 0, 0, 0, 0},
            Step{WW_CHAIN_STEP_WAIT, intOr(cfg, "open_wait", 0), 0, 0, 0, 0, 0, 0, 0, 0},
            Step{WW_CHAIN_STEP_CLICK, kComponentAction, intOr(cfg, "select_option", 1), sub,
                 packComponent(cfg.at("select_interface").get<int32_t>(), dest.at("component").get<int32_t>()),
                 0, 0, 0, 0, 0},
            Step{WW_CHAIN_STEP_WAIT, intOr(cfg, "teleport_wait", 0), 0, 0, 0, 0, 0, 0, 0, 0},
        };
    }

    // One lodestone destination: one transition per authored route (the
    // destination's gates plus the route's own), then the map.
    void lodestoneRows(const json &cfg, const json &dest, std::size_t index, std::vector<DatasetRow> &out)
    {
        DatasetRow base;
        base.source = "item_teleports.lodestones[" + std::to_string(index) + "]";
        Shape &e = base.expected;
        e.origin = WW_TRANSITION_ORIGIN_GLOBAL;
        e.kind = WW_TRANSITION_KIND_LODESTONE;
        e.isRuntimeLoaded = 1;
        e.destTile = tileOf(dest, "x", "y", "plane");
        readRequirements(dest, e.requirements, base);
        const json routes = dest.value("routes", json::array());
        for (std::size_t r = 0; r < routes.size(); ++r)
        {
            DatasetRow route = base;
            route.source += " route " + std::to_string(r);
            readRequirements(routes[r], route.expected.requirements, route);
            readChain(routes[r], route.expected.chain, route.oracleError);
            out.push_back(std::move(route));
        }
        base.source += " map";
        e.chain = lodestoneMapChain(cfg, dest);
        out.push_back(std::move(base));
    }

    json readJson(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            throw std::runtime_error("cannot open " + path);
        }
        return json::parse(in);
    }

    std::vector<DatasetRow> readDatasetRows(const std::string &dir)
    {
        std::vector<DatasetRow> rows;
        const json links = readJson(dir + "/transport_links.json");
        for (std::size_t i = 0; i < links.size(); ++i)
        {
            if (!links[i].value("disabled", false))
            {
                rows.push_back(transportRow(links[i], i));
            }
        }
        const json spells = readJson(dir + "/spell_teleports.json").at("teleports");
        for (std::size_t i = 0; i < spells.size(); ++i)
        {
            rows.push_back(globalRow(spells[i], "spell_teleports[" + std::to_string(i) + "]",
                                     WW_TRANSITION_KIND_SPELL));
        }
        const json items = readJson(dir + "/item_teleports.json");
        for (std::size_t i = 0; i < items.at("teleports").size(); ++i)
        {
            rows.push_back(globalRow(items.at("teleports")[i], "item_teleports[" + std::to_string(i) + "]",
                                     WW_TRANSITION_KIND_ITEM_TELEPORT));
        }
        const json &lode = items.at("lodestones");
        for (std::size_t i = 0; i < lode.at("destinations").size(); ++i)
        {
            lodestoneRows(lode.at("config"), lode.at("destinations")[i], i, rows);
        }
        for (DatasetRow &row : rows)
        {
            std::sort(row.expected.requirements.begin(), row.expected.requirements.end());
        }
        return rows;
    }

    // ---- Provenance gate -----------------------------------------------------

    std::string fnv1a64Hex(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        uint64_t hash = 14695981039346656037ull;
        for (const char ch : bytes)
        {
            hash ^= static_cast<uint8_t>(ch);
            hash *= 1099511628211ull;
        }
        char text[24];
        std::snprintf(text, sizeof(text), "0x%016llX", static_cast<unsigned long long>(hash));
        return text;
    }

    // True when every dataset file the artifact records was baked from the
    // same bytes as the file in `dir`. Comparing against other bytes would
    // report dataset drift as describe bugs.
    bool isBakedFrom(const ww::format::ArtifactReader &reader, const std::string &dir)
    {
        if (!reader.hasProvenance())
        {
            std::printf("describe: artifact has no provenance; cannot tell what it was baked from\n");
            return false;
        }
        const json doc = json::parse(reader.provenanceJson());
        bool isSame = true;
        for (const json &file : doc.at("datasetFiles"))
        {
            const std::string name = file.at("name").get<std::string>();
            const std::string want = file.at("fnv1a64").get<std::string>();
            const std::string have = fnv1a64Hex(dir + "/" + name);
            const bool isMatch = _stricmp(want.c_str(), have.c_str()) == 0;
            std::printf("describe: provenance %-22s baked %s, dir %s %s\n", name.c_str(), want.c_str(),
                        have.c_str(), isMatch ? "same" : "DIFFERENT");
            isSame = isSame && isMatch;
        }
        return isSame;
    }

    // ---- Observed side: ww_transition_describe -> Shape ----------------------

    Shape shapeOf(const WwTransitionInfo &info)
    {
        Shape s;
        s.origin = info.origin;
        s.kind = info.kind;
        s.originTile = info.originTile;
        s.destTile = info.destTile;
        s.objectId = info.objectId;
        s.npcTypeMin = info.npcTypeMin;
        s.npcTypeMax = info.npcTypeMax;
        s.npcSearchRadius = info.npcSearchRadius;
        s.optionIndex = info.optionIndex;
        s.shape = info.shape;
        s.rotation = info.rotation;
        s.isRuntimeLoaded = info.isRuntimeLoaded;
        const uint32_t reqs = std::min<uint32_t>(info.requirementCount, WW_TRANSITION_MAX_REQUIREMENTS);
        for (uint32_t i = 0; i < reqs; ++i)
        {
            const WwRequirement &r = info.requirements[i];
            s.requirements.push_back({r.kind, r.id, r.amount});
        }
        std::sort(s.requirements.begin(), s.requirements.end());
        const uint32_t steps = std::min<uint32_t>(info.chainStepCount, WW_TRANSITION_MAX_CHAIN_STEPS);
        for (uint32_t i = 0; i < steps; ++i)
        {
            const WwChainStep &c = info.chain[i];
            s.chain.push_back({c.kind, c.a, c.b, c.c, c.d, c.e, c.f, c.g, c.h, c.i});
        }
        return s;
    }

    bool sameTile(const WwTile &a, const WwTile &b)
    {
        return a.x == b.x && a.y == b.y && a.plane == b.plane;
    }

    // The dataset's authored destination against the snapped one.
    bool isWithinSnap(const WwTile &authored, const WwTile &snapped)
    {
        return authored.plane == snapped.plane && std::abs(authored.x - snapped.x) <= kDestSnapRadius
            && std::abs(authored.y - snapped.y) <= kDestSnapRadius;
    }

    // Names of the fields that differ; empty when the shapes agree.
    std::string diff(const Shape &want, const Shape &have)
    {
        std::string out;
        const auto note = [&out](bool isSame, const char *field)
        {
            if (!isSame)
            {
                out += out.empty() ? field : std::string(",") + field;
            }
        };
        note(want.origin == have.origin, "origin");
        note(want.kind == have.kind, "kind");
        note(sameTile(want.originTile, have.originTile), "originTile");
        note(isWithinSnap(want.destTile, have.destTile), "destTile");
        note(want.objectId == have.objectId, "objectId");
        note(want.npcTypeMin == have.npcTypeMin && want.npcTypeMax == have.npcTypeMax, "npcType");
        note(want.npcSearchRadius == have.npcSearchRadius, "npcSearchRadius");
        note(want.optionIndex == have.optionIndex, "optionIndex");
        note(want.shape == have.shape && want.rotation == have.rotation, "shape/rotation");
        note(want.isRuntimeLoaded == have.isRuntimeLoaded, "isRuntimeLoaded");
        note(want.requirements == have.requirements, "requirements");
        note(want.chain == have.chain, "chain");
        return out;
    }

    // ---- Matching dataset rows to artifact records ---------------------------

    // What narrows a row to its candidate records: global teleports by kind
    // and destination plane, local ones also by origin and loc (-1 for an NPC
    // origin). The destination's x/y are left out because the bake snaps
    // them; diff() holds them to the snap radius.
    using Key = std::tuple<int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t>;

    Key keyOf(const Shape &s)
    {
        const bool isGlobal = s.origin == WW_TRANSITION_ORIGIN_GLOBAL;
        return {isGlobal ? 1 : 0, s.kind,
                isGlobal ? 0 : s.originTile.x, isGlobal ? 0 : s.originTile.y, isGlobal ? 0 : s.originTile.plane,
                s.destTile.plane, isGlobal ? -1 : s.objectId};
    }

    // Built from the raw records through ArtifactReader, not from the
    // description, so a describe bug in a key field shows up as a mismatch
    // rather than quietly as "row not baked".
    std::multimap<Key, uint32_t> indexRecords(const ww::format::ArtifactReader &reader)
    {
        std::multimap<Key, uint32_t> index;
        const auto txs = reader.transitions();
        const auto chain = reader.chainSteps();
        for (uint32_t i = 0; i < txs.size(); ++i)
        {
            const ww::format::TransitionRecord &tx = txs[i];
            const bool isGlobal = (tx.flags & ww::format::kTransitionFlagGlobalOrigin) != 0u;
            const bool isNpc = !isGlobal && tx.chainCount > 0
                && chain[tx.chainStart].kind == static_cast<uint8_t>(WW_CHAIN_STEP_CLICK_NPC);
            const Key key{isGlobal ? 1 : 0, tx.kind,
                          isGlobal ? 0 : tx.originX, isGlobal ? 0 : tx.originY, isGlobal ? 0 : tx.originPlane,
                          tx.destPlane, (isGlobal || isNpc) ? -1 : tx.objectId};
            index.emplace(key, i);
        }
        return index;
    }

    struct Tally
    {
        std::size_t run{};
        std::size_t failed{};
    };

    void check(Tally &tally, bool isPass, const std::string &what)
    {
        ++tally.run;
        if (!isPass)
        {
            ++tally.failed;
            std::printf("describe: FAIL  %s\n", what.c_str());
        }
    }

    // check() for the one-off checks, which say so when they pass too.
    void checkLoud(Tally &tally, bool isPass, const std::string &what)
    {
        check(tally, isPass, what);
        if (isPass)
        {
            std::printf("describe: PASS  %s\n", what.c_str());
        }
    }

    struct Described
    {
        ww_result        result{};
        WwTransitionInfo info{};
    };

    // Prefilled with 0xCD so a field the library forgets to write is visible.
    Described describe(const ww_artifact *artifact, uint32_t index)
    {
        Described d;
        std::memset(&d.info, 0xCD, sizeof(d.info));
        d.result = ww_transition_describe(artifact, index, &d.info);
        return d;
    }

    // Every byte past the counted entries, and the padding, is zero.
    bool isTailZero(const WwTransitionInfo &info)
    {
        const auto isZero = [](const void *p, std::size_t n)
        {
            const auto *b = static_cast<const unsigned char *>(p);
            return std::all_of(b, b + n, [](unsigned char c) { return c == 0; });
        };
        const std::size_t reqs = std::min<std::size_t>(info.requirementCount, WW_TRANSITION_MAX_REQUIREMENTS);
        const std::size_t steps = std::min<std::size_t>(info.chainStepCount, WW_TRANSITION_MAX_CHAIN_STEPS);
        return isZero(info.pad, sizeof(info.pad))
            && isZero(&info.requirements[reqs], (WW_TRANSITION_MAX_REQUIREMENTS - reqs) * sizeof(WwRequirement))
            && isZero(&info.chain[steps], (WW_TRANSITION_MAX_CHAIN_STEPS - steps) * sizeof(WwChainStep));
    }

    bool isAllZero(const WwTransitionInfo &info)
    {
        const auto *b = reinterpret_cast<const unsigned char *>(&info);
        return std::all_of(b, b + sizeof(info), [](unsigned char c) { return c == 0; });
    }

    // Per record: the description agrees with the raw record exactly on the
    // tiles (which the row check holds only to the snap radius) and on what has
    // no dataset counterpart (cost, code, the movement category, the counts and
    // isRuntimeLoaded), and the zero-tail guarantee holds.
    void checkEveryRecord(const ww_artifact *artifact, const ww::format::ArtifactReader &reader, Tally &tally)
    {
        const auto txs = reader.transitions();
        const auto categories = reader.moveCategories();
        std::size_t bad = 0;
        for (uint32_t i = 0; i < txs.size(); ++i)
        {
            const Described d = describe(artifact, i);
            const ww::format::TransitionRecord &tx = txs[i];
            const bool isGlobal = (tx.flags & ww::format::kTransitionFlagGlobalOrigin) != 0u;
            const WwTile origin = isGlobal ? WwTile{} : WwTile{tx.originX, tx.originY, tx.originPlane};
            const bool isGood = d.result == WW_OK && isTailZero(d.info) && d.info.cost == txs[i].cost
                && sameTile(d.info.originTile, origin)
                && sameTile(d.info.destTile, WwTile{tx.destX, tx.destY, tx.destPlane})
                && std::memcmp(d.info.code, txs[i].code, sizeof(d.info.code)) == 0
                && d.info.moveCategory == static_cast<int32_t>(categories[i])
                && d.info.requirementCount == txs[i].requirementCount
                && d.info.chainStepCount == txs[i].chainCount
                && d.info.isRuntimeLoaded == (i >= reader.bakedTransitions() ? 1 : 0);
            if (!isGood && bad++ < 5)
            {
                std::printf("describe: record %u: result %d or tile/tail/cost/code/category/counts wrong\n", i, d.result);
            }
        }
        checkLoud(tally, bad == 0, "every record: OK, zero tail, tiles/cost/code/category/counts as baked ("
                                   + std::to_string(bad) + " of " + std::to_string(txs.size()) + " bad)");
    }

    void checkErrors(const ww_artifact *artifact, uint32_t count, Tally &tally)
    {
        const Described past = describe(artifact, count);
        checkLoud(tally, past.result == WW_ERR_NOT_FOUND && isAllZero(past.info), "index == count -> NOT_FOUND, zeroed");
        const Described walk = describe(artifact, UINT32_MAX);
        checkLoud(tally, walk.result == WW_ERR_NOT_FOUND && isAllZero(walk.info), "UINT32_MAX -> NOT_FOUND, zeroed");
        const Described null = describe(nullptr, 0);
        checkLoud(tally, null.result == WW_ERR_INVALID && isAllZero(null.info), "NULL artifact -> INVALID, zeroed");
        checkLoud(tally, ww_transition_describe(artifact, 0, nullptr) == WW_ERR_INVALID, "NULL outInfo -> INVALID");
    }

    // ---- Dataset rows --------------------------------------------------------

    struct RowCounts
    {
        std::size_t matched{};
        std::size_t notBaked{};
        std::size_t noLoc{};
        std::size_t loaderGaps{};
        std::size_t superseded{};
        std::size_t mismatched{};
        std::size_t oracleErrors{};
    };

    // Rows named in the PR: each must be found and match. Matched on source.
    constexpr const char *kSpotlights[] = {
        "transport_links[2483] Gate south of the Wilderness Agility Course",
        "transport_links[949] Charter ship - Brimhaven",
        "transport_links[135] View",
        "transport_links[0] Climb-down",
        "spell_teleports[0]",
        "item_teleports[0]",
        "item_teleports.lodestones[0]",
    };

    bool isSpotlight(const std::string &source)
    {
        return std::any_of(std::begin(kSpotlights), std::end(kSpotlights),
                           [&source](const char *s) { return source.rfind(s, 0) == 0; });
    }

    void printInfo(const char *label, const WwTransitionInfo &i)
    {
        std::printf("  %s: origin=%d kind=%d move=%d runtime=%d from=(%d,%d,%d) to=(%d,%d,%d) obj=%d "
                    "npc=[%d,%d] r=%d opt=%d shape=%u rot=%u cost=%.1f reqs=%u steps=%u\n",
                    label, i.origin, i.kind, i.moveCategory, i.isRuntimeLoaded, i.originTile.x, i.originTile.y,
                    i.originTile.plane, i.destTile.x, i.destTile.y, i.destTile.plane, i.objectId, i.npcTypeMin,
                    i.npcTypeMax, i.npcSearchRadius, i.optionIndex, i.shape, i.rotation, i.cost,
                    i.requirementCount, i.chainStepCount);
        for (uint32_t r = 0; r < std::min<uint32_t>(i.requirementCount, WW_TRANSITION_MAX_REQUIREMENTS); ++r)
        {
            std::printf("    req  kind=%d id=%d amount=%d\n", i.requirements[r].kind, i.requirements[r].id,
                        i.requirements[r].amount);
        }
        for (uint32_t s = 0; s < std::min<uint32_t>(i.chainStepCount, WW_TRANSITION_MAX_CHAIN_STEPS); ++s)
        {
            const WwChainStep &c = i.chain[s];
            std::printf("    step kind=%d a..i=%d %d %d %d %d %d %d %d %d\n", c.kind, c.a, c.b, c.c, c.d, c.e,
                        c.f, c.g, c.h, c.i);
        }
    }

    // One row: find its record(s) by key; pass when a candidate describes to
    // exactly the row. A row whose candidates all match some OTHER row lost
    // the bake's dedup on (kind, origin, dest) and is counted, not failed.
    void checkRow(const DatasetRow &row, const std::vector<DatasetRow> &rows,
                  const std::multimap<Key, uint32_t> &index, const ww_artifact *artifact,
                  RowCounts &counts, Tally &tally)
    {
        const bool isNamed = isSpotlight(row.source);
        if (!row.oracleError.empty())
        {
            ++counts.oracleErrors;
            check(tally, false, row.source + ": " + row.oracleError);
            return;
        }
        if (!row.loaderGap.empty())
        {
            ++counts.loaderGaps;
            std::printf("describe: LOADER GAP  %s states %s, which the loader never reads\n",
                        row.source.c_str(), row.loaderGap.c_str());
        }
        const auto [first, last] = index.equal_range(keyOf(row.expected));
        if (first == last && row.hasNoLoc)
        {
            ++counts.noLoc;
            check(tally, !isNamed, row.source + ": named row has object_id 0, which the bake drops");
            return;
        }
        if (first == last)
        {
            ++counts.notBaked;
            check(tally, !isNamed, row.source + ": named row not found in the artifact");
            return;
        }
        std::string firstDiff;
        Described firstSeen;
        for (auto it = first; it != last; ++it)
        {
            const Described d = describe(artifact, it->second);
            const std::string fields = diff(row.expected, shapeOf(d.info));
            if (d.result == WW_OK && fields.empty())
            {
                ++counts.matched;
                if (isNamed)
                {
                    std::printf("describe: spotlight %s -> index %u\n", row.source.c_str(), it->second);
                    printInfo("described", d.info);
                }
                return;
            }
            if (it == first)
            {
                firstDiff = fields;
                firstSeen = d;
            }
        }
        const bool isClaimedElsewhere = std::any_of(rows.begin(), rows.end(), [&](const DatasetRow &other)
        {
            return &other != &row && other.oracleError.empty()
                && diff(other.expected, shapeOf(firstSeen.info)).empty();
        });
        if (isClaimedElsewhere && !isNamed)
        {
            ++counts.superseded;
            return;
        }
        ++counts.mismatched;
        check(tally, false, row.source + ": fields differ: " + firstDiff);
        printInfo("described", firstSeen.info);
    }

    void checkRows(const std::vector<DatasetRow> &rows, const ww::format::ArtifactReader &reader,
                   const ww_artifact *artifact, Tally &tally)
    {
        const std::multimap<Key, uint32_t> index = indexRecords(reader);
        RowCounts counts;
        for (const DatasetRow &row : rows)
        {
            checkRow(row, rows, index, artifact, counts, tally);
        }
        std::printf("describe: rows %zu: matched %zu, dropped for object_id 0 %zu, otherwise not baked %zu, "
                    "lost bake dedup %zu, mismatched %zu, oracle errors %zu, loader gaps %zu\n", rows.size(),
                    counts.matched, counts.noLoc, counts.notBaked, counts.superseded, counts.mismatched,
                    counts.oracleErrors, counts.loaderGaps);
        // Every spotlight was seen at all (a typo in kSpotlights would otherwise pass silently).
        for (const char *name : kSpotlights)
        {
            const bool isPresent = std::any_of(rows.begin(), rows.end(), [name](const DatasetRow &r)
            {
                return r.source.rfind(name, 0) == 0;
            });
            check(tally, isPresent, std::string("spotlight row exists in the datasets: ") + name);
        }
    }

    // ---- Re-entry from an executor callback ----------------------------------

    // The run calls readPosition first. Inside it: start a reload on another
    // thread (it queues behind the run's shared hold), give it time to queue,
    // then describe. Without the held-run check, describe would take the lock
    // shared a second time behind the queued writer and never return.
    struct ReentryProbe
    {
        ww_artifact     *artifact{};
        std::string      teleportDir;
        WwTile           goal{};
        std::jthread     reloader;
        std::atomic<bool> hasReloadReturned{false};
        bool             isReloadQueuedFirst{false};
        Described        inside{};
        bool             hasProbed{false};
    };

    void probeReadPosition(void *user, WwTile *outTile)
    {
        auto *probe = static_cast<ReentryProbe *>(user);
        *outTile = probe->goal;
        if (probe->hasProbed)
        {
            return;
        }
        probe->hasProbed = true;
        probe->reloader = std::jthread([probe]
        {
            ww_artifact_load_teleports(probe->artifact, probe->teleportDir.c_str());
            probe->hasReloadReturned = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        // Still blocked: the reload really is queued behind this run.
        probe->isReloadQueuedFirst = !probe->hasReloadReturned;
        probe->inside = describe(probe->artifact, 0);
    }

    void probeCapability(void *, WwCapabilitySnapshot *outSnapshot) { *outSnapshot = WwCapabilitySnapshot{}; }
    void probeNoopInstance(void *, WwInstanceChunks *outChunks) { *outChunks = WwInstanceChunks{}; }
    int32_t probeZeroItem(void *, int32_t) { return 0; }
    void probeZeroMany(void *, const int32_t *, size_t count, int32_t *outValues)
    {
        std::fill(outValues, outValues + count, 0);
    }
    int32_t probeZeroId(void *, int32_t) { return 0; }
    void probeWalk(void *, WwTile) {}
    int32_t probeInteract(void *, int32_t, WwTile, int32_t) { return 0; }
    void probeChain(void *, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {}
    void probeSleep(void *, int32_t) {}
    int32_t probeCancel(void *) { return 1; }

    WwCallbacks probeCallbacks(ReentryProbe &probe)
    {
        WwCallbacks cb{};
        cb.user = &probe;
        cb.readPosition = probeReadPosition;
        cb.readCapability = probeCapability;
        cb.readInstance = probeNoopInstance;
        cb.readItemCount = probeZeroItem;
        cb.readVarbits = probeZeroMany;
        cb.readItemCounts = probeZeroMany;
        cb.isItemWorn = probeZeroId;
        cb.isInterfaceOpen = probeZeroId;
        cb.walkTo = probeWalk;
        cb.interact = probeInteract;
        cb.runChainStep = probeChain;
        cb.sleepTicks = probeSleep;
        cb.shouldCancel = probeCancel;
        return cb;
    }

    // Runs on its own thread so a deadlock is reported, not hung on: past the
    // deadline the process exits with a FAIL, since a thread stuck in a lock
    // can be neither joined nor cancelled.
    void checkReentry(ww_artifact *artifact, ww_context_pool *pool, const std::string &dir, Tally &tally)
    {
        const Described outside = describe(artifact, 0);
        ReentryProbe probe;
        probe.artifact = artifact;
        probe.teleportDir = dir;
        probe.goal = outside.info.destTile;
        std::packaged_task<int32_t()> task([&probe, artifact, pool]
        {
            const WwCallbacks cb = probeCallbacks(probe);
            return ww_executor_run(artifact, pool, WwGoal{probe.goal.x, probe.goal.y, probe.goal.plane, 0}, &cb);
        });
        std::future<int32_t> status = task.get_future();
        std::jthread runner(std::move(task));
        if (status.wait_for(std::chrono::seconds(20)) != std::future_status::ready)
        {
            std::printf("describe: FAIL  describe inside an executor callback deadlocked behind a queued reload\n");
            std::fflush(stdout);
            std::_Exit(1);
        }
        runner.join();
        probe.reloader.join();
        checkLoud(tally, probe.hasProbed && probe.isReloadQueuedFirst,
              "re-entry: a reload was queued behind the run when describe ran");
        checkLoud(tally, probe.inside.result == WW_OK
                  && std::memcmp(&probe.inside.info, &outside.info, sizeof(WwTransitionInfo)) == 0,
              "re-entry: describe inside the callback returned, and agrees with describe outside");
        checkLoud(tally, status.get() == WW_STATUS_ARRIVED && probe.hasReloadReturned,
              "re-entry: the run arrived and the queued reload then completed");
    }
}

int runDescribeTests(const char *wwaPath, const char *datasetDir)
{
    try
    {
        const std::string dir(datasetDir);
        ww::format::ArtifactReader reader(wwaPath);
        if (!isBakedFrom(reader, dir))
        {
            std::printf("describe: INCONCLUSIVE  artifact was not baked from %s; nothing compared\n", datasetDir);
            return 2;
        }
        ww::runtime::loadGlobalTeleportsInto(reader, dir);

        ww_artifact *artifact = ww_artifact_open(wwaPath);
        ww_context_pool *pool = artifact != nullptr ? ww_context_pool_create(artifact, 1) : nullptr;
        if (pool == nullptr || ww_artifact_load_teleports(artifact, datasetDir) != WW_OK)
        {
            std::printf("describe: FAIL  could not open through the C ABI: %s\n", ww_last_error());
            ww_context_pool_destroy(pool);
            ww_artifact_close(artifact);
            return 1;
        }

        Tally tally;
        const std::vector<DatasetRow> rows = readDatasetRows(dir);
        checkRows(rows, reader, artifact, tally);
        checkEveryRecord(artifact, reader, tally);
        checkErrors(artifact, static_cast<uint32_t>(reader.transitions().size()), tally);
        checkReentry(artifact, pool, dir, tally);

        ww_context_pool_destroy(pool);
        ww_artifact_close(artifact);
        std::printf("describe: %zu of %zu failed\n", tally.failed, tally.run);
        return tally.failed > 0 ? 1 : 0;
    }
    catch (const std::exception &e)
    {
        std::printf("describe: FAIL  %s\n", e.what());
        return 1;
    }
}
