#include "cli/Bench.h"

#include "format/ArtifactReader.h"
#include "runtime/AreaSearch.h"
#include "runtime/PathAssembler.h"
#include "runtime/TileSearch.h"
#include "runtime/WorldView.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <random>
#include <vector>

// Phase 6d — Query-latency benchmark + ADR 0002 input.
//
// The decision recorded in ADR 0002 (HPA*-clusters) was conditional on an
// empirical comparison with the alternative (a connected-component area
// graph) — only the area graph was actually built; the HPA*-cluster path
// remains a paper option. This bench measures the area graph as it exists
// today so the ADR can record concrete numbers when the decision is
// finalised. Pre-empting "yes but HPA* would be faster": the bench reports
// median/p99 of full `PathAssembler::assemble` calls — if the area graph
// already comfortably hits the planner's perf budget here, an HPA*-cluster
// rebuild does not earn its complexity at this scale.
namespace
{
    constexpr std::size_t kCasesPerCategory = 50;

    struct CategoryResult
    {
        const char  *name;
        std::size_t  cases;
        std::size_t  succeeded;
        double       medianUs;
        double       p99Us;
        double       meanUs;
    };

    // Nearest-rank percentile: ceil(q * N) - 1. For N=50, q=0.99 returns
    // samples[49], the maximum — the convention ADR 0002 implicitly relies
    // on ("p99 must remain under 250 ms"). The previous floor-based formula
    // returned samples[N-1 indexed at (N-1)*q], which for q=0.99,N=50 lands
    // on samples[48] — silently understating tail latency by one rank.
    double percentile(std::vector<double> samples, double q)
    {
        if (samples.empty())
        {
            return 0.0;
        }
        std::sort(samples.begin(), samples.end());
        const std::size_t n = samples.size();
        const double rank = q * static_cast<double>(n);
        std::size_t idx = rank <= 1.0 ? 0u : static_cast<std::size_t>(rank + 0.999999) - 1u;
        if (idx >= n) { idx = n - 1; }
        return samples[idx];
    }

    double mean(const std::vector<double> &samples)
    {
        if (samples.empty())
        {
            return 0.0;
        }
        double sum = 0.0;
        for (const double v : samples) { sum += v; }
        return sum / static_cast<double>(samples.size());
    }

    struct Pair { int sx; int sy; int sp; int gx; int gy; int gp; };

    // Same-area pairs: start = node centroid; goal = a random in-area tile
    // within ±32 of the centroid (the actual radius is constrained by the
    // node's footprint, so most goals end up much closer).
    std::vector<Pair> sampleSameArea(const ww::format::ArtifactReader &reader,
                                     ww::runtime::WorldView &view, std::mt19937 &rng)
    {
        const auto nodes = reader.areaNodes();
        std::vector<Pair> out;
        if (nodes.empty())
        {
            return out;
        }
        std::uniform_int_distribution<std::size_t> pickNode(0, nodes.size() - 1);
        std::uniform_int_distribution<int> radius(-32, 32);
        while (out.size() < kCasesPerCategory)
        {
            const ww::format::AreaNodeRecord &node = nodes[pickNode(rng)];
            const int plane = static_cast<int>(node.plane);
            const int area  = view.areaAt(node.centroidX, node.centroidY, plane);
            if (area < 0)
            {
                continue;
            }
            const int gx = node.centroidX + radius(rng);
            const int gy = node.centroidY + radius(rng);
            if (!view.isStandable(gx, gy, plane) || view.areaAt(gx, gy, plane) != area)
            {
                continue;
            }
            out.push_back({node.centroidX, node.centroidY, plane, gx, gy, plane});
        }
        return out;
    }

    // Short-hop pairs: pick a random area edge, use its from-area centroid
    // as the start and the to-area centroid as the goal. Each case is
    // guaranteed to span at least one area hop.
    std::vector<Pair> sampleShortHop(const ww::format::ArtifactReader &reader,
                                     ww::runtime::WorldView &view, std::mt19937 &rng)
    {
        const auto nodes = reader.areaNodes();
        const auto edges = reader.areaEdges();
        std::vector<Pair> out;
        if (nodes.empty() || edges.empty())
        {
            return out;
        }
        std::uniform_int_distribution<std::size_t> pickEdge(0, edges.size() - 1);
        std::size_t attempts = 0;
        while (out.size() < kCasesPerCategory && attempts < kCasesPerCategory * 10)
        {
            ++attempts;
            const ww::format::AreaEdgeRecord &edge = edges[pickEdge(rng)];
            if (static_cast<std::size_t>(edge.fromArea) >= nodes.size()
                || static_cast<std::size_t>(edge.toArea)   >= nodes.size())
            {
                continue;
            }
            const ww::format::AreaNodeRecord &fromNode = nodes[edge.fromArea];
            const ww::format::AreaNodeRecord &toNode   = nodes[edge.toArea];
            if (view.areaAt(fromNode.centroidX, fromNode.centroidY,
                            static_cast<int>(fromNode.plane)) != edge.fromArea
                || view.areaAt(toNode.centroidX, toNode.centroidY,
                               static_cast<int>(toNode.plane)) != edge.toArea)
            {
                continue;
            }
            out.push_back({fromNode.centroidX, fromNode.centroidY,
                           static_cast<int>(fromNode.plane),
                           toNode.centroidX, toNode.centroidY,
                           static_cast<int>(toNode.plane)});
        }
        return out;
    }

    // Long pairs: pick two random area nodes whose centroids resolve to
    // their declared areas. The pair is accepted regardless of whether the
    // assemble eventually finds a route — unreachable cases still measure
    // the planner's failure-path cost, which is part of the budget.
    std::vector<Pair> sampleLong(const ww::format::ArtifactReader &reader,
                                 ww::runtime::WorldView &view, std::mt19937 &rng)
    {
        const auto nodes = reader.areaNodes();
        std::vector<Pair> out;
        if (nodes.size() < 2)
        {
            return out;
        }
        std::uniform_int_distribution<std::size_t> pick(0, nodes.size() - 1);
        std::size_t attempts = 0;
        while (out.size() < kCasesPerCategory && attempts < kCasesPerCategory * 20)
        {
            ++attempts;
            const std::size_t ia = pick(rng);
            const std::size_t ib = pick(rng);
            // Reject self-pairs: same node would route through the same-area
            // refinement path and inflate the success rate with same_area
            // samples that don't belong in the long bucket.
            if (ia == ib)
            {
                continue;
            }
            const ww::format::AreaNodeRecord &a = nodes[ia];
            const ww::format::AreaNodeRecord &b = nodes[ib];
            const int ap = static_cast<int>(a.plane);
            const int bp = static_cast<int>(b.plane);
            if (view.areaAt(a.centroidX, a.centroidY, ap) < 0
                || view.areaAt(b.centroidX, b.centroidY, bp) < 0)
            {
                continue;
            }
            out.push_back({a.centroidX, a.centroidY, ap, b.centroidX, b.centroidY, bp});
        }
        return out;
    }

    CategoryResult runCategory(const char *name, const std::vector<Pair> &pairs,
                               ww::runtime::PathAssembler &assembler)
    {
        CategoryResult result{name, pairs.size(), 0, 0.0, 0.0, 0.0};
        std::vector<double> samples;
        samples.reserve(pairs.size());
        for (const Pair &p : pairs)
        {
            ww::runtime::Plan plan;
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = assembler.assemble(p.sx, p.sy, p.sp, p.gx, p.gy, p.gp, plan);
            const auto t1 = std::chrono::steady_clock::now();
            const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            samples.push_back(us);
            result.succeeded += ok ? 1u : 0u;
        }
        result.medianUs = percentile(samples, 0.50);
        result.p99Us    = percentile(samples, 0.99);
        result.meanUs   = mean(samples);
        return result;
    }
}

int runBench(const char *wwaPath)
{
    try
    {
        const ww::format::ArtifactReader reader(wwaPath);
        ww::runtime::WorldView    view(reader);
        ww::runtime::AreaSearch   areaSearch(reader);
        ww::runtime::TileSearch   tileSearch(view);
        ww::runtime::PathAssembler assembler(reader, view, areaSearch, tileSearch);

        std::printf("bench: %s\n", wwaPath);
        std::printf("bench: areas=%zu edges=%zu transitions=%zu landmarks=%u\n",
                    reader.areaNodes().size(), reader.areaEdges().size(),
                    reader.transitions().size(), reader.landmarkCount());

        std::mt19937 rng(0xC0FFEEu);
        const std::vector<Pair> sameArea = sampleSameArea(reader, view, rng);
        const std::vector<Pair> shortHop = sampleShortHop(reader, view, rng);
        const std::vector<Pair> longCase = sampleLong(reader, view, rng);

        // Warm-up pass so cold caches (page-faulted ALT tables, area-id
        // grids not yet decompressed) don't dominate the first measured
        // case in each category. Previously only sameArea was warmed, which
        // left short_hop and long paying that cost in their first sample
        // and skewed mean / p99 high. Each category now warms its own
        // representative pairs.
        auto warm = [&assembler](const std::vector<Pair> &pairs)
        {
            for (std::size_t i = 0; i < 3 && i < pairs.size(); ++i)
            {
                ww::runtime::Plan plan;
                assembler.assemble(pairs[i].sx, pairs[i].sy, pairs[i].sp,
                                   pairs[i].gx, pairs[i].gy, pairs[i].gp, plan);
            }
        };
        warm(sameArea);
        warm(shortHop);
        warm(longCase);

        const std::vector<CategoryResult> results = {
            runCategory("same_area",  sameArea, assembler),
            runCategory("short_hop",  shortHop, assembler),
            runCategory("long",       longCase, assembler),
        };

        std::printf("bench: %-12s %5s %6s %10s %10s %10s\n",
                    "category", "n", "ok", "median(us)", "mean(us)", "p99(us)");
        for (const CategoryResult &r : results)
        {
            std::printf("bench: %-12s %5zu %6zu %10.1f %10.1f %10.1f\n", r.name, r.cases,
                        r.succeeded, r.medianUs, r.meanUs, r.p99Us);
        }
        return 0;
    }
    catch (const std::exception &e)
    {
        std::printf("bench: failed: %s\n", e.what());
        return 1;
    }
}
