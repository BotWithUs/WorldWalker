#ifndef WORLDWALKER_DATA_DIALOGZONES_H
#define WORLDWALKER_DATA_DIALOGZONES_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ww::data
{
    // An answer travels to the host packed into the nine int32 slots of one
    // runChainStep call, so it is at most this many UTF-8 bytes. The loader
    // refuses a longer one rather than truncate it into a different answer.
    inline constexpr std::size_t kDialogAnswerBytes = 36;

    // A box of tiles where walking can raise a conversation that asks a
    // question, and the replies that answer it (dialog_zones.json). The walker
    // never picks an option anywhere else. Answers are tried in order, each a
    // substring of the option text to pick, the way the scripts' Dialogs
    // matches replies.
    //
    // A zone and not a transition because the ground it covers is usually
    // open: the abbey road is one area on both sides of where the Strykewyrm
    // hunter stops you, so a transition across it would be dropped by the bake
    // as an intra-area edge and never planned.
    struct DialogZone
    {
        std::string name;
        int32_t minX{};
        int32_t minY{};
        int32_t maxX{};
        int32_t maxY{};
        uint8_t planeMin{};
        uint8_t planeMax{};
        std::vector<std::string> answers;
    };

    struct DialogZonesModel
    {
        std::vector<DialogZone> zones;
    };
}

#endif  // WORLDWALKER_DATA_DIALOGZONES_H
