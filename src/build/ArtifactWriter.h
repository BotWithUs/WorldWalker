#ifndef WORLDWALKER_BUILD_ARTIFACTWRITER_H
#define WORLDWALKER_BUILD_ARTIFACTWRITER_H

#include "build/AreaGraph.h"
#include "build/CollisionBuilder.h"
#include "data/Transitions.h"

#include <cstdint>
#include <string>

namespace ww::build
{
    // Serialize a baked artifact to `path`: always a Collision section, plus a
    // Transitions section when `transitions` is non-empty and an Abstraction
    // section when `abstraction` has areas. cacheRevision and datasetHash are
    // recorded in the header (0 when unknown). Throws std::runtime_error on a
    // compression or I/O failure.
    void writeArtifact(const std::string &path, const CollisionModel &collision,
                       const ww::data::TransitionModel &transitions,
                       const AreaGraphModel &abstraction,
                       uint32_t cacheRevision, uint32_t datasetHash);
}

#endif  // WORLDWALKER_BUILD_ARTIFACTWRITER_H
