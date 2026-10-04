#include "cli/MoveTests.h"

#include "c_api/worldwalker_c.h"
#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "format/MoveCategory.h"
#include "runtime/RuntimeTeleports.h"
#include "runtime/TileScan.h"
#include "runtime/WorldView.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <span>
#include <string>
#include <vector>

namespace
{
    using ww::format::MoveCategory;

    constexpr std::size_t kExamplesPerCategory = 3;

    // Queries tried per category before it is reported as having no case:
    // candidate transitions, and for a global, far starts per transition.
    constexpr std::size_t kMaxCandidates = 40;
    constexpr std::size_t kMaxGlobalStarts = 12;

    // A global teleport only wins over walking from far away.
    constexpr std::int32_t kMinGlobalSpread = 300;

    const char *categoryName(std::uint32_t c)
    {
        return ww::format::moveCategoryName(static_cast<MoveCategory>(c));
    }

    std::int32_t spread(std::int32_t ax, std::int32_t ay, std::int32_t bx, std::int32_t by)
    {
        return std::max(std::abs(ax - bx), std::abs(ay - by));
    }

    // One query the mask test runs.
    struct MoveCase
    {
        WwTile start;
        WwGoal goal;
    };

    // RAII over the C ABI handles the test drives.
    struct CHandles
    {
        ww_artifact     *artifact{nullptr};
        ww_context_pool *pool{nullptr};

        CHandles(const char *wwaPath, const char *teleportDir)
        {
            artifact = ww_artifact_open(wwaPath);
            if (artifact != nullptr && teleportDir != nullptr)
            {
                ww_artifact_load_teleports(artifact, teleportDir);
            }
            pool = artifact != nullptr ? ww_context_pool_create(artifact, 1) : nullptr;
        }

        ~CHandles()
        {
            ww_context_pool_destroy(pool);
            ww_artifact_close(artifact);
        }

        CHandles(const CHandles &) = delete;
        CHandles &operator=(const CHandles &) = delete;
    };

    // A planned path, copied out of the C ABI's buffer. isFound is false when
    // the query found no route.
    struct Planned
    {
        bool                isFound{false};
        std::vector<WwStep> steps;
        float               cost{0.0f};
    };

    Planned queryMoves(CHandles &h, const MoveCase &mc, std::uint32_t mask)
    {
        WwPath path{};
        Planned out;
        out.isFound = ww_query_moves(h.artifact, h.pool, mc.start, mc.goal, nullptr, nullptr,
                                     &path, mask) == WW_OK;
        out.steps.assign(path.steps, path.steps + path.stepCount);
        out.cost = path.cost;
        ww_path_free(&path);
        return out;
    }

    Planned queryEx(CHandles &h, const MoveCase &mc)
    {
        WwPath path{};
        Planned out;
        out.isFound = ww_query_ex(h.artifact, h.pool, mc.start, mc.goal, nullptr, nullptr,
                                  &path) == WW_OK;
        out.steps.assign(path.steps, path.steps + path.stepCount);
        out.cost = path.cost;
        ww_path_free(&path);
        return out;
    }

    bool isSamePath(const Planned &a, const Planned &b)
    {
        return a.isFound == b.isFound && a.cost == b.cost && a.steps.size() == b.steps.size()
            && (a.steps.empty()
                || std::memcmp(a.steps.data(), b.steps.data(),
                               a.steps.size() * sizeof(WwStep)) == 0);
    }

    // How many Transition steps of `p` are in category `c`.
    std::size_t countCategory(const Planned &p, std::span<const std::uint8_t> categories,
                              std::uint32_t c)
    {
        std::size_t n = 0;
        for (const WwStep &s : p.steps)
        {
            const bool isInCategory = s.kind == WW_STEP_KIND_TRANSITION
                && s.transitionIndex < categories.size() && categories[s.transitionIndex] == c;
            n += isInCategory ? 1u : 0u;
        }
        return n;
    }

    // The query from a standable tile beside a local transition's origin to
    // its destination; false when the origin has no standable neighbour.
    bool localCase(ww::runtime::WorldView &view, const ww::format::TransitionRecord &tx,
                   MoveCase &outCase)
    {
        const std::int32_t plane = static_cast<std::int32_t>(tx.originPlane);
        const auto isStandable = [&](std::int32_t x, std::int32_t y)
        {
            return view.isStandable(x, y, plane);
        };
        std::int32_t x = 0;
        std::int32_t y = 0;
        if (!ww::runtime::findNearestTile(tx.originX, tx.originY, ww::data::kTransitionApproachRadius,
                                          true, isStandable, tx.originX, tx.originY, x, y))
        {
            return false;
        }
        outCase.start = WwTile{ x, y, plane };
        outCase.goal = WwGoal{ tx.destX, tx.destY, static_cast<std::int32_t>(tx.destPlane), 0 };
        return true;
    }

    // Candidate queries for category `c`: a local transition of it, from
    // beside its origin to its destination; a global one, from the centroids
    // of areas far from where it lands.
    std::vector<MoveCase> casesFor(const ww::format::ArtifactReader &reader,
                                   ww::runtime::WorldView &view, std::uint32_t c)
    {
        std::vector<MoveCase> cases;
        const auto txs = reader.transitions();
        const auto categories = reader.moveCategories();
        const auto nodes = reader.areaNodes();
        for (std::size_t i = 0; i < txs.size() && cases.size() < kMaxCandidates; ++i)
        {
            const ww::format::TransitionRecord &tx = txs[i];
            if (categories[i] != c)
            {
                continue;
            }
            MoveCase mc{};
            if ((tx.flags & ww::format::kTransitionFlagGlobalOrigin) == 0u)
            {
                if (localCase(view, tx, mc))
                {
                    cases.push_back(mc);
                }
                continue;
            }
            std::size_t starts = 0;
            for (std::size_t a = 0; a < nodes.size() && starts < kMaxGlobalStarts; a += 97)
            {
                const ww::format::AreaNodeRecord &n = nodes[a];
                const bool isFar = n.plane == 0 && n.tileCount >= 256
                    && spread(n.centroidX, n.centroidY, tx.destX, tx.destY) >= kMinGlobalSpread
                    && view.isStandable(n.centroidX, n.centroidY, 0);
                if (!isFar)
                {
                    continue;
                }
                cases.push_back(MoveCase{ WwTile{ n.centroidX, n.centroidY, 0 },
                                          WwGoal{ tx.destX, tx.destY,
                                                  static_cast<std::int32_t>(tx.destPlane), 0 } });
                ++starts;
            }
        }
        return cases;
    }

    // Outcome of one category's mask check.
    struct CategoryResult
    {
        bool     hasCase{false};
        bool     isPassed{false};
        MoveCase used{};
        Planned  off;
        Planned  on;
    };

    CategoryResult checkCategory(CHandles &h, const ww::format::ArtifactReader &reader,
                                 ww::runtime::WorldView &view, std::uint32_t c,
                                 std::size_t &ioAbiMismatches)
    {
        CategoryResult r;
        const auto categories = reader.moveCategories();
        for (const MoveCase &mc : casesFor(reader, view, c))
        {
            Planned off = queryMoves(h, mc, 0u);
            ioAbiMismatches += isSamePath(off, queryEx(h, mc)) ? 0u : 1u;
            if (!off.isFound || countCategory(off, categories, c) == 0)
            {
                continue;
            }
            r.hasCase = true;
            r.used = mc;
            r.on = queryMoves(h, mc, 1u << c);
            r.isPassed = countCategory(r.on, categories, c) == 0;
            r.off = std::move(off);
            return r;
        }
        return r;
    }

    void printPlanned(const char *label, const Planned &p, std::span<const std::uint8_t> categories,
                      std::uint32_t c)
    {
        if (!p.isFound)
        {
            std::printf(" %s: no route", label);
            return;
        }
        std::printf(" %s: %zu steps cost %.1f uses %zu", label, p.steps.size(),
                    static_cast<double>(p.cost), countCategory(p, categories, c));
    }
}

int runMoveCategories(const char *wwaPath, const char *teleportDir)
{
    try
    {
        ww::format::ArtifactReader reader(wwaPath);
        const std::size_t baked = reader.transitions().size();
        if (teleportDir != nullptr)
        {
            ww::runtime::loadGlobalTeleportsInto(reader, teleportDir);
        }
        const auto txs = reader.transitions();
        const auto categories = reader.moveCategories();
        std::array<std::size_t, ww::format::kMoveCategoryCount> counts{};
        std::array<std::vector<std::size_t>, ww::format::kMoveCategoryCount> examples{};
        for (std::size_t i = 0; i < txs.size(); ++i)
        {
            const std::uint8_t c = categories[i];
            ++counts[c];
            if (examples[c].size() < kExamplesPerCategory)
            {
                examples[c].push_back(i);
            }
        }
        std::printf("categories: %zu transitions (%zu baked, %zu runtime teleports)\n",
                    txs.size(), baked, txs.size() - baked);
        for (std::uint32_t c = 0; c < ww::format::kMoveCategoryCount; ++c)
        {
            std::printf("  %2u %-14s %6zu", c, categoryName(c), counts[c]);
            for (const std::size_t i : examples[c])
            {
                const ww::format::TransitionRecord &tx = txs[i];
                std::printf("  tx%zu loc %d (%d,%d,p%u)->(%d,%d,p%u)", i, tx.objectId, tx.originX,
                            tx.originY, tx.originPlane, tx.destX, tx.destY, tx.destPlane);
            }
            std::printf("\n");
        }
        return 0;
    }
    catch (const std::exception &e)
    {
        std::printf("categories: %s\n", e.what());
        return 1;
    }
}

int runMoveTests(const char *wwaPath, const char *teleportDir)
{
    try
    {
        ww::format::ArtifactReader reader(wwaPath);
        if (teleportDir != nullptr)
        {
            ww::runtime::loadGlobalTeleportsInto(reader, teleportDir);
        }
        ww::runtime::WorldView view(reader);
        CHandles h(wwaPath, teleportDir);
        if (h.artifact == nullptr || h.pool == nullptr)
        {
            std::printf("moves: could not open %s through the C ABI: %s\n", wwaPath,
                        ww_last_error());
            return 1;
        }
        const auto categories = reader.moveCategories();
        std::size_t failures = 0;
        std::size_t abiMismatches = 0;
        std::size_t tested = 0;
        for (std::uint32_t c = 0; c < ww::format::kMoveCategoryCount; ++c)
        {
            const CategoryResult r = checkCategory(h, reader, view, c, abiMismatches);
            if (!r.hasCase)
            {
                std::printf("moves: %-14s skipped (no query whose route uses it)\n", categoryName(c));
                continue;
            }
            ++tested;
            std::printf("moves: %-14s %s (%d,%d,p%d)->(%d,%d,p%d)", categoryName(c),
                        r.isPassed ? "PASS" : "FAIL", r.used.start.x, r.used.start.y,
                        r.used.start.plane, r.used.goal.x, r.used.goal.y, r.used.goal.plane);
            printPlanned("off", r.off, categories, c);
            printPlanned(";  on", r.on, categories, c);
            std::printf("\n");
            failures += r.isPassed ? 0u : 1u;
        }
        std::printf("moves: %zu categories tested, %zu failed; ww_query_ex vs ww_query_moves(0)"
                    " mismatches=%zu (expect 0)\n", tested, failures, abiMismatches);
        return (failures == 0 && abiMismatches == 0) ? 0 : 1;
    }
    catch (const std::exception &e)
    {
        std::printf("moves: %s\n", e.what());
        return 1;
    }
}
