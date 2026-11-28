#include "cli/SpellbookTests.h"

#include "c_api/worldwalker_c.h"
#include "data/DatasetLoader.h"
#include "data/Transitions.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"
#include "runtime/AreaSearch.h"
#include "runtime/CapabilitySnapshot.h"
#include "runtime/PathAssembler.h"
#include "runtime/RuntimeTeleports.h"
#include "runtime/TileSearch.h"
#include "runtime/WorldView.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    using ww::data::ChainStepKind;
    using ww::data::Requirement;
    using ww::data::RequirementKind;
    using ww::data::Transition;
    using ww::data::TransitionKind;

    // gameval varbit 0 ZAROS_SPELLBOOK: the active book. clientscript-2660
    // hides an ability-book spell when its param 2871 is neither 3 nor this
    // value; clientscript-6889 reads 2 as "you're on the Lunar spellbook".
    constexpr int32_t kSpellbookVarbit = 0;
    constexpr int32_t kStandardBook = 0;
    constexpr int32_t kLunarBook = 2;
    // clientscript-15411 case 14867 / 14868: Trollheim Farm Teleport needs
    // varbit 16374 LUNARFM_SPELLS_UNLOCKED >= 9 (the group cast >= 10) and
    // QUEST_FINISHED(314), My Arm's Big Adventure, which quests.json keeps in
    // varbit 9987 MYARM (complete at 320).
    constexpr int32_t kLunarFarmVarbit = 16374;
    constexpr int32_t kFarmUnlocked = 9;
    constexpr int32_t kGroupFarmUnlocked = 10;
    constexpr int32_t kMyArmVarbit = 9987;
    constexpr int32_t kMyArmDone = 320;

    constexpr int32_t kMagic = 6;
    constexpr int32_t kLastSkill = 28;
    constexpr int32_t kMaxLevel = 99;
    constexpr int32_t kWaterRune = 555;
    constexpr int32_t kAstralRune = 9075;
    constexpr int32_t kLawRune = 563;
    // gameval obj 20175 POH_TABLET_TROLLHEIMTELEPORT.
    constexpr int32_t kTrollheimTablet = 20175;
    // The tablet's landing is V1's (rs3-nav-api teleports.dat, the 20175
    // "Break" row); the cache gives the tablet none.
    constexpr int32_t kTabletX = 2891;
    constexpr int32_t kTabletY = 3673;
    // Backpack op 1 is the tablet's first widget action, "Break".
    constexpr int32_t kBreakOption = 1;
    constexpr int32_t kBackpackInterface = 1473;
    constexpr int32_t kBackpackComponent = 5;

    constexpr int32_t kAbilityBook = 1461;
    constexpr int32_t kFarmComponent = 440;          // struct 14867 param 2975
    constexpr int32_t kFishingGuildComponent = 330;  // struct 14857 param 2975
    constexpr int32_t kFarmX = 2817;                 // struct 14867 param 2941
    constexpr int32_t kFarmY = 3676;
    constexpr int32_t kFishingGuildX = 2613;         // struct 14857 param 2941
    constexpr int32_t kFishingGuildY = 3383;

    // BWU-JARKM9: the live start near Yanille and the sextant dig.
    constexpr int32_t kYanilleX = 2616;
    constexpr int32_t kYanilleY = 3077;
    constexpr int32_t kDigX = 2848;
    constexpr int32_t kDigY = 3684;
    // A tile of the farm landing's own pocket, a few steps from the landing.
    constexpr int32_t kFarmPocketX = 2812;
    constexpr int32_t kFarmPocketY = 3672;
    // BWU-JCKQ5S: the Fishing Guild bank, from Varrock square.
    constexpr int32_t kGuildBankX = 2586;
    constexpr int32_t kGuildBankY = 3422;
    constexpr int32_t kVarrockX = 3212;
    constexpr int32_t kVarrockY = 3424;

    int fail(const char *what)
    {
        std::printf("  FAIL: %s\n", what);
        return 1;
    }

    // The shipped datasets/ directory, found by walking up from the working
    // directory. Empty when no ancestor holds datasets/spell_teleports.json.
    std::filesystem::path findShippedDatasets()
    {
        std::error_code ec;
        std::filesystem::path at = std::filesystem::current_path(ec);
        while (!ec && !at.empty())
        {
            const std::filesystem::path candidate = at / "datasets";
            if (std::filesystem::exists(candidate / "spell_teleports.json", ec))
            {
                return candidate;
            }
            if (at == at.parent_path())
            {
                break;
            }
            at = at.parent_path();
        }
        return {};
    }

    bool hasRequirement(const std::vector<Requirement> &reqs, RequirementKind kind, int32_t id,
                        int32_t amount)
    {
        for (const Requirement &r : reqs)
        {
            if (r.kind == kind && r.id == id && r.amount == amount)
            {
                return true;
            }
        }
        return false;
    }

    // The value of the row's varbit-0 gate, or empty when it has none.
    std::optional<int32_t> bookGate(const Transition &t)
    {
        for (const Requirement &r : t.requirements)
        {
            if (r.kind == RequirementKind::Varbit && r.id == kSpellbookVarbit)
            {
                return r.amount;
            }
        }
        return std::nullopt;
    }

    const Transition *findLabel(const std::vector<Transition> &txs, std::string_view label)
    {
        for (const Transition &t : txs)
        {
            if (t.label == label)
            {
                return &t;
            }
        }
        return nullptr;
    }

    bool startsWith(std::string_view text, std::string_view prefix)
    {
        return text.substr(0, prefix.size()) == prefix;
    }

    // Every spell row names its book, but for the two any-book teleports.
    int checkEverySpellGated(const std::vector<Transition> &txs)
    {
        int failures = 0;
        std::size_t gated = 0;
        for (const Transition &t : txs)
        {
            if (t.kind != TransitionKind::Spell)
            {
                continue;
            }
            const bool isAnyBook = startsWith(t.label, "War's Retreat")
                                   || startsWith(t.label, "Max guild");
            const std::optional<int32_t> book = bookGate(t);
            if (isAnyBook == book.has_value())
            {
                std::printf("  FAIL: spell row '%s' book gate=%d (any-book=%d)\n",
                            t.label.c_str(), book.value_or(-1), isAnyBook ? 1 : 0);
                ++failures;
            }
            gated += book.has_value() ? 1 : 0;
        }
        std::printf("spellbook: %zu spell rows carry a varbit 0 gate\n", gated);
        return failures;
    }

    int expectBook(const std::vector<Transition> &txs, const char *label, int32_t book)
    {
        const Transition *t = findLabel(txs, label);
        if (t == nullptr || bookGate(*t) != book)
        {
            std::printf("  FAIL: '%s' is not gated on book %d\n", label, book);
            return 1;
        }
        return 0;
    }

    int checkTrollheimFarmUnlock(const std::vector<Transition> &txs)
    {
        int failures = 0;
        const Transition *single = findLabel(txs, "Trollheim Farm Teleport");
        const Transition *group = findLabel(txs, "Tele-group Trollheim Farm");
        if (single == nullptr || group == nullptr)
        {
            return fail("dataset: a Trollheim Farm row is missing");
        }
        failures += hasRequirement(single->requirements, RequirementKind::VarbitAtLeast,
                                   kLunarFarmVarbit, kFarmUnlocked)
            ? 0 : fail("dataset: Trollheim Farm Teleport lacks varbit 16374 >= 9");
        failures += hasRequirement(group->requirements, RequirementKind::VarbitAtLeast,
                                   kLunarFarmVarbit, kGroupFarmUnlocked)
            ? 0 : fail("dataset: Tele-group Trollheim Farm lacks varbit 16374 >= 10");
        for (const Transition *t : {single, group})
        {
            failures += hasRequirement(t->requirements, RequirementKind::VarbitAtLeast,
                                       kMyArmVarbit, kMyArmDone)
                ? 0 : fail("dataset: a Trollheim Farm row lacks My Arm's Big Adventure");
        }
        return failures;
    }

    int checkTabletRow(const std::vector<Transition> &txs)
    {
        const Transition *t = findLabel(txs, "Trollheim tablet");
        if (t == nullptr)
        {
            return fail("dataset: no Trollheim tablet row");
        }
        const bool isShaped = t->kind == TransitionKind::ItemTeleport && t->isGlobalOrigin
                              && t->destX == kTabletX && t->destY == kTabletY
                              && t->destPlane == 0 && t->requirements.size() == 1
                              && hasRequirement(t->requirements, RequirementKind::Item,
                                                kTrollheimTablet, 1)
                              && !t->chain.empty()
                              && t->chain.front().kind == ChainStepKind::ClickItem
                              && t->chain.front().e == kBackpackInterface
                              && t->chain.front().f == kBackpackComponent
                              && t->chain.front().g == kBreakOption;
        return isShaped ? 0 : fail("dataset: the Trollheim tablet row is not item 20175 ->"
                                   " 2891,3673 by backpack Break");
    }

    int runDatasetChecks(const std::filesystem::path &dir)
    {
        const ww::data::LoadedDatasets loaded = ww::data::loadGlobalTeleports(dir.string());
        const std::vector<Transition> &txs = loaded.model.transitions;
        std::printf("spellbook: shipped datasets at %s -> %zu transitions\n",
                    dir.string().c_str(), txs.size());
        int failures = checkEverySpellGated(txs);
        failures += expectBook(txs, "Trollheim Teleport", kStandardBook);
        failures += expectBook(txs, "Ardougne Teleport", kStandardBook);
        failures += expectBook(txs, "Paddewwa Teleport", 1);
        failures += expectBook(txs, "Trollheim Farm Teleport", kLunarBook);
        failures += expectBook(txs, "Fishing Guild Teleport", kLunarBook);
        failures += checkTrollheimFarmUnlock(txs);
        return failures + checkTabletRow(txs);
    }

    // ---- planner -----------------------------------------------------------

    // The appended spell landing on (x, y) that clicks 1461:<component>, or
    // UINT32_MAX when the artifact holds none.
    uint32_t findSpell(const ww::format::ArtifactReader &reader, int32_t x, int32_t y,
                       int32_t component)
    {
        const auto txs = reader.transitions();
        const auto chain = reader.chainSteps();
        const int32_t hash = (kAbilityBook << 16) | component;
        for (std::size_t i = 0; i < txs.size(); ++i)
        {
            const ww::format::TransitionRecord &t = txs[i];
            if (t.kind != static_cast<uint8_t>(TransitionKind::Spell) || t.destX != x
                || t.destY != y || t.chainCount == 0)
            {
                continue;
            }
            const ww::format::ChainStepRecord &first = chain[t.chainStart];
            if (first.kind == static_cast<uint8_t>(ChainStepKind::Click) && first.d == hash)
            {
                return static_cast<uint32_t>(i);
            }
        }
        return UINT32_MAX;
    }

    uint32_t findTablet(const ww::format::ArtifactReader &reader)
    {
        const auto txs = reader.transitions();
        for (std::size_t i = 0; i < txs.size(); ++i)
        {
            const ww::format::TransitionRecord &t = txs[i];
            if (t.kind == static_cast<uint8_t>(TransitionKind::ItemTeleport) && t.destX == kTabletX
                && t.destY == kTabletY)
            {
                return static_cast<uint32_t>(i);
            }
        }
        return UINT32_MAX;
    }

    // True when transition `index` is gated on the Lunar book.
    bool isLunar(const ww::format::ArtifactReader &reader, uint32_t index)
    {
        const ww::format::TransitionRecord &t = reader.transitions()[index];
        const auto reqs = reader.requirements().subspan(t.requirementStart, t.requirementCount);
        for (const ww::format::RequirementRecord &r : reqs)
        {
            if (r.kind == static_cast<uint8_t>(RequirementKind::Varbit)
                && r.id == kSpellbookVarbit && r.amount == kLunarBook)
            {
                return true;
            }
        }
        return false;
    }

    struct Account
    {
        int32_t book{kStandardBook};
        int32_t water{};
        bool hasTablet{};
    };

    // An account with every skill at 99, the Trollheim Farm unlocks, astral
    // and law runes for one Lunar cast, and `water` water runes: 10 admits
    // Trollheim Farm Teleport (not the group cast, 20), 8 the Fishing Guild
    // Teleport (not the group cast, 10). No lodestone is unlocked.
    ww::runtime::CapabilitySnapshot snapshotFor(const Account &a)
    {
        ww::runtime::CapabilitySnapshot s;
        for (int32_t skill = 0; skill <= kLastSkill; ++skill)
        {
            s.setSkillLevel(skill, kMaxLevel);
        }
        s.setVarbit(kSpellbookVarbit, a.book);
        s.setVarbit(kLunarFarmVarbit, kGroupFarmUnlocked);
        s.setVarbit(kMyArmVarbit, kMyArmDone);
        if (a.water > 0)
        {
            s.setItemCount(kWaterRune, a.water);
            s.setItemCount(kAstralRune, 3);
            s.setItemCount(kLawRune, 3);
        }
        if (a.hasTablet)
        {
            s.setItemCount(kTrollheimTablet, 1);
        }
        return s;
    }

    struct Planner
    {
        const ww::format::ArtifactReader &reader;
        ww::runtime::PathAssembler &assembler;
    };

    struct Query
    {
        const char *label;
        int32_t startX;
        int32_t startY;
        int32_t goalX;
        int32_t goalY;
        Account account;
    };

    struct Outcome
    {
        bool isPlanned{};
        bool usesWanted{};
        bool usesLunar{};
        float cost{};
    };

    Outcome plan(const Planner &p, const Query &q, uint32_t wanted)
    {
        ww::runtime::CapabilitySnapshot snapshot = snapshotFor(q.account);
        ww::runtime::Plan result;
        Outcome out;
        out.isPlanned = p.assembler.assemble(q.startX, q.startY, 0, q.goalX, q.goalY, 0,
                                             &snapshot, result);
        out.cost = result.cost;
        for (const ww::runtime::Step &s : result.steps)
        {
            if (s.kind != ww::runtime::StepKind::Transition)
            {
                continue;
            }
            out.usesWanted = out.usesWanted || s.transitionIndex == wanted;
            out.usesLunar = out.usesLunar || isLunar(p.reader, s.transitionIndex);
        }
        std::printf("spellbook: %-44s planned=%d uses=%d lunar=%d cost=%.1f\n", q.label,
                    out.isPlanned ? 1 : 0, out.usesWanted ? 1 : 0, out.usesLunar ? 1 : 0,
                    static_cast<double>(out.cost));
        return out;
    }

    int expectUses(const Planner &p, const Query &q, uint32_t wanted)
    {
        const Outcome o = plan(p, q, wanted);
        return o.isPlanned && o.usesWanted ? 0 : fail(q.label);
    }

    int expectAvoids(const Planner &p, const Query &q, uint32_t unwanted)
    {
        const Outcome o = plan(p, q, unwanted);
        const bool isLunarAllowed = q.account.book == kLunarBook;
        return !o.usesWanted && (isLunarAllowed || !o.usesLunar) ? 0 : fail(q.label);
    }

    // The farm landing walks to the dig only on an artifact baked with the
    // summit join (the Troll ladder 18834 / Exit 3774 rows); before it the
    // landing pocket's only ways out are one-way down into the cave. Asked
    // with every gate refused and no teleport of any family, so a teleport
    // out of the pocket does not pass for a walk.
    bool isFarmJoined(const Planner &p)
    {
        constexpr uint32_t kNoTeleports =
            (1u << WW_MOVE_TELEPORTS) | (1u << WW_MOVE_LODESTONES) | (1u << WW_MOVE_FAIRY_RINGS)
            | (1u << WW_MOVE_SPIRIT_TREES) | (1u << WW_MOVE_OTHER_CHAINS);
        ww::runtime::CapabilitySnapshot walkOnly;
        walkOnly.disableMoves(kNoTeleports, p.reader.moveCategories(), p.reader.membersOnly());
        ww::runtime::Plan result;
        return p.assembler.assemble(kFarmX, kFarmY, 0, kDigX, kDigY, 0, &walkOnly, result);
    }

    int checkTrollheim(const Planner &p, uint32_t farm, uint32_t tablet)
    {
        const Account lunar{kLunarBook, 10, false};
        const Account standard{kStandardBook, 10, false};
        const Account tabletOnly{kStandardBook, 0, true};
        const Account neither{kLunarBook, 0, false};
        int failures = expectUses(p, {"Lunar: Yanille -> farm pocket by Farm Teleport",
                                      kYanilleX, kYanilleY, kFarmPocketX, kFarmPocketY, lunar},
                                  farm);
        failures += expectAvoids(p, {"standard book: Yanille -> farm pocket, no Lunar", kYanilleX,
                                     kYanilleY, kFarmPocketX, kFarmPocketY, standard},
                                 farm);
        if (isFarmJoined(p))
        {
            failures += expectUses(p, {"Lunar: Yanille -> dig 2848,3684 by Farm Teleport",
                                       kYanilleX, kYanilleY, kDigX, kDigY, lunar},
                                   farm);
        }
        else
        {
            std::printf("spellbook: Lunar: Yanille -> dig by Farm Teleport SKIP  the farm"
                        " landing does not walk to the dig in this artifact\n");
        }
        failures += expectUses(p, {"tablet: Yanille -> dig 2848,3684 by tablet", kYanilleX,
                                   kYanilleY, kDigX, kDigY, tabletOnly},
                               tablet);
        failures += expectAvoids(p, {"no runes, no tablet: Yanille -> dig, no spell", kYanilleX,
                                     kYanilleY, kDigX, kDigY, neither},
                                 farm);
        return failures + expectAvoids(p, {"no runes, no tablet: Yanille -> dig, no tablet",
                                           kYanilleX, kYanilleY, kDigX, kDigY, neither},
                                       tablet);
    }

    int checkFishingGuild(const Planner &p, uint32_t guild)
    {
        const Account lunar{kLunarBook, 8, false};
        const Account standard{kStandardBook, 8, false};
        int failures = expectUses(p, {"Lunar: Varrock -> Fishing Guild bank by teleport",
                                      kVarrockX, kVarrockY, kGuildBankX, kGuildBankY, lunar},
                                  guild);
        return failures + expectAvoids(p, {"standard book: Varrock -> Fishing Guild, no Lunar",
                                           kVarrockX, kVarrockY, kGuildBankX, kGuildBankY,
                                           standard},
                                       guild);
    }

    int runPlannerChecks(const char *artifactPath, const std::filesystem::path &dir)
    {
        ww::format::ArtifactReader reader(artifactPath);
        const std::size_t appended = ww::runtime::loadGlobalTeleportsInto(reader, dir.string());
        std::printf("spellbook: appended %zu shipped teleports to %s\n", appended, artifactPath);
        const uint32_t farm = findSpell(reader, kFarmX, kFarmY, kFarmComponent);
        const uint32_t guild = findSpell(reader, kFishingGuildX, kFishingGuildY,
                                         kFishingGuildComponent);
        const uint32_t tablet = findTablet(reader);
        if (farm == UINT32_MAX || guild == UINT32_MAX || tablet == UINT32_MAX)
        {
            return fail("planner: a Farm Teleport, Fishing Guild Teleport or tablet record is"
                        " missing from the appended teleports");
        }
        ww::runtime::WorldView view(reader);
        ww::runtime::AreaSearch areaSearch(reader);
        ww::runtime::TileSearch tileSearch(view);
        ww::runtime::PathAssembler assembler(reader, view, areaSearch, tileSearch);
        const Planner p{reader, assembler};
        return checkTrollheim(p, farm, tablet) + checkFishingGuild(p, guild);
    }
}

int runSpellbookTests(const char *artifactPath)
{
    std::printf("spellbook: spell teleports gated on the active book, the Trollheim tablet\n");
    const std::filesystem::path dir = findShippedDatasets();
    if (dir.empty())
    {
        std::printf("  FAIL: datasets/spell_teleports.json not found above the working"
                    " directory (run wwcli from inside the checkout)\n");
        return 1;
    }
    int failures = 0;
    try
    {
        failures += runDatasetChecks(dir);
        if (artifactPath == nullptr)
        {
            std::printf("spellbook: planner SKIP  pass an artifact to plan against\n");
        }
        else
        {
            failures += runPlannerChecks(artifactPath, dir);
        }
    }
    catch (const std::exception &e)
    {
        std::printf("  FAIL: threw: %s\n", e.what());
        ++failures;
    }
    if (failures == 0)
    {
        std::printf("spellbook: all checks passed\n");
        return 0;
    }
    std::printf("spellbook: %d check(s) FAILED\n", failures);
    return 1;
}
