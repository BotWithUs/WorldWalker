#ifndef WORLDWALKER_RUNTIME_CAPABILITYSNAPSHOT_H
#define WORLDWALKER_RUNTIME_CAPABILITYSNAPSHOT_H

#include "data/Transitions.h"
#include "format/Artifact.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace ww::runtime
{
    // A per-query view of the player's queryable state, tested against transition
    // RequirementRecords during AreaSearch edge relaxation. Sparse — only the
    // skill/item/varbit/varp ids the caller has populated are present; unset ids
    // read as zero, which conservatively fails any non-zero-threshold predicate.
    //
    // Storage is four sorted vector<(id, value)> tables. Setters append + mark
    // the table dirty; the first read (lookup or meets) lazily sorts and
    // dedups so subsequent reads can use std::lower_bound. The build-then-
    // query pattern of the executor (set every entry from the host callbacks,
    // then hand the snapshot to AreaSearch) finalises each table at most
    // once per query — replacing the previous std::unordered_map storage's
    // per-insert allocator traffic with a handful of geometric vector grows.
    //
    // Mutable; populate via the setters, then borrow by const reference for one
    // findPath call. Not thread-safe; one snapshot per in-flight query.
    class CapabilitySnapshot
    {
    public:
        CapabilitySnapshot() = default;

        // Drop every entry across all four tables so the same snapshot can
        // be reused for a fresh query without reallocating its backing
        // storage. Capacity is retained — typical workloads cycle through
        // similar snapshot sizes, so the next round of setters reuses the
        // already-grown vectors.
        void clear()
        {
            skills.clear();
            items.clear();
            varbits.clear();
            varps.clear();
            skillsDirty = false;
            itemsDirty = false;
            varbitsDirty = false;
            varpsDirty = false;
            excludedTransitions.clear();
        }

        // Refuse one transition for this query whatever its requirements say.
        // The executor excludes a local transition whose loc the host could not
        // find, so the re-plan routes around it instead of choosing it again.
        void excludeTransition(uint32_t transitionIndex)
        {
            excludedTransitions.push_back(transitionIndex);
        }

        // Linear: a run excludes at most a handful (one per reroute).
        bool isTransitionExcluded(uint32_t transitionIndex) const
        {
            return std::find(excludedTransitions.begin(), excludedTransitions.end(),
                             transitionIndex) != excludedTransitions.end();
        }

        void setSkillLevel(int32_t id, int32_t level)
        {
            skills.push_back({id, level});
            skillsDirty = true;
        }

        void setItemCount(int32_t id, int32_t count)
        {
            items.push_back({id, count});
            itemsDirty = true;
        }

        void setVarbit(int32_t id, int32_t value)
        {
            varbits.push_back({id, value});
            varbitsDirty = true;
        }

        void setVarp(int32_t id, int32_t value)
        {
            varps.push_back({id, value});
            varpsDirty = true;
        }

        int32_t skillLevel(int32_t id) const
        {
            return lookup(skills, skillsDirty, id);
        }

        int32_t itemCount(int32_t id) const
        {
            return lookup(items, itemsDirty, id);
        }

        int32_t varbit(int32_t id) const
        {
            return lookup(varbits, varbitsDirty, id);
        }

        int32_t varp(int32_t id) const
        {
            return lookup(varps, varpsDirty, id);
        }

        // True when this snapshot satisfies one structured predicate. Skill and
        // item gates pass when the snapshot value is at least req.amount; varbit
        // and varp gates require an exact match (a single-value gate is the
        // documented dataset convention), and VarbitAtLeast is the minimum-value
        // form for the varbits that count up rather than flag — a task-set
        // reward state, a reputation total — where an exact gate would deny the
        // very accounts that are furthest past it. An unknown kind byte is
        // rejected conservatively: a malformed artifact must not silently pass a
        // gate, and a reader older than the kind denies the edge and walks.
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
                case data::RequirementKind::VarbitAtLeast:
                    return varbit(req.id) >= req.amount;
            }
            return false;
        }

    private:
        using Entry = std::pair<int32_t, int32_t>;

        // Sort + dedup-keeping-last-write so a later setVarbit(id, v2) wins
        // over an earlier setVarbit(id, v1). Cheap when called after a
        // bulk-set phase: one O(N log N) sort, one linear sweep, then every
        // subsequent lookup() is a single std::lower_bound.
        static void finalize(std::vector<Entry> &v)
        {
            if (v.empty())
            {
                return;
            }
            std::stable_sort(v.begin(), v.end(),
                             [](const Entry &a, const Entry &b)
                             { return a.first < b.first; });
            auto write = v.begin();
            auto read = v.begin();
            while (read != v.end())
            {
                auto next = read + 1;
                while (next != v.end() && next->first == read->first)
                {
                    ++next;
                }
                *write++ = *(next - 1);  // last write wins
                read = next;
            }
            v.erase(write, v.end());
        }

        static int32_t lookup(std::vector<Entry> &table, bool &dirty, int32_t id)
        {
            if (dirty)
            {
                finalize(table);
                dirty = false;
            }
            const auto it = std::lower_bound(table.begin(), table.end(), id,
                                              [](const Entry &e, int32_t key)
                                              { return e.first < key; });
            if (it == table.end() || it->first != id)
            {
                return 0;
            }
            return it->second;
        }

        // The backing vectors and dirty flags are mutable so the lookup
        // path can lazily finalize from a const meets() call. The mutation
        // is observably idempotent — lookup result is identical before
        // and after — so const-correctness is preserved at the API level.
        mutable std::vector<Entry> skills;
        mutable std::vector<Entry> items;
        mutable std::vector<Entry> varbits;
        mutable std::vector<Entry> varps;
        mutable bool skillsDirty{false};
        mutable bool itemsDirty{false};
        mutable bool varbitsDirty{false};
        mutable bool varpsDirty{false};
        std::vector<uint32_t> excludedTransitions;
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
    // Raise `outSnapshot` until it satisfies every record in `reqs`: skill and
    // item ids go to the highest amount any record names, varbit and varp ids
    // to the last value asked for. Two records demanding different exact
    // values of the same varbit cannot both pass (the last wins), so a caller
    // that needs one particular transition admitted should pass just that
    // transition's run rather than the whole pool. Dev-harness helper: the
    // "fully equipped player" every wwcli subcommand plans as.
    inline void applyPermissiveRequirements(std::span<const format::RequirementRecord> reqs,
                                            CapabilitySnapshot &outSnapshot)
    {
        for (const format::RequirementRecord &r : reqs)
        {
            switch (static_cast<data::RequirementKind>(r.kind))
            {
                case data::RequirementKind::Skill:
                    if (outSnapshot.skillLevel(r.id) < r.amount)
                    {
                        outSnapshot.setSkillLevel(r.id, r.amount);
                    }
                    break;
                case data::RequirementKind::Item:
                    if (outSnapshot.itemCount(r.id) < r.amount)
                    {
                        outSnapshot.setItemCount(r.id, r.amount);
                    }
                    break;
                case data::RequirementKind::Varbit:
                    outSnapshot.setVarbit(r.id, r.amount);
                    break;
                case data::RequirementKind::Varp:
                    outSnapshot.setVarp(r.id, r.amount);
                    break;
                case data::RequirementKind::VarbitAtLeast:
                    if (outSnapshot.varbit(r.id) < r.amount)
                    {
                        outSnapshot.setVarbit(r.id, r.amount);
                    }
                    break;
            }
        }
    }

    // True when `snapshot` excludes transition `transitionIndex` outright. A
    // null snapshot excludes nothing, like it gates nothing.
    inline bool isExcluded(const CapabilitySnapshot *snapshot, uint32_t transitionIndex)
    {
        return snapshot != nullptr && snapshot->isTransitionExcluded(transitionIndex);
    }

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
