#ifndef WORLDWALKER_DATA_DATASETLOADER_H
#define WORLDWALKER_DATA_DATASETLOADER_H

#include "data/DialogZones.h"
#include "data/Transitions.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ww::data
{
    // One dataset file that was actually read, for the bake's provenance record.
    // `fingerprint` is FNV-1a 64 over the file's bytes — deliberately not called a
    // checksum: it identifies a version of the file for review and change
    // detection, and is not a defence against anyone crafting a collision.
    struct DatasetFileInfo
    {
        std::string name;
        std::uint64_t bytes{};
        std::uint64_t fingerprint{};
    };

    struct LoadedDatasets
    {
        TransitionModel model;     // raw transitions: pre-snap, pre-dedup, pre-cost
        DialogZonesModel dialogZones;  // dialog_zones.json, baked as-is
        uint32_t datasetHash{};    // FNV-1a over the bytes of the files that were present
        std::size_t filesFound{};   // dataset files that were present and parsed
        std::size_t filesMissing{}; // dataset files that were not found (warned + skipped)
        // The files behind filesFound, in load order. Same set datasetHash was
        // computed over, itemised so a bake can record what it actually read.
        std::vector<DatasetFileInfo> files;
    };

    // Parse the four transition datasets in `directory` (transport_links.json,
    // teleport_chains.json, spell_teleports.json, item_teleports.json) into a raw
    // transition model, and dialog_zones.json into `dialogZones`. A missing
    // file is warned about (stderr), skipped, and
    // counted in filesMissing (present ones in filesFound) so the caller can tell
    // a partial dataset from a wrong directory (wwbuild refuses the latter). Throws (a nlohmann json
    // exception, derived from std::exception) on malformed JSON in a present file.
    LoadedDatasets loadDatasets(const std::string &directory);

    // Parse ONLY the global-origin teleport datasets in `directory`
    // (spell_teleports.json + item_teleports.json) into a raw transition model,
    // keeping only global-origin transitions. Used by the runtime to load
    // scripter-editable teleports without re-baking; transport_links /
    // teleport_chains are skipped (those are local, baked into the area graph).
    // Missing files are skipped; throws on malformed JSON in a present file.
    LoadedDatasets loadGlobalTeleports(const std::string &directory);
}

#endif  // WORLDWALKER_DATA_DATASETLOADER_H
