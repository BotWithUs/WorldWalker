#ifndef WORLDWALKER_BUILD_ARTIFACTWRITER_H
#define WORLDWALKER_BUILD_ARTIFACTWRITER_H

#include "build/AltLandmarks.h"
#include "build/AreaGraph.h"
#include "build/CollisionBuilder.h"
#include "data/DialogZones.h"
#include "data/TeleportZones.h"
#include "data/Transitions.h"

#include <cstdint>
#include <string>

namespace ww::build
{
    // Everything the artifact records about itself rather than about the world.
    // Grouped into a struct rather than trailing parameters because the list was
    // already two longs and a third would make the call site unreadable.
    struct ArtifactMeta
    {
        // Legacy cache-directory mtime hash. Carried through unchanged and not
        // to be trusted — see the ArtifactHeader comment in format/Artifact.h.
        uint32_t cacheRevision{};
        uint32_t datasetHash{};
        // Provenance document (compact JSON). Empty omits the section entirely,
        // which is what a caller that has nothing honest to record should pass:
        // an absent section reads as "unknown", an invented one does not.
        std::string provenanceJson;
    };

    // Serialize a baked artifact to `path`: always a Collision section, plus a
    // Transitions section when `transitions` is non-empty, an Abstraction section
    // when `abstraction` has areas, an AltLandmarks section when `altLandmarks`
    // has landmarks, a TeleportAllowed section when `teleportZones` has any
    // wilderness region or no-tele zone, a DialogZones section when
    // `dialogZones` has any zone, and a Provenance section when
    // `meta.provenanceJson` is non-empty. Throws std::runtime_error on a
    // compression or I/O failure.
    void writeArtifact(const std::string &path, const CollisionModel &collision,
                       const ww::data::TransitionModel &transitions,
                       const AreaGraphModel &abstraction,
                       const AltLandmarksModel &altLandmarks,
                       const ww::data::TeleportZonesModel &teleportZones,
                       const ww::data::DialogZonesModel &dialogZones,
                       const ArtifactMeta &meta);
}

#endif  // WORLDWALKER_BUILD_ARTIFACTWRITER_H
