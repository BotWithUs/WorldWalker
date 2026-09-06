#ifndef WORLDWALKER_DATA_DATASETORIGINS_H
#define WORLDWALKER_DATA_DATASETORIGINS_H

#include "data/Transitions.h"

#include <cstdint>
#include <unordered_set>

namespace ww::data
{
    // The origin tiles the curated datasets already claim.
    //
    // ADR 0003 makes the datasets authoritative over cache derivation on
    // conflict, and "conflict" is judged at the origin tile, not at the whole
    // endpoint tuple: a curated entry for a gated door or a mid-landing ladder
    // records where you stand and what really happens next, which is exactly
    // the case where the cache-derived guess about the destination is wrong.
    // Matching on the full tuple would let the derived guess survive alongside
    // the curated truth, since the two disagree about precisely the field the
    // dataset was written to correct.
    //
    // Every deriver that emits local-origin candidates consults this, so a
    // curated entry suppresses cache derivation the same way whatever derived
    // it.
    using DatasetOrigins = std::unordered_set<uint64_t>;

    // Pack a tile + plane into a unique key. World coords fit well under 2^24,
    // so the shifted fields never overlap.
    inline uint64_t datasetOriginKey(int32_t x, int32_t y, uint8_t plane)
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 40)
             | (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 8)
             | static_cast<uint64_t>(plane);
    }

    // Origin keys of every local-origin transition in `datasets`. Global-origin
    // transitions are excluded: they have no meaningful origin tile.
    inline DatasetOrigins collectDatasetOrigins(const TransitionModel &datasets)
    {
        DatasetOrigins origins;
        origins.reserve(datasets.transitions.size() * 2 + 1);
        for (const Transition &t : datasets.transitions)
        {
            if (!t.isGlobalOrigin)
            {
                origins.insert(datasetOriginKey(t.originX, t.originY, t.originPlane));
            }
        }
        return origins;
    }
}

#endif  // WORLDWALKER_DATA_DATASETORIGINS_H
