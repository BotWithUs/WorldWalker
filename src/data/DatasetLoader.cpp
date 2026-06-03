#include "data/DatasetLoader.h"

#include "data/Transitions.h"

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
            return v.get<int>();
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
            return v.get<int>();
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
            return arr[i].get<int>();
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
            const json &req = node.at("requirements");
            if (req.contains("skill") && req.at("skill").is_object())
            {
                const json &s = req.at("skill");
                out.push_back({RequirementKind::Skill,
                               readOptionalInt(s, "id", -1, "requirements.skill"),
                               readOptionalInt(s, "level", 0, "requirements.skill")});
            }
            if (req.contains("varbit") && req.at("varbit").is_object())
            {
                const json &v = req.at("varbit");
                out.push_back({RequirementKind::Varbit,
                               readOptionalInt(v, "id", -1, "requirements.varbit"),
                               readOptionalInt(v, "value", 0, "requirements.varbit")});
            }
            if (req.contains("varp") && req.at("varp").is_object())
            {
                const json &v = req.at("varp");
                out.push_back({RequirementKind::Varp,
                               readOptionalInt(v, "id", -1, "requirements.varp"),
                               readOptionalInt(v, "value", 0, "requirements.varp")});
            }
            if (req.contains("items") && req.at("items").is_array())
            {
                for (const json &it : req.at("items"))
                {
                    out.push_back({RequirementKind::Item,
                                   readOptionalInt(it, "id", -1, "requirements.item"),
                                   readOptionalInt(it, "count", 1, "requirements.item")});
                }
            }
        }

        void parseChain(const json &node, std::vector<ChainStep> &out)
        {
            if (!node.contains("chain") || !node.at("chain").is_array())
            {
                return;
            }
            for (const json &step : node.at("chain"))
            {
                if (step.contains("click") && step.at("click").is_array())
                {
                    // Component-click shorthand [interface, component, option, sub?]
                    // -> generic COMPONENT action: param1=option, param2=sub (-1
                    // when absent), param3=(iface<<16)|comp.
                    const json &c = step.at("click");
                    const int iface  = arrInt(c, 0, 0,  "chain.click[0]");
                    const int comp   = arrInt(c, 1, 0,  "chain.click[1]");
                    const int option = arrInt(c, 2, 0,  "chain.click[2]");
                    const int sub    = arrInt(c, 3, -1, "chain.click[3]");
                    out.push_back({ChainStepKind::Click, kComponentActionId, option, sub,
                                   (iface << 16) | comp});
                }
                else if (step.contains("action") && step.at("action").is_array())
                {
                    // Raw queued action [actionId, param1, param2, param3].
                    const json &a = step.at("action");
                    out.push_back({ChainStepKind::Click,
                                   arrInt(a, 0, 0, "chain.action[0]"),
                                   arrInt(a, 1, 0, "chain.action[1]"),
                                   arrInt(a, 2, 0, "chain.action[2]"),
                                   arrInt(a, 3, 0, "chain.action[3]")});
                }
                else if (step.contains("wait"))
                {
                    out.push_back({ChainStepKind::Wait,
                                   readOptionalInt(step, "wait", 0, "chain.wait"),
                                   0, 0, 0});
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
                t.originPlane = static_cast<uint8_t>(
                    readRequiredInt(e, "plane", "transport_links"));
                t.destX = readRequiredInt(e, "dest_x", "transport_links");
                t.destY = readRequiredInt(e, "dest_y", "transport_links");
                t.destPlane = static_cast<uint8_t>(
                    readRequiredInt(e, "dest_plane", "transport_links"));
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
                t.originPlane = static_cast<uint8_t>(
                    readRequiredInt(e, "origin_plane", "teleport_chains"));
                t.destX = readRequiredInt(e, "dest_x", "teleport_chains");
                t.destY = readRequiredInt(e, "dest_y", "teleport_chains");
                t.destPlane = static_cast<uint8_t>(
                    readRequiredInt(e, "dest_plane", "teleport_chains"));
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
                t.destPlane = static_cast<uint8_t>(
                    readRequiredInt(e, "dest_plane", "spell_teleports"));
                // Non-global spells require an origin tile — otherwise the
                // build would emit a transition rooted at (0,0,0). Read it
                // strictly when isGlobalOrigin=false; global spells default
                // origin to (0,0,0) which is unused (the executor casts in
                // place from the player's tile).
                if (!t.isGlobalOrigin)
                {
                    t.originX = readRequiredInt(e, "origin_x", "spell_teleports(non-global)");
                    t.originY = readRequiredInt(e, "origin_y", "spell_teleports(non-global)");
                    t.originPlane = static_cast<uint8_t>(
                        readRequiredInt(e, "origin_plane", "spell_teleports(non-global)"));
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
            LodestoneConfig c;
            c.openInterface = readOptionalInt(cfg, "open_interface", 0, "lodestones.config");
            c.openComponent = readOptionalInt(cfg, "open_component", 0, "lodestones.config");
            c.openOption = readOptionalInt(cfg, "open_option", 1, "lodestones.config");
            c.selectInterface = readOptionalInt(cfg, "select_interface", 0, "lodestones.config");
            c.selectOption = readOptionalInt(cfg, "select_option", 1, "lodestones.config");
            c.selectSub = readOptionalInt(cfg, "select_sub_component", -1, "lodestones.config");
            c.openWait = readOptionalInt(cfg, "open_wait", 0, "lodestones.config");
            c.teleportWait = readOptionalInt(cfg, "teleport_wait", 0, "lodestones.config");
            return c;
        }

        void buildLodestoneChain(const LodestoneConfig &c, int component, int selectSub,
                                 std::vector<ChainStep> &out)
        {
            // Open the lodestone map: click the ribbon/HUD component (no sub).
            out.push_back({ChainStepKind::Click, kComponentActionId, c.openOption, -1,
                           (c.openInterface << 16) | c.openComponent});
            out.push_back({ChainStepKind::Wait, c.openWait, 0, 0, 0});
            // Select the destination lodestone in the map. Some chains target a
            // sub-component of the select component (selectSub); -1 = none.
            out.push_back({ChainStepKind::Click, kComponentActionId, c.selectOption, selectSub,
                           (c.selectInterface << 16) | component});
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
                t.destPlane = static_cast<uint8_t>(
                    readRequiredInt(d, "plane", "lodestones.destination"));
                parseRequirements(d, t.requirements);
                const int comp = readOptionalInt(d, "component", 0, "lodestones.destination");
                const int sub =
                    readOptionalInt(d, "sub_component", cfg.selectSub, "lodestones.destination");
                buildLodestoneChain(cfg, comp, sub, t.chain);
                model.transitions.push_back(std::move(t));
            }
        }

        void loadOne(const std::string &directory, const char *filename, LoaderFn parser,
                     TransitionModel &model, uint32_t &hash)
        {
            std::string text;
            const std::string path = directory + "/" + filename;
            if (!readFileBytes(path, text))
            {
                std::fprintf(stderr, "wwbuild: dataset not found, skipping: %s\n", path.c_str());
                return;
            }
            fnv1a(hash, text);
            const json j = json::parse(text);
            parser(j, model);
        }
    }

    LoadedDatasets loadDatasets(const std::string &directory)
    {
        LoadedDatasets result;
        uint32_t hash = 2166136261u;
        loadOne(directory, "transport_links.json", &parseTransportLinks, result.model, hash);
        loadOne(directory, "teleport_chains.json", &parseTeleportChains, result.model, hash);
        loadOne(directory, "spell_teleports.json", &parseSpellTeleports, result.model, hash);
        loadOne(directory, "item_teleports.json", &parseLodestones, result.model, hash);
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
        loadOne(directory, "spell_teleports.json", &parseSpellTeleports, result.model, hash);
        loadOne(directory, "item_teleports.json", &parseLodestones, result.model, hash);
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
