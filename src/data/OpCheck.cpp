#include "data/OpCheck.h"

#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include <nlohmann/json.hpp>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ww::data
{
    namespace
    {
        using json = nlohmann::json;

        // Five option slots, as in the game's loc and npc configs.
        constexpr int32_t kOptionSlots = 5;

        // A morph of a morph is rare and a cycle is possible in principle; two
        // hops covers every chain in the export.
        constexpr int kMaxMorphDepth = 2;

        // Depths of the export's layout: [ {entry}, ... ] puts an entry at 2,
        // its `actions` / `morphs_N` value at 3, and a morph's `ids` at 4.
        constexpr int kEntryDepth = 2;
        constexpr int kFieldDepth = 3;
        constexpr int kMorphIdsDepth = 4;

        // SAX reader for the export. Keeps `id`, the option slots with text
        // (`actions`, plus the members-only spellings), and the positive ids
        // under `morphs_N.ids`; skips everything else without building it.
        class DefinitionReader
        {
        public:
            explicit DefinitionReader(std::unordered_map<int32_t, OpDefinition> &out)
                : out(out)
            {
            }

            bool null()
            {
                if (isInActions())
                {
                    ++actionIndex;
                }
                return true;
            }

            bool boolean(bool)
            {
                return true;
            }

            bool number_integer(json::number_integer_t value)
            {
                takeNumber(static_cast<int64_t>(value));
                return true;
            }

            bool number_unsigned(json::number_unsigned_t value)
            {
                takeNumber(static_cast<int64_t>(value));
                return true;
            }

            bool number_float(json::number_float_t, const json::string_t &)
            {
                return true;
            }

            bool string(json::string_t &value)
            {
                if (isInActions())
                {
                    if (!value.empty())
                    {
                        markOption(actionIndex);
                    }
                    ++actionIndex;
                }
                else if (depth == kEntryDepth && !value.empty())
                {
                    markOption(memberOptionSlot(entryKey));
                }
                return true;
            }

            bool binary(json::binary_t &)
            {
                return true;
            }

            bool start_object(std::size_t)
            {
                ++depth;
                if (depth == kEntryDepth)
                {
                    current = OpDefinition{};
                    currentId = -1;
                    entryKey.clear();
                }
                return true;
            }

            bool end_object()
            {
                if (depth == kEntryDepth && currentId >= 0)
                {
                    out[currentId] = std::move(current);
                }
                --depth;
                return true;
            }

            bool start_array(std::size_t)
            {
                ++depth;
                if (isInActions())
                {
                    actionIndex = 0;
                }
                return true;
            }

            bool end_array()
            {
                --depth;
                return true;
            }

            bool key(json::string_t &name)
            {
                if (depth == kEntryDepth)
                {
                    entryKey = name;
                }
                else if (depth == kFieldDepth)
                {
                    fieldKey = name;
                }
                return true;
            }

            bool parse_error(std::size_t position, const std::string &, const nlohmann::detail::exception &e)
            {
                throw std::runtime_error("definition export: parse error at byte "
                                         + std::to_string(position) + ": " + e.what());
            }

        private:
            // Members-only options count: the bots play on members' worlds.
            // An NPC lists them as a `members_actions` array beside `actions`.
            bool isInActions() const
            {
                return depth == kFieldDepth
                    && (entryKey == "actions" || entryKey == "members_actions");
            }

            // A loc lists its members-only options as scalars, 1-based:
            // `members_action_1` .. `members_action_5`. Returns the 0-based
            // slot, or -1 for any other key.
            static int32_t memberOptionSlot(const std::string &name)
            {
                constexpr std::string_view kPrefix = "members_action_";
                if (name.size() != kPrefix.size() + 1 || !name.starts_with(kPrefix))
                {
                    return -1;
                }
                return name.back() - '1';
            }

            void markOption(int32_t slot)
            {
                if (slot >= 0 && slot < kOptionSlots)
                {
                    current.opMask = static_cast<uint8_t>(current.opMask | (1u << slot));
                }
            }

            bool isMorphKey() const
            {
                return entryKey == "morphs_1" || entryKey == "morphs_2";
            }

            void takeNumber(int64_t value)
            {
                if (depth == kEntryDepth && entryKey == "id")
                {
                    currentId = static_cast<int32_t>(value);
                }
                else if (depth == kMorphIdsDepth && isMorphKey() && fieldKey == "ids" && value > 0)
                {
                    current.morphs.push_back(static_cast<int32_t>(value));
                }
            }

            std::unordered_map<int32_t, OpDefinition> &out;
            OpDefinition current;
            int32_t currentId{-1};
            int depth{};
            int32_t actionIndex{};
            std::string entryKey;
            std::string fieldKey;
        };

        void readDefinitions(const std::filesystem::path &path,
                             std::unordered_map<int32_t, OpDefinition> &out)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                throw std::runtime_error("op check: cannot open " + path.string());
            }
            const std::string text((std::istreambuf_iterator<char>(stream)),
                                   std::istreambuf_iterator<char>());
            DefinitionReader reader(out);
            // The export carries `// <id> <name>` annotations after some
            // values, so comments must be accepted.
            constexpr bool kIsStrict = true;
            constexpr bool kIgnoreComments = true;
            json::sax_parse(text, &reader, json::input_format_t::json, kIsStrict, kIgnoreComments);
            if (out.empty())
            {
                throw std::runtime_error("op check: no definitions in " + path.string());
            }
        }

        bool hasOp(const std::unordered_map<int32_t, OpDefinition> &defs, int32_t id,
                   int32_t option, int depth)
        {
            const auto it = defs.find(id);
            if (it == defs.end())
            {
                return false;
            }
            if ((it->second.opMask & (1u << option)) != 0u)
            {
                return true;
            }
            if (depth >= kMaxMorphDepth)
            {
                return false;
            }
            for (const int32_t morph : it->second.morphs)
            {
                if (morph != id && hasOp(defs, morph, option, depth + 1))
                {
                    return true;
                }
            }
            return false;
        }

        OpVerdict verdictFor(const std::unordered_map<int32_t, OpDefinition> &defs,
                             int32_t firstId, int32_t lastId, int32_t option)
        {
            bool isAnyKnown = false;
            for (int32_t id = firstId; id <= lastId; ++id)
            {
                if (!defs.contains(id))
                {
                    continue;
                }
                isAnyKnown = true;
                if (option >= 0 && option < kOptionSlots && hasOp(defs, id, option, 0))
                {
                    return OpVerdict::Valid;
                }
            }
            return isAnyKnown ? OpVerdict::MissingOp : OpVerdict::UnknownDefinition;
        }

        bool opensWithNpc(const Transition &t)
        {
            return !t.chain.empty() && t.chain.front().kind == ChainStepKind::ClickNpc;
        }

        std::string describeRow(const Transition &t)
        {
            char line[256];
            if (opensWithNpc(t))
            {
                const ChainStep &npc = t.chain.front();
                std::snprintf(line, sizeof(line), "npc %d..%d op %d @ %d,%d,%u -> %d,%d,%u",
                              npc.f, npc.g, npc.a, t.originX, t.originY,
                              static_cast<unsigned>(t.originPlane), t.destX, t.destY,
                              static_cast<unsigned>(t.destPlane));
            }
            else
            {
                std::snprintf(line, sizeof(line), "loc %d op %u @ %d,%d,%u -> %d,%d,%u",
                              t.objectId, static_cast<unsigned>(t.optionIndex), t.originX,
                              t.originY, static_cast<unsigned>(t.originPlane), t.destX, t.destY,
                              static_cast<unsigned>(t.destPlane));
            }
            return t.label + ": " + line;
        }

        // The verdict for one transition, or Valid for a transition the check
        // does not cover (a teleport, a derived crossing, a row naming no loc).
        OpVerdict judge(const Transition &t, const OpTable &table)
        {
            if (t.kind != TransitionKind::Transport)
            {
                return OpVerdict::Valid;
            }
            if (opensWithNpc(t))
            {
                const ChainStep &npc = t.chain.front();
                return verdictFor(table.npcs, npc.f, npc.g, npc.a);
            }
            if (t.objectId <= 0)
            {
                return OpVerdict::Valid;
            }
            return verdictFor(table.locs, t.objectId, t.objectId, t.optionIndex);
        }

        bool isChecked(const Transition &t)
        {
            return t.kind == TransitionKind::Transport && (opensWithNpc(t) || t.objectId > 0);
        }
    }

    OpTable loadOpTable(const std::string &defsDir)
    {
        const std::filesystem::path dir(defsDir);
        OpTable table;
        readDefinitions(dir / "locations.json", table.locs);
        readDefinitions(dir / "npcs.json", table.npcs);
        return table;
    }

    OpVerdict checkLocOp(const OpTable &table, int32_t locId, int32_t option)
    {
        return verdictFor(table.locs, locId, locId, option);
    }

    OpVerdict checkNpcOp(const OpTable &table, int32_t firstId, int32_t lastId, int32_t option)
    {
        return verdictFor(table.npcs, firstId, lastId, option);
    }

    TransitionModel dropInvalidOps(const TransitionModel &model, const OpTable &table,
                                   OpCheckReport *outReport)
    {
        OpCheckReport report;
        TransitionModel kept;
        kept.transitions.reserve(model.transitions.size());
        for (const Transition &t : model.transitions)
        {
            report.checked += isChecked(t) ? 1u : 0u;
            const OpVerdict verdict = judge(t, table);
            if (verdict == OpVerdict::MissingOp)
            {
                ++report.dropped;
                ++report.droppedByFamily[t.label];
                report.droppedRows.push_back(describeRow(t));
                continue;
            }
            if (verdict == OpVerdict::UnknownDefinition)
            {
                ++report.unverified;
                ++report.unverifiedByFamily[t.label];
            }
            kept.transitions.push_back(t);
        }
        if (outReport != nullptr)
        {
            *outReport = std::move(report);
        }
        return kept;
    }

    void printOpCheckReport(const OpCheckReport &report)
    {
        std::printf("  op check: %zu rows checked, %zu dropped (origin lacks the option), "
                    "%zu unverified (origin id not in the export)\n",
                    report.checked, report.dropped, report.unverified);
        for (const auto &[family, count] : report.droppedByFamily)
        {
            std::printf("    dropped %4zu  %s\n", count, family.c_str());
        }
        for (const auto &[family, count] : report.unverifiedByFamily)
        {
            std::printf("    unverified %4zu  %s\n", count, family.c_str());
        }
        for (const std::string &row : report.droppedRows)
        {
            std::printf("    - %s\n", row.c_str());
        }
    }
}
