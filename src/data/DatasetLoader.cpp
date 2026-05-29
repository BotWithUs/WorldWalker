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
                out.push_back({RequirementKind::Skill, s.value("id", -1), s.value("level", 0)});
            }
            if (req.contains("varbit") && req.at("varbit").is_object())
            {
                const json &v = req.at("varbit");
                out.push_back({RequirementKind::Varbit, v.value("id", -1), v.value("value", 0)});
            }
            if (req.contains("varp") && req.at("varp").is_object())
            {
                const json &v = req.at("varp");
                out.push_back({RequirementKind::Varp, v.value("id", -1), v.value("value", 0)});
            }
            if (req.contains("items") && req.at("items").is_array())
            {
                for (const json &it : req.at("items"))
                {
                    out.push_back({RequirementKind::Item, it.value("id", -1), it.value("count", 1)});
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
                    const json &c = step.at("click");
                    const int a = c.size() > 0 ? c[0].get<int>() : 0;
                    const int b = c.size() > 1 ? c[1].get<int>() : 0;
                    const int d = c.size() > 2 ? c[2].get<int>() : 0;
                    out.push_back({ChainStepKind::Click, a, b, d});
                }
                else if (step.contains("wait"))
                {
                    out.push_back({ChainStepKind::Wait, step.value("wait", 0), 0, 0});
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
                t.originX = e.value("x", 0);
                t.originY = e.value("y", 0);
                t.originPlane = static_cast<uint8_t>(e.value("plane", 0));
                t.destX = e.value("dest_x", 0);
                t.destY = e.value("dest_y", 0);
                t.destPlane = static_cast<uint8_t>(e.value("dest_plane", 0));
                t.objectId = e.value("object_id", -1);
                t.shape = static_cast<uint8_t>(e.value("shape", 0));
                t.rotation = static_cast<uint8_t>(e.value("rotation", 0));
                t.optionIndex = static_cast<uint8_t>(e.value("option_index", 0));
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
                t.originX = e.value("origin_x", 0);
                t.originY = e.value("origin_y", 0);
                t.originPlane = static_cast<uint8_t>(e.value("origin_plane", 0));
                t.destX = e.value("dest_x", 0);
                t.destY = e.value("dest_y", 0);
                t.destPlane = static_cast<uint8_t>(e.value("dest_plane", 0));
                t.objectId = e.value("object_id", -1);
                copyCode(t.code, e.value("code", std::string{}));
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
                t.destX = e.value("dest_x", 0);
                t.destY = e.value("dest_y", 0);
                t.destPlane = static_cast<uint8_t>(e.value("dest_plane", 0));
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
            int openWait{};
            int teleportWait{};
        };

        LodestoneConfig readLodestoneConfig(const json &cfg)
        {
            LodestoneConfig c;
            c.openInterface = cfg.value("open_interface", 0);
            c.openComponent = cfg.value("open_component", 0);
            c.openOption = cfg.value("open_option", 1);
            c.selectInterface = cfg.value("select_interface", 0);
            c.selectOption = cfg.value("select_option", 1);
            c.openWait = cfg.value("open_wait", 0);
            c.teleportWait = cfg.value("teleport_wait", 0);
            return c;
        }

        void buildLodestoneChain(const LodestoneConfig &c, int component,
                                 std::vector<ChainStep> &out)
        {
            out.push_back({ChainStepKind::Click, c.openInterface, c.openComponent, c.openOption});
            out.push_back({ChainStepKind::Wait, c.openWait, 0, 0});
            out.push_back({ChainStepKind::Click, c.selectInterface, component, c.selectOption});
            out.push_back({ChainStepKind::Wait, c.teleportWait, 0, 0});
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
                t.destX = d.value("x", 0);
                t.destY = d.value("y", 0);
                t.destPlane = static_cast<uint8_t>(d.value("plane", 0));
                parseRequirements(d, t.requirements);
                buildLodestoneChain(cfg, d.value("component", 0), t.chain);
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
}
