#ifndef WORLDWALKER_DATA_DATASETLOADER_H
#define WORLDWALKER_DATA_DATASETLOADER_H

#include "data/Transitions.h"

#include <cstdint>
#include <string>

namespace ww::data
{
    struct LoadedDatasets
    {
        TransitionModel model;     // raw transitions: pre-snap, pre-dedup, pre-cost
        uint32_t datasetHash{};    // FNV-1a over the bytes of the files that were present
    };

    // Parse the four transition datasets in `directory` (transport_links.json,
    // teleport_chains.json, spell_teleports.json, item_teleports.json) into a raw
    // transition model. A missing file is warned about (stderr) and skipped, so a
    // partial dataset still produces an artifact. Throws (a nlohmann json
    // exception, derived from std::exception) on malformed JSON in a present file.
    LoadedDatasets loadDatasets(const std::string &directory);
}

#endif  // WORLDWALKER_DATA_DATASETLOADER_H
