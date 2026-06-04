#ifndef WORLDWALKER_RUNTIME_CAPABILITYSNAPSHOT_H
#define WORLDWALKER_RUNTIME_CAPABILITYSNAPSHOT_H

#include "data/Transitions.h"
#include "format/Artifact.h"

#include <cstdint>
#include <span>
#include <unordered_map>

namespace ww::runtime
{
    // A per-query view of the player's queryable state, tested against transition
    // RequirementRecords during AreaSearch edge relaxation. Sparse — only the
    // skill/item/varbit/varp ids the caller has populated are present; unset ids
    // read as zero, which conservatively fails any non-zero-threshold predicate.
    //
    // Mutable; populate via the setters, then borrow by const reference for one
    // findPath call. Not thread-safe; one snapshot per in-flight query.
    class CapabilitySnapshot
    {
    public:
        CapabilitySnapshot() = default;

        void setSkillLevel(int32_t id, int32_t level)
        {
            skills[id] = level;
        }

        void setItemCount(int32_t id, int32_t count)
        {
            items[id] = count;
        }

        void setVarbit(int32_t id, int32_t value)
        {
            varbits[id] = value;
        }

        void setVarp(int32_t id, int32_t value)
        {
            varps[id] = value;
        }

        int32_t skillLevel(int32_t id) const
        {
            return lookup(skills, id);
        }

        int32_t itemCount(int32_t id) const
        {
            return lookup(items, id);
        }

        int32_t varbit(int32_t id) const
        {
            return lookup(varbits, id);
        }

        int32_t varp(int32_t id) const
        {
            return lookup(varps, id);
        }

        // True when this snapshot satisfies one structured predicate. Skill and
        // item gates pass when the snapshot value is at least req.amount; varbit
        // and varp gates require an exact match (a single-value gate is the
        // documented dataset convention). An unknown kind byte is rejected
        // conservatively — a malformed artifact must not silently pass a gate.
        bool meets(const format::RequirementRecord &req) const
        {
            switch (static_cast<data::RequirementKind>(req.kind))
            {
                case data::RequirementKind::Skill:
                    return skillLevel(req.id) >= req.amount;
                case data::RequirementKind::Item:
                    return itemCount(req.id) >= req.amount;
                case data::RequirementKind::Varbit:
                    return varbit(req.id) == req.amount;
                case data::RequirementKind::Varp:
                    return varp(req.id) == req.amount;
            }
            return false;
        }

    private:
        static int32_t lookup(const std::unordered_map<int32_t, int32_t> &table, int32_t id)
        {
            const auto it = table.find(id);
            return it == table.end() ? 0 : it->second;
        }

        std::unordered_map<int32_t, int32_t> skills;
        std::unordered_map<int32_t, int32_t> items;
        std::unordered_map<int32_t, int32_t> varbits;
        std::unordered_map<int32_t, int32_t> varps;
    };

    // Convenience predicate over a Requirement run. Skill / varbit / varp gates
    // are always all-required (AND). Item gates depend on `kind`:
    //
    //   - ItemTeleport: the item requirements are ALTERNATIVES (OR). The dataset
    //     lists every accepted variant of the teleport item (e.g. the
    //     dungeoneering / max / completionist cape ids), any one of which works,
    //     so the run passes as long as the player holds at least one — and fails
    //     only when there are item gates and none is met. (Matches the legacy
    //     nav stack's evaluateTeleport: hasItemReq && !hasAnyItem.)
    //   - everything else (spell teleports list the runes a cast consumes,
    //     transport links, …): item gates are all-required (AND).
    //
    // A null snapshot accepts everything (the no-gate default used by the
    // unfiltered overloads). Used by both AreaSearch (edge gating) and
    // PathAssembler (global-teleport seed gating) so the two share one rule.
    inline bool meetsRequirements(const CapabilitySnapshot *snapshot,
                                  std::span<const format::RequirementRecord> reqs,
                                  data::TransitionKind kind = data::TransitionKind::Transport)
    {
        if (snapshot == nullptr)
        {
            return true;
        }
        const bool itemsAreAlternatives = (kind == data::TransitionKind::ItemTeleport);
        bool hasItemGate = false;
        bool anyItemMet = false;
        for (const format::RequirementRecord &r : reqs)
        {
            const bool isItem =
                static_cast<data::RequirementKind>(r.kind) == data::RequirementKind::Item;
            if (isItem && itemsAreAlternatives)
            {
                hasItemGate = true;
                anyItemMet = anyItemMet || snapshot->meets(r);
            }
            else if (!snapshot->meets(r))
            {
                return false;
            }
        }
        if (hasItemGate && !anyItemMet)
        {
            return false;
        }
        return true;
    }
}

#endif  // WORLDWALKER_RUNTIME_CAPABILITYSNAPSHOT_H
