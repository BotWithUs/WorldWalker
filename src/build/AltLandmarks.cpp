#include "build/AltLandmarks.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

namespace ww::build
{
    namespace
    {
        struct Edge
        {
            int32_t to;
            float cost;
        };

        constexpr float kInfinity = std::numeric_limits<float>::infinity();

        // Forward (fromArea -> toArea) and reverse (toArea -> fromArea) adjacency
        // over the directed area graph, indexed by area id.
        void buildAdjacency(const AreaGraphModel &graph,
                            std::vector<std::vector<Edge>> &outForward,
                            std::vector<std::vector<Edge>> &outReverse)
        {
            outForward.assign(graph.nodes.size(), {});
            outReverse.assign(graph.nodes.size(), {});
            for (const AreaEdge &edge : graph.edges)
            {
                outForward[static_cast<std::size_t>(edge.fromArea)].push_back({edge.toArea, edge.cost});
                outReverse[static_cast<std::size_t>(edge.toArea)].push_back({edge.fromArea, edge.cost});
            }
        }

        // Areas incident to at least one edge — the only areas that can take part
        // in an abstract (multi-area) route, so the only sensible landmarks.
        std::vector<int32_t> collectCandidates(const AreaGraphModel &graph)
        {
            std::vector<uint8_t> incident(graph.nodes.size(), 0u);
            for (const AreaEdge &edge : graph.edges)
            {
                incident[static_cast<std::size_t>(edge.fromArea)] = 1u;
                incident[static_cast<std::size_t>(edge.toArea)] = 1u;
            }
            std::vector<int32_t> candidates;
            for (std::size_t i = 0; i < incident.size(); ++i)
            {
                if (incident[i] != 0u)
                {
                    candidates.push_back(static_cast<int32_t>(i));
                }
            }
            return candidates;
        }

        double centroidDist2(const AreaNode &a, const AreaNode &b)
        {
            const double dx = static_cast<double>(a.centroidX) - static_cast<double>(b.centroidX);
            const double dy = static_cast<double>(a.centroidY) - static_cast<double>(b.centroidY);
            return dx * dx + dy * dy;
        }

        bool centroidLess(const AreaNode &a, const AreaNode &b)
        {
            if (a.centroidX != b.centroidX)
            {
                return a.centroidX < b.centroidX;
            }
            return a.centroidY < b.centroidY;
        }

        // Seed the farthest-point set with a deterministic corner (the candidate
        // with the smallest centroid), so the build is reproducible.
        std::size_t pickSeed(const std::vector<AreaNode> &nodes, const std::vector<int32_t> &candidates)
        {
            std::size_t seed = 0;
            for (std::size_t i = 1; i < candidates.size(); ++i)
            {
                if (centroidLess(nodes[static_cast<std::size_t>(candidates[i])],
                                 nodes[static_cast<std::size_t>(candidates[seed])]))
                {
                    seed = i;
                }
            }
            return seed;
        }

        // Greedy farthest-point sampling over candidate centroids: repeatedly add
        // the candidate maximizing its minimum distance to the chosen set. Cheap
        // and gives well-spread landmarks, which is what the ALT bound wants.
        std::vector<int32_t> selectLandmarks(const std::vector<AreaNode> &nodes,
                                             const std::vector<int32_t> &candidates, std::size_t want)
        {
            std::vector<int32_t> chosen;
            if (candidates.empty() || want == 0)
            {
                return chosen;
            }
            const std::size_t target = std::min(want, candidates.size());
            chosen.push_back(candidates[pickSeed(nodes, candidates)]);

            std::vector<double> minDist2(candidates.size());
            for (std::size_t i = 0; i < candidates.size(); ++i)
            {
                minDist2[i] = centroidDist2(nodes[static_cast<std::size_t>(candidates[i])],
                                            nodes[static_cast<std::size_t>(chosen.back())]);
            }
            while (chosen.size() < target)
            {
                const auto best = std::max_element(minDist2.begin(), minDist2.end());
                const std::size_t bestIdx = static_cast<std::size_t>(best - minDist2.begin());
                chosen.push_back(candidates[bestIdx]);
                for (std::size_t i = 0; i < candidates.size(); ++i)
                {
                    minDist2[i] = std::min(minDist2[i],
                                           centroidDist2(nodes[static_cast<std::size_t>(candidates[i])],
                                                         nodes[static_cast<std::size_t>(chosen.back())]));
                }
            }
            return chosen;
        }

        void dijkstra(const std::vector<std::vector<Edge>> &adjacency, int32_t source,
                      std::vector<float> &outDist)
        {
            std::fill(outDist.begin(), outDist.end(), kInfinity);
            outDist[static_cast<std::size_t>(source)] = 0.0f;

            using Node = std::pair<float, int32_t>;
            std::priority_queue<Node, std::vector<Node>, std::greater<Node>> frontier;
            frontier.push({0.0f, source});
            while (!frontier.empty())
            {
                const Node top = frontier.top();
                frontier.pop();
                if (top.first > outDist[static_cast<std::size_t>(top.second)])
                {
                    continue;
                }
                for (const Edge &edge : adjacency[static_cast<std::size_t>(top.second)])
                {
                    const float candidate = top.first + edge.cost;
                    if (candidate < outDist[static_cast<std::size_t>(edge.to)])
                    {
                        outDist[static_cast<std::size_t>(edge.to)] = candidate;
                        frontier.push({candidate, edge.to});
                    }
                }
            }
        }

        std::size_t countFinite(const std::vector<float> &table)
        {
            std::size_t finite = 0;
            for (float value : table)
            {
                if (value != kInfinity)
                {
                    ++finite;
                }
            }
            return finite;
        }
    }

    AltLandmarksModel buildAltLandmarks(const AreaGraphModel &graph, std::size_t landmarkCount,
                                        AltLandmarksReport *outReport)
    {
        AltLandmarksModel model;
        const std::size_t areaCount = graph.nodes.size();
        model.areaCount = static_cast<uint32_t>(areaCount);

        std::vector<std::vector<Edge>> forward;
        std::vector<std::vector<Edge>> reverse;
        buildAdjacency(graph, forward, reverse);

        const std::vector<int32_t> candidates = collectCandidates(graph);
        model.landmarks = selectLandmarks(graph.nodes, candidates, landmarkCount);

        const std::size_t chosen = model.landmarks.size();
        model.fromLandmark.resize(chosen * areaCount);
        model.toLandmark.resize(chosen * areaCount);

        std::vector<float> dist(areaCount);
        for (std::size_t l = 0; l < chosen; ++l)
        {
            const int32_t landmark = model.landmarks[l];
            dijkstra(forward, landmark, dist);
            std::copy(dist.begin(), dist.end(), model.fromLandmark.begin() + static_cast<std::ptrdiff_t>(l * areaCount));
            dijkstra(reverse, landmark, dist);
            std::copy(dist.begin(), dist.end(), model.toLandmark.begin() + static_cast<std::ptrdiff_t>(l * areaCount));
        }

        if (outReport != nullptr)
        {
            outReport->landmarkCount = chosen;
            outReport->areaCount = areaCount;
            outReport->candidateAreas = candidates.size();
            outReport->reachablePairs = countFinite(model.fromLandmark) + countFinite(model.toLandmark);
        }
        return model;
    }
}
