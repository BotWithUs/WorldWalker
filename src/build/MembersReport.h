#ifndef WORLDWALKER_BUILD_MEMBERSREPORT_H
#define WORLDWALKER_BUILD_MEMBERSREPORT_H

#include "data/Transitions.h"
#include "format/ArtifactReader.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ww::build
{
    // What the bake found while checking the free-to-play classification of a
    // just-written artifact (with the runtime teleports appended, exactly as a
    // host loads it).
    struct MembersCheck
    {
        std::size_t transitions{};
        std::size_t membersOnly{};
        // Expectations that failed, one line each. Any entry fails the bake.
        std::vector<std::string> failures;
        // Rows whose `"members"` flag disagrees with what geography and the
        // members-network rule alone would say. The flag wins; these are
        // printed so a reviewer can see each one is deliberate.
        std::vector<std::string> overridesAgainstGeography;
    };

    // Classify every transition of `reader` and hold it to what is known
    // independently of f2p_zones.json:
    //   - every charter ship row (family "Charter ship - ...") is members-only;
    //   - the 11 lodestones cache enum 12260 lists as free are free to play,
    //     and so is the land each one lands on;
    //   - the 15 lodestones enum 12261 lists as members are members-only;
    //   - any other lodestone carries an explicit `"members"` flag.
    // `labels` names every transition of `reader` by index (the dataset
    // family, empty for a cache-derived one); a shorter list is a caller bug
    // and throws std::invalid_argument. Prints the per-family counts.
    MembersCheck checkMembers(const format::ArtifactReader &reader,
                              const std::vector<std::string> &labels);

    void printMembersCheck(const MembersCheck &check);
}

#endif  // WORLDWALKER_BUILD_MEMBERSREPORT_H
