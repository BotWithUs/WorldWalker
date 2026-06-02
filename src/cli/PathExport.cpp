#include "cli/PathExport.h"

#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "runtime/AreaSearch.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/PathAssembler.h"
#include "runtime/TileSearch.h"
#include "runtime/WorldView.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

// `wwcli path <artifact> sx sy sp gx gy gp [--out path.json]`.
//
// Runs the runtime PathAssembler with a maximally permissive capability
// snapshot (so requirement-gated transitions are admitted) and emits the
// resulting Plan as JSON for the standalone Leaflet viewer in tools/viewer/.
// The snapshot is built from the artifact's own requirement pool — same
// strategy as dumpTeleportSeeding in main.cpp's harness — so the same
// invocation lands the same path the executor would have followed.
namespace
{
    struct Args
    {
        const char *artifactPath;
        int32_t     startX;
        int32_t     startY;
        int32_t     startPlane;
        int32_t     goalX;
        int32_t     goalY;
        int32_t     goalPlane;
        const char *outPath;   // nullptr -> stdout
    };

    void printUsage()
    {
        std::printf("usage: wwcli path <artifact.wwa> <fromX> <fromY> <fromPlane>"
                    " <toX> <toY> <toPlane> [--out path.json]\n");
    }

    bool parseInt(const char *s, int32_t &out)
    {
        if (s == nullptr)
        {
            return false;
        }
        const std::string_view view{s};
        const auto *first = view.data();
        const auto *last  = first + view.size();
        int32_t value = 0;
        const auto result = std::from_chars(first, last, value);
        if (result.ec != std::errc{} || result.ptr != last)
        {
            return false;
        }
        out = value;
        return true;
    }

    bool parseArgs(int argc, char **argv, Args &out)
    {
        if (argc < 7)
        {
            return false;
        }
        out.artifactPath = argv[0];
        if (!parseInt(argv[1], out.startX) || !parseInt(argv[2], out.startY)
            || !parseInt(argv[3], out.startPlane) || !parseInt(argv[4], out.goalX)
            || !parseInt(argv[5], out.goalY) || !parseInt(argv[6], out.goalPlane))
        {
            return false;
        }
        out.outPath = nullptr;
        for (int i = 7; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--out") == 0)
            {
                if (i + 1 >= argc)
                {
                    return false;
                }
                out.outPath = argv[i + 1];
                ++i;
                continue;
            }
            return false;
        }
        return true;
    }

    // Build a maximally permissive snapshot from the artifact's own requirement
    // pool. Mirrors the buildPermissiveSnapshotFromArtifact helper in main.cpp's
    // harness so this subcommand admits the same set of transitions the
    // executor would have admitted when given a fully-equipped player.
    void buildPermissiveSnapshot(const ww::format::ArtifactReader &reader,
                                 ww::runtime::CapabilitySnapshot &outSnapshot)
    {
        for (const ww::format::RequirementRecord &r : reader.requirements())
        {
            switch (static_cast<ww::data::RequirementKind>(r.kind))
            {
                case ww::data::RequirementKind::Skill:
                    if (outSnapshot.skillLevel(r.id) < r.amount)
                    {
                        outSnapshot.setSkillLevel(r.id, r.amount);
                    }
                    break;
                case ww::data::RequirementKind::Item:
                    if (outSnapshot.itemCount(r.id) < r.amount)
                    {
                        outSnapshot.setItemCount(r.id, r.amount);
                    }
                    break;
                case ww::data::RequirementKind::Varbit:
                    outSnapshot.setVarbit(r.id, r.amount);
                    break;
                case ww::data::RequirementKind::Varp:
                    outSnapshot.setVarp(r.id, r.amount);
                    break;
            }
        }
    }

    const char *transitionKindName(uint8_t kind)
    {
        switch (static_cast<ww::data::TransitionKind>(kind))
        {
            case ww::data::TransitionKind::Transport:     return "transport";
            case ww::data::TransitionKind::FairyRing:     return "fairy_ring";
            case ww::data::TransitionKind::TeleportChain: return "teleport_chain";
            case ww::data::TransitionKind::Spell:         return "spell";
            case ww::data::TransitionKind::Lodestone:     return "lodestone";
        }
        return "unknown";
    }

    // Re-refine the tile-by-tile route between `prev` and the walk step's
    // target through TileSearch — the chunked WALK step the planner emits only
    // records the endpoint, but for visualisation we want the actual sequence
    // of tiles the executor will walk through (otherwise the viewer draws a
    // straight line through buildings the planner correctly routed around).
    //
    // areaConstraint defaults to the area of `prev` so the refinement matches
    // the area the planner chunked through; -1 fallback for off-area cursors
    // (rare). Failure (different area, blocked, etc.) drops the route — viewer
    // falls back to a straight line.
    void appendRoute(ww::runtime::TileSearch &tileSearch, ww::runtime::WorldView &view,
                     int32_t prevX, int32_t prevY, int32_t plane,
                     int32_t targetX, int32_t targetY, nlohmann::ordered_json &outStep)
    {
        const int32_t area = view.areaAt(prevX, prevY, plane);
        ww::runtime::TilePath path;
        const bool ok = tileSearch.findPath(prevX, prevY, targetX, targetY, plane,
                                            area, path)
            || tileSearch.findPath(prevX, prevY, targetX, targetY, plane, -1, path);
        if (!ok || path.tiles.empty())
        {
            return;
        }
        nlohmann::ordered_json route = nlohmann::ordered_json::array();
        for (const ww::runtime::TilePoint &t : path.tiles)
        {
            route.push_back({ t.x, t.y });
        }
        outStep["route"] = route;
    }

    nlohmann::ordered_json buildJson(const Args &a, const ww::runtime::Plan &plan,
                                     ww::runtime::TileSearch &tileSearch,
                                     ww::runtime::WorldView &view,
                                     std::span<const ww::format::TransitionRecord> transitions)
    {
        nlohmann::ordered_json doc;
        doc["artifact"] = a.artifactPath;
        doc["start"]    = { {"x", a.startX}, {"y", a.startY}, {"plane", a.startPlane} };
        doc["goal"]     = { {"x", a.goalX},  {"y", a.goalY},  {"plane", a.goalPlane}  };
        doc["cost"]     = plan.cost;

        int32_t cursorX = a.startX;
        int32_t cursorY = a.startY;
        int32_t cursorP = a.startPlane;

        nlohmann::ordered_json steps = nlohmann::ordered_json::array();
        for (const ww::runtime::Step &s : plan.steps)
        {
            const int32_t stepPlane = static_cast<int32_t>(s.plane);
            nlohmann::ordered_json js;
            js["x"]     = s.targetX;
            js["y"]     = s.targetY;
            js["plane"] = stepPlane;
            if (s.kind == ww::runtime::StepKind::Walk)
            {
                js["kind"] = "walk";
                if (cursorP == stepPlane)
                {
                    appendRoute(tileSearch, view, cursorX, cursorY, stepPlane,
                                s.targetX, s.targetY, js);
                }
                cursorX = s.targetX;
                cursorY = s.targetY;
                cursorP = stepPlane;
            }
            else
            {
                js["kind"]            = "transition";
                js["transitionIndex"] = s.transitionIndex;
                if (s.transitionIndex < transitions.size())
                {
                    const ww::format::TransitionRecord &tx = transitions[s.transitionIndex];
                    js["transitionKind"] = transitionKindName(tx.kind);
                    js["destX"]          = tx.destX;
                    js["destY"]          = tx.destY;
                    js["destPlane"]      = static_cast<int32_t>(tx.destPlane);
                    js["isGlobal"] =
                        (tx.flags & ww::format::kTransitionFlagGlobalOrigin) != 0;
                    if (tx.objectId >= 0)
                    {
                        js["objectId"] = tx.objectId;
                    }
                    cursorX = tx.destX;
                    cursorY = tx.destY;
                    cursorP = static_cast<int32_t>(tx.destPlane);
                }
                else
                {
                    cursorX = s.targetX;
                    cursorY = s.targetY;
                    cursorP = stepPlane;
                }
            }
            steps.push_back(js);
        }
        doc["steps"] = steps;
        return doc;
    }

    bool writeOutput(const char *outPath, const std::string &payload)
    {
        if (outPath == nullptr)
        {
            std::fputs(payload.c_str(), stdout);
            std::fputc('\n', stdout);
            return true;
        }
        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            std::fprintf(stderr, "path: failed to open %s for writing\n", outPath);
            return false;
        }
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        out.put('\n');
        return out.good();
    }
}

int runPathExport(int argc, char **argv)
{
    Args args{};
    if (!parseArgs(argc, argv, args))
    {
        printUsage();
        return 1;
    }

    try
    {
        const ww::format::ArtifactReader reader(args.artifactPath);
        ww::runtime::WorldView    view(reader);
        ww::runtime::AreaSearch   areaSearch(reader);
        ww::runtime::TileSearch   tileSearch(view);
        ww::runtime::PathAssembler assembler(reader, view, areaSearch, tileSearch);

        ww::runtime::CapabilitySnapshot snapshot;
        buildPermissiveSnapshot(reader, snapshot);

        ww::runtime::Plan plan;
        const bool ok =
            assembler.assemble(args.startX, args.startY, args.startPlane,
                               args.goalX,  args.goalY,  args.goalPlane,
                               &snapshot, plan);
        if (!ok)
        {
            std::fprintf(stderr, "path: no route from (%d,%d,p%d) to (%d,%d,p%d)\n",
                         args.startX, args.startY, args.startPlane,
                         args.goalX,  args.goalY,  args.goalPlane);
            return 2;
        }

        const nlohmann::ordered_json doc =
            buildJson(args, plan, tileSearch, view, reader.transitions());
        const std::string payload = doc.dump(2);
        if (!writeOutput(args.outPath, payload))
        {
            return 1;
        }
        std::fprintf(stderr, "path: %zu steps, cost=%.1f -> %s\n", plan.steps.size(),
                     static_cast<double>(plan.cost),
                     args.outPath != nullptr ? args.outPath : "stdout");
        return 0;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "path: %s\n", e.what());
        return 1;
    }
}
