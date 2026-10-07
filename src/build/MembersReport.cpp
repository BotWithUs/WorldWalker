#include "build/MembersReport.h"

#include "data/Transitions.h"
#include "format/F2pLand.h"
#include "format/MoveCategory.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ww::build
{
    namespace
    {
        // Cache enum 12260 CHEEVO_LODESTONE_FREE and 12261
        // CHEEVO_LODESTONE_MEMBERS, by the names item_teleports.json uses.
        constexpr std::array<std::string_view, 11> kFreeLodestones = {
            "Al Kharid", "Ashdale", "Burthorpe", "Draynor Village", "Edgeville", "Falador",
            "Lumbridge", "Port Sarim", "Taverley", "Varrock", "Wilderness Crater",
        };
        constexpr std::array<std::string_view, 15> kMembersLodestones = {
            "Ardougne", "Bandit Camp", "Canifis", "Catherby", "Eagles' Peak",
            "Fremennik Province", "Karamja", "Lunar Isle", "Oo'glog", "Prifddinas",
            "Seers' Village", "Tirannwn", "Yanille", "Menaphos", "Anachronia",
        };

        constexpr std::string_view kCharterPrefix = "Charter ship";

        template <std::size_t N>
        bool isListed(const std::array<std::string_view, N> &names, const std::string &name)
        {
            for (const std::string_view listed : names)
            {
                if (listed == name)
                {
                    return true;
                }
            }
            return false;
        }

        struct FamilyCount
        {
            std::size_t total{};
            std::size_t members{};
        };

        std::string familyOf(const format::TransitionRecord &tx, format::MoveCategory category,
                             const std::string &label)
        {
            const auto kind = static_cast<data::TransitionKind>(tx.kind);
            if (kind == data::TransitionKind::Lodestone)
            {
                return "lodestone";
            }
            if (kind == data::TransitionKind::Spell)
            {
                return "spell teleport";
            }
            if (kind == data::TransitionKind::ItemTeleport)
            {
                return "item teleport";
            }
            if (!label.empty())
            {
                return label;
            }
            return std::string("(derived ") + format::moveCategoryName(category) + ")";
        }

        void checkLodestone(const std::string &name, bool isMembers, bool isGeographyMembers,
                            int8_t override, MembersCheck &io)
        {
            if (isListed(kFreeLodestones, name))
            {
                if (isMembers || isGeographyMembers)
                {
                    io.failures.push_back("lodestone " + name
                                          + " is free (enum 12260) but classifies members"
                                          + (isGeographyMembers ? " (its landing is off f2p land)"
                                                                : ""));
                }
                return;
            }
            if (isListed(kMembersLodestones, name))
            {
                if (!isMembers)
                {
                    io.failures.push_back("lodestone " + name
                                          + " is members (enum 12261) but classifies free");
                }
                return;
            }
            if (override < 0)
            {
                io.failures.push_back("lodestone " + name
                                      + " is in neither cache enum and has no \"members\" flag");
            }
        }

        void checkOne(const format::ArtifactReader &reader, std::size_t i,
                      const std::string &label, std::map<std::string, FamilyCount> &families,
                      MembersCheck &io)
        {
            const format::TransitionRecord &tx = reader.transitions()[i];
            const auto category = static_cast<format::MoveCategory>(reader.moveCategories()[i]);
            const int8_t override = reader.membersOverrides()[i];
            const bool isMembers = reader.membersOnly()[i] != 0u;
            const format::F2pLand *land = reader.hasF2pZones() ? &reader.f2pLand() : nullptr;
            const bool isGeographyMembers = format::isMembersOnly(tx, category, -1, land);

            FamilyCount &count = families[familyOf(tx, category, label)];
            ++count.total;
            count.members += isMembers ? 1u : 0u;
            io.membersOnly += isMembers ? 1u : 0u;

            if (override >= 0 && isMembers != isGeographyMembers)
            {
                io.overridesAgainstGeography.push_back(
                    "#" + std::to_string(i) + " " + (label.empty() ? "(unnamed)" : label)
                    + ": \"members\": " + (override != 0 ? "true" : "false") + " ("
                    + std::to_string(tx.destX) + "," + std::to_string(tx.destY) + ","
                    + std::to_string(tx.destPlane) + ")");
            }
            if (static_cast<data::TransitionKind>(tx.kind) == data::TransitionKind::Lodestone)
            {
                checkLodestone(label, isMembers, isGeographyMembers, override, io);
            }
            if (label.starts_with(kCharterPrefix) && !isMembers)
            {
                io.failures.push_back("#" + std::to_string(i) + " " + label
                                      + " is a charter ship but classifies free");
            }
        }
    }

    MembersCheck checkMembers(const format::ArtifactReader &reader,
                              const std::vector<std::string> &labels)
    {
        const std::size_t count = reader.transitions().size();
        if (labels.size() < count)
        {
            throw std::invalid_argument("checkMembers: a label per transition is required");
        }
        MembersCheck check;
        check.transitions = count;
        if (!reader.hasF2pZones())
        {
            check.failures.push_back("the artifact has no F2pZones section");
        }
        std::map<std::string, FamilyCount> families;
        for (std::size_t i = 0; i < count; ++i)
        {
            checkOne(reader, i, labels[i], families, check);
        }
        std::printf("  members by family (members / total):\n");
        for (const auto &[family, c] : families)
        {
            std::printf("    %5zu / %5zu  %s\n", c.members, c.total, family.c_str());
        }
        return check;
    }

    void printMembersCheck(const MembersCheck &check)
    {
        std::printf("  free-to-play: %zu of %zu transitions members-only, %zu free to play\n",
                    check.membersOnly, check.transitions, check.transitions - check.membersOnly);
        for (const std::string &line : check.overridesAgainstGeography)
        {
            std::printf("  members flag overrides geography: %s\n", line.c_str());
        }
        for (const std::string &line : check.failures)
        {
            std::fprintf(stderr, "  FREE-TO-PLAY CHECK FAILED: %s\n", line.c_str());
        }
    }
}
