#ifndef WORLDWALKER_BUILD_ARTIFACTWRITER_H
#define WORLDWALKER_BUILD_ARTIFACTWRITER_H

#include "build/CollisionBuilder.h"

#include <cstdint>
#include <string>

namespace ww::build
{
    // Serialize a baked artifact (currently the collision section only) to
    // `path`. cacheRevision is recorded in the header (0 if unknown). Throws
    // std::runtime_error on a compression or I/O failure.
    void writeArtifact(const std::string &path, const CollisionModel &collision,
                       uint32_t cacheRevision);
}

#endif  // WORLDWALKER_BUILD_ARTIFACTWRITER_H
