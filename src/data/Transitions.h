#ifndef WORLDWALKER_DATA_TRANSITIONS_H
#define WORLDWALKER_DATA_TRANSITIONS_H

#include <cstdint>
#include <vector>

// In-memory transition model used while building the artifact. This is the
// build-time shape (std containers, variable-length); the serialized on-disk
// shape lives in format/Artifact.h. Only wwbuild touches these types — the
// runtime reads the baked records, never this model.
namespace ww::data
{
    // How a transition is executed, and what its origin means. The planner and
    // (later) the executor dispatch on this.
    enum class TransitionKind : uint8_t
    {
        Transport     = 0,  // object link (ladder/stair/door/lever): local origin, interact to dest
        FairyRing     = 1,  // teleport_chains type=fairy_ring: stand at ring, dial code
        TeleportChain = 2,  // teleport_chains, any other type
        Spell         = 3,  // spell_teleports: global, cast chain + requirements
        Lodestone     = 4,  // item_teleports lodestones: global, config-built chain
        ItemTeleport  = 5,  // item_teleports `teleports[]`: global, item-click chain
    };

    enum class RequirementKind : uint8_t
    {
        Skill  = 0,  // amount = required level
        Item   = 1,  // amount = required count
        Varbit = 2,  // amount = required value
        Varp   = 3,  // amount = required value
    };

    // One structured predicate filtered against a per-query capability snapshot.
    struct Requirement
    {
        RequirementKind kind{};
        int32_t id{};
        int32_t amount{};
    };

    enum class ChainStepKind : uint8_t
    {
        // A queued game action dispatched verbatim by the host:
        //   a=actionId, b=param1, c=param2, d=param3.
        // For a component click that is (COMPONENT, option, sub_component,
        // (iface<<16)|comp); the executor derives the interface-open gate from
        // param3>>16 when actionId==COMPONENT.
        Click = 0,
        Wait  = 1,  // a=ticks to wait
        // Block until interface `a` is open (engine poll, host-side); used between
        // the click that opens a teleport dialog and the selection inside it.
        WaitInterface = 2,
        // Select option `b` in dialogue interface `a`. c=per_page, d=next_comp,
        // e=wait_ticks — the host resolves the option component against the live
        // dialogue (paging) since it depends on engine state, not the artifact.
        DialogueSelect = 3,
        // Click a teleport item that may be worn OR carried. The host checks the
        // live worn/backpack containers for the transition's required item and
        // dispatches the matching variant:
        //   a..d = worn   (iface, comp, option, sub_component)
        //   e..h = backpack(iface, comp, option, sub_component)
        //   i    = backpack uses the COMPONENT_SPECIAL action when non-zero.
        ClickItem = 4,
    };

    // One step of an execution chain, passed through to the executor verbatim.
    // The nine generic slots cover every kind above (see ChainStepKind for the
    // per-kind field mapping); unused slots are zero.
    struct ChainStep
    {
        ChainStepKind kind{};
        int32_t a{};
        int32_t b{};
        int32_t c{};
        int32_t d{};
        int32_t e{};
        int32_t f{};
        int32_t g{};
        int32_t h{};
        int32_t i{};
    };

    // A single movement/interaction edge. Origin is meaningful only when
    // !isGlobalOrigin; global teleports work from anywhere so only dest is used.
    struct Transition
    {
        TransitionKind kind{};
        bool isGlobalOrigin{};

        int32_t originX{};
        int32_t originY{};
        uint8_t originPlane{};

        int32_t destX{};
        int32_t destY{};
        uint8_t destPlane{};

        int32_t objectId{-1};
        uint8_t shape{};
        uint8_t rotation{};
        uint8_t optionIndex{};
        char code[4]{};  // fairy-ring code (e.g. "aip"), null-padded; empty otherwise

        float cost{};        // tick cost (chain waits + per-kind default)
        float costQuick{-1.0f};  // reserved: quick-teleport cost, -1 when n/a

        std::vector<Requirement> requirements;
        std::vector<ChainStep> chain;
    };

    struct TransitionModel
    {
        std::vector<Transition> transitions;
    };
}

#endif  // WORLDWALKER_DATA_TRANSITIONS_H
