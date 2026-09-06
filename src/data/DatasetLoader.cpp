#include "data/DatasetLoader.h"

#include "data/Transitions.h"
#include "format/Artifact.h"

#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include <nlohmann/json.hpp>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ww::data
{
    namespace
    {
        using json = nlohmann::json;
        using LoaderFn = void (*)(const json &, TransitionModel &);

        bool readFileBytes(const std::string &path, std::string &out)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                return false;
            }
            std::ostringstream buffer;
            buffer << stream.rdbuf();
            out = buffer.str();
            return true;
        }

        void fnv1a(uint32_t &hash, const std::string &bytes)
        {
            for (const char ch : bytes)
            {
                hash ^= static_cast<uint8_t>(ch);
                hash *= 16777619u;
            }
        }

        void copyCode(char (&dst)[4], const std::string &code)
        {
            const std::size_t n = code.size() < 3 ? code.size() : 3;
            for (std::size_t i = 0; i < n; ++i)
            {
                dst[i] = code[i];
            }
        }

        // Narrow a validated-integer JSON value to int, failing loud when it
        // does not fit (nlohmann's get<int> silently truncates wider values,
        // which is at odds with the loader's fail-loud philosophy).
        int narrowJsonInt(const json &v, const char *context)
        {
            const int64_t wide = v.get<int64_t>();
            if (wide < std::numeric_limits<int>::min()
                || wide > std::numeric_limits<int>::max())
            {
                throw std::runtime_error(std::string(context)
                                         + ": integer value out of int range");
            }
            return static_cast<int>(wide);
        }

        // Required-field reader. Loudly fails the build on missing or
        // non-integer fields rather than letting `value(key, 0)` quietly
        // invent a transition rooted at tile (0, 0, 0). Missed fields are
        // typos / dataset-format drift; either way the build should surface
        // the bad record by file + key, not bake a broken transition.
        int readRequiredInt(const json &node, const char *key, const char *context)
        {
            if (!node.contains(key))
            {
                throw std::runtime_error(std::string(context)
                                         + ": missing required field '"
                                         + key + "'");
            }
            const json &v = node.at(key);
            if (!v.is_number_integer())
            {
                throw std::runtime_error(std::string(context)
                                         + ": field '" + key
                                         + "' must be an integer");
            }
            return narrowJsonInt(v, context);
        }

        // Plane reader: a required integer that must also be a legal plane
        // (0..3). The value is stored in a uint8_t and later packed into a
        // 4-bit grid key, so an unchecked -1 or 16 would not fail - it would
        // alias onto a real square's plane 0 and bake a transition into the
        // wrong place. Every other malformed field throws; so does this one.
        uint8_t readPlane(const json &node, const char *key, const char *context)
        {
            const int plane = readRequiredInt(node, key, context);
            if (plane < 0 || plane >= format::kClipPlanes)
            {
                throw std::runtime_error(std::string(context) + ": field '" + key
                                         + "' must be a plane in 0.." 
                                         + std::to_string(format::kClipPlanes - 1));
            }
            return static_cast<uint8_t>(plane);
        }

        // Optional-field reader that still enforces integer typing when
        // present. value<int>(key, default) on a JSON float silently
        // truncates in some nlohmann configurations; this helper closes
        // that bug class at a single site.
        int readOptionalInt(const json &node, const char *key, int defaultValue,
                            const char *context)
        {
            if (!node.contains(key))
            {
                return defaultValue;
            }
            const json &v = node.at(key);
            if (!v.is_number_integer())
            {
                throw std::runtime_error(std::string(context)
                                         + ": field '" + key
                                         + "' must be an integer if present");
            }
            return narrowJsonInt(v, context);
        }

        // Read element i of a JSON array as an int, or `dflt` when the array is
        // shorter; throws when the present element is not an integer.
        int arrInt(const json &arr, std::size_t i, int dflt, const char *what)
        {
            if (i >= arr.size())
            {
                return dflt;
            }
            if (!arr[i].is_number_integer())
            {
                throw std::runtime_error(std::string(what) + " must be an integer");
            }
            return narrowJsonInt(arr[i], what);
        }

        // Upper bound for any data-driven wait, in game ticks (~30s of game
        // time). Waits come from scripter-editable JSON; a fat-fingered value
        // should fail the load, not stall the executor mid-run.
        constexpr int kMaxWaitTicks = 50;

        int readWaitTicks(const json &node, const char *key, int defaultValue,
                          const char *context)
        {
            const int ticks = readOptionalInt(node, key, defaultValue, context);
            if (ticks < 0 || ticks > kMaxWaitTicks)
            {
                throw std::runtime_error(std::string(context)
                                         + ": wait ticks out of range [0, "
                                         + std::to_string(kMaxWaitTicks) + "]");
            }
            return ticks;
        }

        // Pack (interface, component) into the COMPONENT action's param3.
        // Validated so a negative component cannot sign-extend over the
        // interface id and an oversized interface cannot overflow the packed
        // int (the executor unpacks the interface as param3 >> 16, so the
        // packed value must stay non-negative).
        int packIfaceComp(int iface, int comp, const char *context)
        {
            if (iface < 0 || iface > 0x7FFF || comp < 0 || comp > 0xFFFF)
            {
                throw std::runtime_error(std::string(context)
                                         + ": interface/component out of packing range");
            }
            return static_cast<int>((static_cast<uint32_t>(iface) << 16)
                                    | static_cast<uint32_t>(comp));
        }

        // Host action id for an interface-component interaction. Must match
        // ActionTypes.COMPONENT on the Java side (the value rides in the baked
        // chain / runtime teleport JSON, so it is part of the wire contract).
        constexpr int kComponentActionId = 57;

        void parseRequirements(const json &node, std::vector<Requirement> &out)
        {
            if (!node.contains("requirements") || !node.at("requirements").is_object())
            {
                return;
            }
            // `id` is required on every gate: a requirement without one is
            // meaningless, and the silent -1 default used to flow through to
            // the executor as a varbit / item probe of id -1.
            const json &req = node.at("requirements");
            if (req.contains("skill") && req.at("skill").is_object())
            {
                const json &s = req.at("skill");
                out.push_back({RequirementKind::Skill,
                               readRequiredInt(s, "id", "requirements.skill"),
                               readOptionalInt(s, "level", 0, "requirements.skill")});
            }
            if (req.contains("varbit") && req.at("varbit").is_object())
            {
                const json &v = req.at("varbit");
                out.push_back({RequirementKind::Varbit,
                               readRequiredInt(v, "id", "requirements.varbit"),
                               readOptionalInt(v, "value", 0, "requirements.varbit")});
            }
            if (req.contains("varp") && req.at("varp").is_object())
            {
                const json &v = req.at("varp");
                out.push_back({RequirementKind::Varp,
                               readRequiredInt(v, "id", "requirements.varp"),
                               readOptionalInt(v, "value", 0, "requirements.varp")});
            }
            if (req.contains("items") && req.at("items").is_array())
            {
                for (const json &it : req.at("items"))
                {
                    out.push_back({RequirementKind::Item,
                                   readRequiredInt(it, "id", "requirements.item"),
                                   readOptionalInt(it, "count", 1, "requirements.item")});
                }
            }
        }

        // One parser per chain-step shape. Each recognises its own key, appends
        // exactly one ChainStep and returns true, or returns false untouched so
        // the next parser gets a look. Splitting the old else-if ladder this way
        // keeps each step kind's field mapping (see ChainStepKind) beside the
        // validation that shape needs, instead of a hundred lines deep in one
        // function.

        // Component-click shorthand [interface, component, option, sub?] ->
        // generic COMPONENT action: param1=option, param2=sub (-1 when absent),
        // param3=(iface<<16)|comp. The first two elements are required — a
        // short array used to default them to 0 and bake a chain that clicks
        // interface 0.
        bool parseClickStep(const json &step, std::vector<ChainStep> &out)
        {
            if (!step.contains("click") || !step.at("click").is_array())
            {
                return false;
            }
            const json &c = step.at("click");
            if (c.size() < 2)
            {
                throw std::runtime_error("chain.click needs at least [interface, component]");
            }
            const int iface  = arrInt(c, 0, 0,  "chain.click[0]");
            const int comp   = arrInt(c, 1, 0,  "chain.click[1]");
            const int option = arrInt(c, 2, 0,  "chain.click[2]");
            const int sub    = arrInt(c, 3, -1, "chain.click[3]");
            out.push_back({ChainStepKind::Click, kComponentActionId, option, sub,
                           packIfaceComp(iface, comp, "chain.click")});
            return true;
        }

        // Raw queued action [actionId, param1, param2, param3].
        bool parseActionStep(const json &step, std::vector<ChainStep> &out)
        {
            if (!step.contains("action") || !step.at("action").is_array())
            {
                return false;
            }
            const json &a = step.at("action");
            if (a.empty())
            {
                throw std::runtime_error("chain.action needs at least [actionId]");
            }
            out.push_back({ChainStepKind::Click,
                           arrInt(a, 0, 0, "chain.action[0]"),
                           arrInt(a, 1, 0, "chain.action[1]"),
                           arrInt(a, 2, 0, "chain.action[2]"),
                           arrInt(a, 3, 0, "chain.action[3]")});
            return true;
        }

        // Fixed wait. a=ticks.
        bool parseWaitStep(const json &step, std::vector<ChainStep> &out)
        {
            if (!step.contains("wait"))
            {
                return false;
            }
            out.push_back({ChainStepKind::Wait,
                           readWaitTicks(step, "wait", 0, "chain.wait"),
                           0, 0, 0, 0, 0, 0, 0, 0});
            return true;
        }

        // Block until interface N is open (host poll). a=interfaceId.
        bool parseWaitInterfaceStep(const json &step, std::vector<ChainStep> &out)
        {
            if (!step.contains("wait_interface"))
            {
                return false;
            }
            out.push_back({ChainStepKind::WaitInterface,
                           readOptionalInt(step, "wait_interface", 0, "chain.wait_interface"),
                           0, 0, 0, 0, 0, 0, 0, 0});
            return true;
        }

        // Select option `index` in dialogue interface `interface`. a=interface,
        // b=index, c=per_page, d=next_comp, e=wait_ticks. The host resolves the
        // option component against the live dialogue (paging), so only these
        // descriptors are carried.
        bool parseDialogueSelectStep(const json &step, std::vector<ChainStep> &out)
        {
            if (!step.contains("dialogue_select") || !step.at("dialogue_select").is_object())
            {
                return false;
            }
            const json &ds = step.at("dialogue_select");
            out.push_back({ChainStepKind::DialogueSelect,
                           readOptionalInt(ds, "interface", 720, "chain.dialogue_select.interface"),
                           readOptionalInt(ds, "index", 0, "chain.dialogue_select.index"),
                           readOptionalInt(ds, "per_page", 9, "chain.dialogue_select.per_page"),
                           readOptionalInt(ds, "next_comp", 44, "chain.dialogue_select.next_comp"),
                           readWaitTicks(ds, "wait_ticks", 3, "chain.dialogue_select.wait_ticks"),
                           0, 0, 0, 0});
            return true;
        }

        // Click a teleport item that may be worn OR carried. The host picks the
        // variant by checking the live worn/backpack containers for the
        // transition's required item.
        //   a..d = worn(iface, comp, option, sub)
        //   e..h = backpack(iface, comp, option, sub)
        //   i    = backpack_special (COMPONENT_SPECIAL when non-zero)
        bool parseClickItemStep(const json &step, std::vector<ChainStep> &out)
        {
            if (!step.contains("click_item") || !step.at("click_item").is_object())
            {
                return false;
            }
            const json &ci = step.at("click_item");
            const json &w = ci.contains("worn") ? ci.at("worn") : json::array();
            const json &b = ci.contains("backpack") ? ci.at("backpack") : json::array();
            // Absent = "variant not available"; a present-but-short array is a
            // typo that would bake a click on interface 0.
            if ((!w.empty() && w.size() < 2) || (!b.empty() && b.size() < 2))
            {
                throw std::runtime_error(
                    "chain.click_item worn/backpack need at least [interface, component]");
            }
            const int special = ci.value("backpack_special", false) ? 1 : 0;
            out.push_back({ChainStepKind::ClickItem,
                           arrInt(w, 0, 0, "chain.click_item.worn[0]"),
                           arrInt(w, 1, 0, "chain.click_item.worn[1]"),
                           arrInt(w, 2, 1, "chain.click_item.worn[2]"),
                           arrInt(w, 3, -1, "chain.click_item.worn[3]"),
                           arrInt(b, 0, 0, "chain.click_item.backpack[0]"),
                           arrInt(b, 1, 0, "chain.click_item.backpack[1]"),
                           arrInt(b, 2, 1, "chain.click_item.backpack[2]"),
                           arrInt(b, 3, -1, "chain.click_item.backpack[3]"),
                           special});
            return true;
        }

        void parseChain(const json &node, std::vector<ChainStep> &out)
        {
            if (!node.contains("chain") || !node.at("chain").is_array())
            {
                return;
            }
            // Tried in order; the first parser that recognises the step's shape
            // consumes it. A step matching none is ignored, which is what the
            // else-if ladder this replaces did with an unknown chain key.
            using StepParser = bool (*)(const json &, std::vector<ChainStep> &);
            static constexpr StepParser kStepParsers[] = {
                parseClickStep,
                parseActionStep,
                parseWaitStep,
                parseWaitInterfaceStep,
                parseDialogueSelectStep,
                parseClickItemStep,
            };
            for (const json &step : node.at("chain"))
            {
                for (const StepParser parseStep : kStepParsers)
                {
                    if (parseStep(step, out))
                    {
                        break;
                    }
                }
            }
        }

        void parseTransportLinks(const json &j, TransitionModel &model)
        {
            if (!j.is_array())
            {
                return;
            }
            for (const json &e : j)
            {
                Transition t;
                t.kind = TransitionKind::Transport;
                t.originX = readRequiredInt(e, "x", "transport_links");
                t.originY = readRequiredInt(e, "y", "transport_links");
                t.originPlane = readPlane(e, "plane", "transport_links");
                t.destX = readRequiredInt(e, "dest_x", "transport_links");
                t.destY = readRequiredInt(e, "dest_y", "transport_links");
                t.destPlane = readPlane(e, "dest_plane", "transport_links");
                t.objectId = readOptionalInt(e, "object_id", -1, "transport_links");
                t.shape = static_cast<uint8_t>(
                    readOptionalInt(e, "shape", 0, "transport_links"));
                t.rotation = static_cast<uint8_t>(
                    readOptionalInt(e, "rotation", 0, "transport_links"));
                t.optionIndex = static_cast<uint8_t>(
                    readOptionalInt(e, "option_index", 0, "transport_links"));
                parseRequirements(e, t.requirements);
                parseChain(e, t.chain);
                model.transitions.push_back(std::move(t));
            }
        }

        void parseTeleportChains(const json &j, TransitionModel &model)
        {
            if (!j.is_array())
            {
                return;
            }
            for (const json &e : j)
            {
                Transition t;
                const std::string type = e.value("type", std::string{});
                t.kind = (type == "fairy_ring") ? TransitionKind::FairyRing
                                                : TransitionKind::TeleportChain;
                t.originX = readRequiredInt(e, "origin_x", "teleport_chains");
                t.originY = readRequiredInt(e, "origin_y", "teleport_chains");
                t.originPlane = readPlane(e, "origin_plane", "teleport_chains");
                t.destX = readRequiredInt(e, "dest_x", "teleport_chains");
                t.destY = readRequiredInt(e, "dest_y", "teleport_chains");
                t.destPlane = readPlane(e, "dest_plane", "teleport_chains");
                t.objectId = readOptionalInt(e, "object_id", -1, "teleport_chains");
                copyCode(t.code, e.value("code", std::string{}));
                // Previously omitted: per-entry capability gates and embedded
                // chain step lists were dropped at parse time, so every fairy
                // ring / teleport chain landed in the artifact with empty
                // requirements + empty chain, leaving the executor with
                // nothing to run.
                parseRequirements(e, t.requirements);
                parseChain(e, t.chain);
                model.transitions.push_back(std::move(t));
            }
        }

        void parseSpellTeleports(const json &j, TransitionModel &model)
        {
            if (!j.contains("teleports") || !j.at("teleports").is_array())
            {
                return;
            }
            for (const json &e : j.at("teleports"))
            {
                Transition t;
                t.kind = TransitionKind::Spell;
                t.isGlobalOrigin = e.value("global", true);
                t.destX = readRequiredInt(e, "dest_x", "spell_teleports");
                t.destY = readRequiredInt(e, "dest_y", "spell_teleports");
                t.destPlane = readPlane(e, "dest_plane", "spell_teleports");
                // Non-global spells require an origin tile — otherwise the
                // build would emit a transition rooted at (0,0,0). Read it
                // strictly when isGlobalOrigin=false; global spells default
                // origin to (0,0,0) which is unused (the executor casts in
                // place from the player's tile).
                if (!t.isGlobalOrigin)
                {
                    t.originX = readRequiredInt(e, "origin_x", "spell_teleports(non-global)");
                    t.originY = readRequiredInt(e, "origin_y", "spell_teleports(non-global)");
                    t.originPlane = readPlane(e, "origin_plane", "spell_teleports(non-global)");
                }
                parseRequirements(e, t.requirements);
                parseChain(e, t.chain);
                model.transitions.push_back(std::move(t));
            }
        }

        struct LodestoneConfig
        {
            int openInterface{};
            int openComponent{};
            int openOption{1};
            int selectInterface{};
            int selectOption{1};
            int selectSub{-1};
            int openWait{};
            int teleportWait{};
        };

        LodestoneConfig readLodestoneConfig(const json &cfg)
        {
            // The interfaces/components are required: defaulting them to 0
            // used to bake a network of chains that all click interface 0.
            LodestoneConfig c;
            c.openInterface = readRequiredInt(cfg, "open_interface", "lodestones.config");
            c.openComponent = readRequiredInt(cfg, "open_component", "lodestones.config");
            c.openOption = readOptionalInt(cfg, "open_option", 1, "lodestones.config");
            c.selectInterface = readRequiredInt(cfg, "select_interface", "lodestones.config");
            c.selectOption = readOptionalInt(cfg, "select_option", 1, "lodestones.config");
            c.selectSub = readOptionalInt(cfg, "select_sub_component", -1, "lodestones.config");
            c.openWait = readWaitTicks(cfg, "open_wait", 0, "lodestones.config");
            c.teleportWait = readWaitTicks(cfg, "teleport_wait", 0, "lodestones.config");
            return c;
        }

        void buildLodestoneChain(const LodestoneConfig &c, int component, int selectSub,
                                 std::vector<ChainStep> &out)
        {
            // Open the lodestone map: click the ribbon/HUD component (no sub).
            out.push_back({ChainStepKind::Click, kComponentActionId, c.openOption, -1,
                           packIfaceComp(c.openInterface, c.openComponent, "lodestones.config")});
            out.push_back({ChainStepKind::Wait, c.openWait, 0, 0, 0});
            // Select the destination lodestone in the map. Some chains target a
            // sub-component of the select component (selectSub); -1 = none.
            out.push_back({ChainStepKind::Click, kComponentActionId, c.selectOption, selectSub,
                           packIfaceComp(c.selectInterface, component, "lodestones.destination")});
            out.push_back({ChainStepKind::Wait, c.teleportWait, 0, 0, 0});
        }

        void parseLodestones(const json &j, TransitionModel &model)
        {
            if (!j.contains("lodestones") || !j.at("lodestones").is_object())
            {
                return;
            }
            const json &lode = j.at("lodestones");
            if (!lode.contains("destinations") || !lode.at("destinations").is_array())
            {
                return;
            }
            const json cfgNode = lode.contains("config") ? lode.at("config") : json::object();
            const LodestoneConfig cfg = readLodestoneConfig(cfgNode);
            for (const json &d : lode.at("destinations"))
            {
                Transition t;
                t.kind = TransitionKind::Lodestone;
                t.isGlobalOrigin = true;
                t.destX = readRequiredInt(d, "x", "lodestones.destination");
                t.destY = readRequiredInt(d, "y", "lodestones.destination");
                t.destPlane = readPlane(d, "plane", "lodestones.destination");
                parseRequirements(d, t.requirements);
                // Required: a destination without its map component used to
                // default to component 0 and bake a teleport that clicks the
                // wrong widget while the planner sees a perfectly valid edge.
                const int comp = readRequiredInt(d, "component", "lodestones.destination");
                const int sub =
                    readOptionalInt(d, "sub_component", cfg.selectSub, "lodestones.destination");
                buildLodestoneChain(cfg, comp, sub, t.chain);
                model.transitions.push_back(std::move(t));
            }
        }

        // item_teleports.json `teleports[]`: generic item-click teleports
        // (dungeoneering cape, jewellery, etc.). Distinct from the `lodestones`
        // object in the same file (parseLodestones handles that). Each entry is
        // a global-origin teleport with an explicit dest + a chain that clicks
        // the item and works the resulting dialog.
        void parseItemTeleports(const json &j, TransitionModel &model)
        {
            if (!j.contains("teleports") || !j.at("teleports").is_array())
            {
                return;
            }
            for (const json &e : j.at("teleports"))
            {
                Transition t;
                t.kind = TransitionKind::ItemTeleport;
                t.isGlobalOrigin = e.value("global", true);
                t.destX = readRequiredInt(e, "dest_x", "item_teleports.teleport");
                t.destY = readRequiredInt(e, "dest_y", "item_teleports.teleport");
                t.destPlane = readPlane(e, "dest_plane", "item_teleports.teleport");
                parseRequirements(e, t.requirements);
                parseChain(e, t.chain);
                model.transitions.push_back(std::move(t));
            }
        }

        // item_teleports.json carries two independent sections — the lodestone
        // network (`lodestones`) and the generic item teleports (`teleports`).
        // Parse both from the one file.
        void parseItemTeleportsFile(const json &j, TransitionModel &model)
        {
            parseLodestones(j, model);
            parseItemTeleports(j, model);
        }

        // False when the file is absent (warned, skipped, and counted by the
        // caller); true once a present file has been parsed into `model`.
        bool loadOne(const std::string &directory, const char *filename, LoaderFn parser,
                     TransitionModel &model, uint32_t &hash)
        {
            std::string text;
            const std::string path = directory + "/" + filename;
            if (!readFileBytes(path, text))
            {
                std::fprintf(stderr, "wwbuild: dataset not found, skipping: %s\n", path.c_str());
                return false;
            }
            fnv1a(hash, text);
            const json j = json::parse(text);
            parser(j, model);
            return true;
        }
    }

    LoadedDatasets loadDatasets(const std::string &directory)
    {
        LoadedDatasets result;
        uint32_t hash = 2166136261u;
        if (loadOne(directory, "transport_links.json", &parseTransportLinks, result.model, hash))
        {
            ++result.filesFound;
        }
        else
        {
            ++result.filesMissing;
        }
        if (loadOne(directory, "teleport_chains.json", &parseTeleportChains, result.model, hash))
        {
            ++result.filesFound;
        }
        else
        {
            ++result.filesMissing;
        }
        if (loadOne(directory, "spell_teleports.json", &parseSpellTeleports, result.model, hash))
        {
            ++result.filesFound;
        }
        else
        {
            ++result.filesMissing;
        }
        if (loadOne(directory, "item_teleports.json", &parseItemTeleportsFile, result.model, hash))
        {
            ++result.filesFound;
        }
        else
        {
            ++result.filesMissing;
        }
        result.datasetHash = hash;
        return result;
    }

    LoadedDatasets loadGlobalTeleports(const std::string &directory)
    {
        LoadedDatasets result;
        uint32_t hash = 2166136261u;
        // Only the global-origin teleport datasets. transport_links /
        // teleport_chains are local transitions wired into the baked area graph
        // and cannot be supplied at runtime, so they are deliberately skipped.
        if (loadOne(directory, "spell_teleports.json", &parseSpellTeleports, result.model, hash))
        {
            ++result.filesFound;
        }
        else
        {
            ++result.filesMissing;
        }
        if (loadOne(directory, "item_teleports.json", &parseItemTeleportsFile, result.model, hash))
        {
            ++result.filesFound;
        }
        else
        {
            ++result.filesMissing;
        }
        result.datasetHash = hash;

        // Defensive: keep only global-origin transitions. The two files above
        // produce global teleports today, but a non-global spell (origin_x set)
        // would have no area-graph edge baked for it, so it could never be
        // reached — drop it rather than append a dead record.
        std::vector<Transition> kept;
        kept.reserve(result.model.transitions.size());
        for (Transition &t : result.model.transitions)
        {
            if (t.isGlobalOrigin)
            {
                kept.push_back(std::move(t));
            }
        }
        result.model.transitions = std::move(kept);
        return result;
    }
}
