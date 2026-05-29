#ifndef WORLDWALKER_BUILD_ALTLANDMARKS_H
#define WORLDWALKER_BUILD_ALTLANDMARKS_H

#include "build/AreaGraph.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ww::build
{
    // Precomputed ALT landmark distance tables over the area graph. `landmarks`
    // holds the chosen landmark area ids; `fromLandmark` (landmark -> area) and
    // `toLandmark` (area -> landmark) are landmark-major float tick costs indexed
    // [landmarkIndex * areaCount + area], with +infinity where a landmark cannot
    // reach (or be reached from) an area. The planner reads these to bound
    // area-to-area distance via the triangle inequality.
    struct AltLandmarksModel
    {
        uint32_t areaCount{};
        std::vector<int32_t> landmarks;   // landmark area ids
        std::vector<float> fromLandmark;  // landmark -> area
        std::vector<float> toLandmark;    // area -> landmark
    };

    // Build-log accounting for buildAltLandmarks.
    struct AltLandmarksReport
    {
        std::size_t landmarkCount{};
        std::size_t areaCount{};
        std::size_t candidateAreas{};  // areas incident to at least one edge
        std::size_t reachablePairs{};  // finite entries across both tables
    };

    // Choose up to `landmarkCount` landmark areas (spatial farthest-point over
    // edge-incident areas) and run Dijkstra from each over the directed area
    // graph in both directions. *outReport (nullable) receives the counts. The
    // returned model is empty when the graph has no edges.
    AltLandmarksModel buildAltLandmarks(const AreaGraphModel &graph,
                                        std::size_t landmarkCount,
                                        AltLandmarksReport *outReport);
}

#endif  // WORLDWALKER_BUILD_ALTLANDMARKS_H
