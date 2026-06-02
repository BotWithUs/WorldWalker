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

        // The ALT landmark distance must be the TRUE min-cost in the area
        // graph + the global-teleport set, otherwise the planner's
        // triangle-inequality bound becomes inadmissible — a query whose true
        // shortest path involves a teleport gets an over-estimating heuristic
        // and A* returns sub-optimal plans.
        //
        // We model teleports through one virtual "hub" node at index
        // areaCount. Forward edges:   every A -> Hub at cost 0
        //                             Hub -> tx.destArea at cost tx.cost
        // Reverse edges:              Hub -> every A at cost 0
        //                             tx.destArea -> Hub at cost tx.cost
        //
        // Forward Dijkstra from L: walks to any A, 0 to Hub, teleport to dest;
        // arbitrarily many hubs/teleports compose naturally. Reverse Dijkstra
        // from L: a path A -> Hub -> ... -> L lets any A reach L via teleport.
        // The output tables ignore the hub index (areaCount), so the runtime's
        // h() reads only area-indexed entries.
        constexpr int32_t kHubOffset = 0;  // hub index = areaCount + kHubOffset

        void buildAdjacency(const AreaGraphModel &graph,
                            std::vector<std::vector<Edge>> &outForward,
                            std::vector<std::vector<Edge>> &outReverse)
        {
            const std::size_t areaCount = graph.nodes.size();
            const int32_t hub = static_cast<int32_t>(areaCount) + kHubOffset;
            outForward.assign(areaCount + 1u, {});
            outReverse.assign(areaCount + 1u, {});

            // Real (walk) edges.
            for (const AreaEdge &edge : graph.edges)
            {
                outForward[static_cast<std::size_t>(edge.fromArea)].push_back({edge.toArea, edge.cost});
                outReverse[static_cast<std::size_t>(edge.toArea)].push_back({edge.fromArea, edge.cost});
            }
            // Virtual hub edges. A -> Hub (forward) and Hub -> A (reverse) at
            // cost 0 model "I can begin / I just finished a teleport from
            // anywhere". The teleport itself is Hub -> destArea forward and
            // destArea -> Hub reverse at the teleport's tick cost.
            for (std::size_t a = 0; a < areaCount; ++a)
            {
                outForward[a].push_back({hub, 0.0f});
                outReverse[hub].push_back({static_cast<int32_t>(a), 0.0f});
            }
            for (const GlobalTeleport &tx : graph.globalTeleports)
            {
                if (tx.destArea < 0 || static_cast<std::size_t>(tx.destArea) >= areaCount)
                {
                    continue;
                }
                outForward[hub].push_back({tx.destArea, tx.cost});
                outReverse[static_cast<std::size_t>(tx.destArea)].push_back({hub, tx.cost});
            }
        }

        // Areas incident to at least one edge OR a global teleport destination
        // — the only areas reachable from elsewhere in the abstract graph, so
        // the only sensible landmark candidates.
        std::vector<int32_t> collectCandidates(const AreaGraphModel &graph)
        {
            std::vector<uint8_t> incident(graph.nodes.size(), 0u);
            for (const AreaEdge &edge : graph.edges)
            {
                incident[static_cast<std::size_t>(edge.fromArea)] = 1u;
                incident[static_cast<std::size_t>(edge.toArea)] = 1u;
            }
            for (const GlobalTeleport &tx : graph.globalTeleports)
            {
                if (tx.destArea >= 0
                    && static_cast<std::size_t>(tx.destArea) < incident.size())
                {
                    incident[static_cast<std::size_t>(tx.destArea)] = 1u;
                }
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

        // Dijkstra over the augmented graph (areaCount + 1 nodes — the last is
        // the virtual teleport hub). Output tables ignore the hub by copying
        // only the first areaCount entries.
        std::vector<float> dist(areaCount + 1u);
        for (std::size_t l = 0; l < chosen; ++l)
        {
            const int32_t landmark = model.landmarks[l];
            dijkstra(forward, landmark, dist);
            std::copy(dist.begin(), dist.begin() + static_cast<std::ptrdiff_t>(areaCount),
                      model.fromLandmark.begin() + static_cast<std::ptrdiff_t>(l * areaCount));
            dijkstra(reverse, landmark, dist);
            std::copy(dist.begin(), dist.begin() + static_cast<std::ptrdiff_t>(areaCount),
                      model.toLandmark.begin() + static_cast<std::ptrdiff_t>(l * areaCount));
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
