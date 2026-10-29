#include "cli/SosTests.h"

#include "c_api/worldwalker_c.h"
#include "format/Artifact.h"
#include "format/ArtifactReader.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <vector>

namespace
{
    // gameval locs the routes must take (datasets/README.md, "Stronghold of
    // Security"): the Barbarian Village entrance, each reward room's ladder
    // down, the War start-room ladder up, and each start-room portal.
    constexpr std::int32_t kEntrance       = 16154;  // SOS_DUNG_ENT_OPEN
    constexpr std::int32_t kWarLadderDown  = 16149;  // SOS_WAR_LADD_DOWN
    constexpr std::int32_t kFamLadderDown  = 16081;  // SOS_FAM_LADD_DOWN
    constexpr std::int32_t kWarLadderUp    = 16148;  // SOS_WAR_LADD_UP

    // A level's reward varbit (SOS_*_REWARD, varp 3108 one bit per level) and
    // the start-room portal it opens.
    struct Level
    {
        const char  *name;
        std::int32_t rewardVarbit;
        std::int32_t portalLoc;
        WwTile       startRoom;   // where the previous ladder lands
        WwTile       rewardRoom;  // a free tile beside the reward
    };

    constexpr WwTile kBarbarianVillage{ 3081, 3426, 0 };

    constexpr std::array<Level, 3> kLevels{ {
        { "War",        16023, 16150, { 1859, 5243, 0 }, { 1906, 5222, 0 } },
        { "Famine",     16024, 16082, { 2042, 5245, 0 }, { 2023, 5216, 0 } },
        { "Pestilence", 16025, 16116, { 2123, 5252, 0 }, { 2145, 5280, 0 } },
    } };

    // Every reward varbit the rows gate on, Death's included, so a "nothing
    // claimed" snapshot denies all four portals.
    constexpr std::array<std::int32_t, 4> kRewardVarbits{ 16023, 16024, 16025, 16026 };

    // RAII over the C ABI handles the suite plans with.
    struct CHandles
    {
        ww_artifact     *artifact{nullptr};
        ww_context_pool *pool{nullptr};

        explicit CHandles(const char *wwaPath)
        {
            artifact = ww_artifact_open(wwaPath);
            pool = artifact != nullptr ? ww_context_pool_create(artifact, 1) : nullptr;
        }

        ~CHandles()
        {
            ww_context_pool_destroy(pool);
            ww_artifact_close(artifact);
        }

        CHandles(const CHandles &) = delete;
        CHandles &operator=(const CHandles &) = delete;
    };

    // A player's reward varbits: every one 0, except `claimed` at 1 when set.
    // The snapshot borrows `entries`, so the two travel together.
    struct Rewards
    {
        std::vector<WwCapabilityEntry> entries;
        WwCapabilitySnapshot           snapshot{};

        explicit Rewards(std::int32_t claimed = 0)
        {
            for (const std::int32_t id : kRewardVarbits)
            {
                entries.push_back(WwCapabilityEntry{ id, id == claimed ? 1 : 0 });
            }
            snapshot.varbits = entries.data();
            snapshot.varbitCount = entries.size();
        }

        Rewards(const Rewards &) = delete;
        Rewards &operator=(const Rewards &) = delete;
    };

    struct Planned
    {
        bool                isFound{false};
        std::vector<WwStep> steps;
    };

    Planned plan(CHandles &h, WwTile start, WwTile goal, const Rewards &rewards)
    {
        WwPath path{};
        Planned out;
        out.isFound = ww_query(h.artifact, h.pool, start, WwGoal{ goal.x, goal.y, goal.plane, 0 },
                               &rewards.snapshot, &path) == WW_OK;
        out.steps.assign(path.steps, path.steps + path.stepCount);
        ww_path_free(&path);
        return out;
    }

    // Whether a Transition step of `p` clicks `objectId`.
    bool usesLoc(const Planned &p, std::span<const ww::format::TransitionRecord> txs,
                 std::int32_t objectId)
    {
        for (const WwStep &s : p.steps)
        {
            if (s.kind == WW_STEP_KIND_TRANSITION && s.transitionIndex < txs.size()
                && txs[s.transitionIndex].objectId == objectId)
            {
                return true;
            }
        }
        return false;
    }

    // The derived ladders over the stronghold climbed onto plane 1, which is
    // empty sky there, and the planner walked it between levels. Every row
    // keeps the player on plane 0, so any other plane is that bug back.
    bool staysOnGround(const Planned &p)
    {
        for (const WwStep &s : p.steps)
        {
            if (s.plane != 0)
            {
                return false;
            }
        }
        return true;
    }

    int expect(bool isOk, const std::string &what)
    {
        std::printf("sos: %s %s\n", isOk ? "ok  " : "FAIL", what.c_str());
        return isOk ? 0 : 1;
    }

    // One leg of the descent: found, on plane 0, through `viaLoc`, and not
    // through a portal (nothing is claimed).
    int checkLeg(CHandles &h, std::span<const ww::format::TransitionRecord> txs, const char *what,
                 WwTile start, WwTile goal, std::int32_t viaLoc)
    {
        const Rewards none;
        const Planned p = plan(h, start, goal, none);
        bool isPortalFree = true;
        for (const Level &level : kLevels)
        {
            isPortalFree = isPortalFree && !usesLoc(p, txs, level.portalLoc);
        }
        const bool isOk = p.isFound && staysOnGround(p) && usesLoc(p, txs, viaLoc) && isPortalFree;
        return expect(isOk, std::string(what) + " (" + std::to_string(p.steps.size()) + " steps)");
    }

    // A level's start room plans through its portal only once its reward is
    // claimed.
    int checkPortal(CHandles &h, std::span<const ww::format::TransitionRecord> txs,
                    const Level &level)
    {
        const Rewards none;
        const Rewards claimed(level.rewardVarbit);
        const Planned walked = plan(h, level.startRoom, level.rewardRoom, none);
        const Planned jumped = plan(h, level.startRoom, level.rewardRoom, claimed);
        int failures = expect(walked.isFound && !usesLoc(walked, txs, level.portalLoc),
                              std::string(level.name) + " start -> reward walks while unclaimed");
        failures += expect(jumped.isFound && usesLoc(jumped, txs, level.portalLoc),
                           std::string(level.name) + " start -> reward takes the portal once claimed");
        return failures;
    }
}

int runSosTests(const char *wwaPath)
{
    try
    {
        const ww::format::ArtifactReader reader(wwaPath);
        CHandles h(wwaPath);
        if (h.artifact == nullptr || h.pool == nullptr)
        {
            std::printf("sos: could not open %s through the C ABI: %s\n", wwaPath, ww_last_error());
            return 1;
        }
        const auto txs = reader.transitions();
        const Level &war = kLevels[0];
        const Level &famine = kLevels[1];
        const Level &pestilence = kLevels[2];
        int failures = checkLeg(h, txs, "Barbarian Village -> War reward room", kBarbarianVillage,
                                war.rewardRoom, kEntrance);
        failures += checkLeg(h, txs, "War reward room -> Famine reward room", war.rewardRoom,
                             famine.rewardRoom, kWarLadderDown);
        failures += checkLeg(h, txs, "Famine reward room -> Pestilence reward room",
                             famine.rewardRoom, pestilence.rewardRoom, kFamLadderDown);
        failures += checkLeg(h, txs, "Pestilence reward room -> Barbarian Village",
                             pestilence.rewardRoom, kBarbarianVillage, kWarLadderUp);
        for (const Level &level : kLevels)
        {
            failures += checkPortal(h, txs, level);
        }
        std::printf("sos: %d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    catch (const std::exception &e)
    {
        std::printf("sos: %s\n", e.what());
        return 1;
    }
}
