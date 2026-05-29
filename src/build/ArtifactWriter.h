#ifndef WORLDWALKER_BUILD_ARTIFACTWRITER_H
#define WORLDWALKER_BUILD_ARTIFACTWRITER_H

#include "build/CollisionBuilder.h"
#include "data/Transitions.h"

#include <cstdint>
#include <string>

namespace ww::build
{
    // Serialize a baked artifact to `path`: always a Collision section, plus a
    // Transitions section when `transitions` is non-empty. cacheRevision and
    // datasetHash are recorded in the header (0 when unknown). Throws
    // std::runtime_error on a compression or I/O failure.
    void writeArtifact(const std::string &path, const CollisionModel &collision,
                       const ww::data::TransitionModel &transitions,
                       uint32_t cacheRevision, uint32_t datasetHash);
}

#endif  // WORLDWALKER_BUILD_ARTIFACTWRITER_H
