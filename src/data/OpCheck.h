#ifndef WORLDWALKER_DATA_OPCHECK_H
#define WORLDWALKER_DATA_OPCHECK_H

#include "data/Transitions.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// Bake-time check that a curated transition clicks an option its origin has.
//
// A transport_links row names a loc (or an NPC range) and a 0-based option
// index. The host clicks exactly that: when the loc has no option there, the
// click does nothing, the executor waits out its timeout, and only then routes
// around the row. Meanwhile the planner keeps choosing it, because nothing in
// the artifact says it cannot work. So a row whose origin lacks its option is
// dropped at bake time, where the reason is known, and reported by family.
//
// WHY NOT NXTCACHE. The bake already decodes the cache through NXTCacheLibrary,
// and its loc and npc getters return an `options` list. They are not used here
// because the decoder in the current NXTCacheLibrary loses the morph (multiloc /
// multinpc) table on every definition, and some definitions decode to defaults
// outright (loc 5492, a ladder, reads as nameless with no options). Judged
// against it, at least 89 valid rows would be dropped, 50 of them spirit trees whose patch
// loc only gains `Teleport` through a morph. The definitions come instead from
// the rs3-cs2-dumps export (`locations.json`, `npcs.json`: `actions` and
// `morphs_1` / `morphs_2`), which is regenerated from the live cache and keeps
// both.
namespace ww::data
{
    // The option slots a definition has text in (bit i = option index i), and
    // the ids it can morph into. Only the facts the check needs are kept: the
    // two export files are ~60 MB of JSON, and a DOM of them would cost far more
    // than the bake itself.
    struct OpDefinition
    {
        uint8_t opMask{};
        std::vector<int32_t> morphs;
    };

    struct OpTable
    {
        std::unordered_map<int32_t, OpDefinition> locs;
        std::unordered_map<int32_t, OpDefinition> npcs;
    };

    // Reads `locations.json` and `npcs.json` from `defsDir`. Throws
    // std::runtime_error when either is missing or malformed: an op check that
    // silently read nothing would pass every row.
    OpTable loadOpTable(const std::string &defsDir);

    enum class OpVerdict : uint8_t
    {
        Valid,              // the definition, or a morph of it, has the option
        MissingOp,          // the definition exists and nothing it becomes has it
        UnknownDefinition,  // the id is absent from the table (newer than the export)
    };

    // Whether loc `locId` (or any loc it morphs into) has option `option`.
    OpVerdict checkLocOp(const OpTable &table, int32_t locId, int32_t option);

    // Whether any NPC in [firstId, lastId] (or anything one morphs into) has
    // option `option`.
    OpVerdict checkNpcOp(const OpTable &table, int32_t firstId, int32_t lastId, int32_t option);

    struct OpCheckReport
    {
        std::size_t checked{};    // transport rows with a loc or NPC origin
        std::size_t dropped{};    // rows whose origin lacks the option
        std::size_t unverified{}; // rows whose origin id the table does not know (kept)
        std::map<std::string, std::size_t> droppedByFamily;
        std::map<std::string, std::size_t> unverifiedByFamily;
        // One line per dropped row, for the bake log.
        std::vector<std::string> droppedRows;
    };

    // Returns `model` without the Transport transitions whose origin lacks the
    // option they click. A row naming object_id 0 is left alone: it names no
    // loc at all, and finalizeTransitions drops it under its own count. An id
    // the table does not know is kept and counted as unverified, because an
    // export older than the cache is not evidence the option is missing.
    TransitionModel dropInvalidOps(const TransitionModel &model, const OpTable &table,
                                   OpCheckReport *outReport);

    // Prints the report: totals, then per family, then every dropped row.
    void printOpCheckReport(const OpCheckReport &report);
}

#endif  // WORLDWALKER_DATA_OPCHECK_H
