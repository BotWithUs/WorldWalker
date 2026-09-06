#include "cli/WallShapeTests.h"

#include "data/DoorEdges.h"
#include "format/ClipFlags.h"

#include <array>
#include <cstdint>
#include <cstdio>

// Phase 6b — Wall-shape unit tests over ww::data::doorEdgeMask.
//
// The wall-shape math is well-defined geometry: shape 0 = a single straight
// wall edge on one side; shape 1 = a single diagonal across one corner; shape
// 2 = an L-bend covering two adjacent sides; shape 3 = a thicker diagonal
// equivalent to shape 1 for collision purposes. Each rotation turns the pattern
// 90 degrees clockwise around the source tile.
//
// Every check below calls doorEdgeMask. The vendored reference values survive
// only as the EXPECTED side of the first check; nothing here re-derives what the
// function is supposed to return.
namespace
{
    using ww::data::doorEdgeMask;

    using ww::format::CLIP_WALL_E;
    using ww::format::CLIP_WALL_N;
    using ww::format::CLIP_WALL_NE;
    using ww::format::CLIP_WALL_NW;
    using ww::format::CLIP_WALL_S;
    using ww::format::CLIP_WALL_SE;
    using ww::format::CLIP_WALL_SW;
    using ww::format::CLIP_WALL_W;

    constexpr int kShapeCount    = 4;   // shapes 0..3 are the wall-bearing ones
    constexpr int kRotationCount = 4;

    // Vendored expected values: the own-tile wall bits the prior reference math
    // at collision_map.cpp:91 produces per (shape, rotation). Shapes 4..8 are
    // corner/diagonal decoration variants the reference maps onto shape 1 at the
    // call site; shapes 9..21 are objects and shape 22 is floor decoration —
    // none of those produce wall edges, which is the tail of this check.
    constexpr std::array<std::array<std::uint32_t, kRotationCount>, kShapeCount> kExpected{{
        // shape 0 — straight wall along one side.
        {{CLIP_WALL_W, CLIP_WALL_N, CLIP_WALL_E, CLIP_WALL_S}},
        // shape 1 — thin diagonal across one corner.
        {{CLIP_WALL_NW, CLIP_WALL_NE, CLIP_WALL_SE, CLIP_WALL_SW}},
        // shape 2 — L-bend covering two adjacent sides.
        {{CLIP_WALL_W | CLIP_WALL_N, CLIP_WALL_N | CLIP_WALL_E,
          CLIP_WALL_E | CLIP_WALL_S, CLIP_WALL_S | CLIP_WALL_W}},
        // shape 3 — thick diagonal; same collision as shape 1.
        {{CLIP_WALL_NW, CLIP_WALL_NE, CLIP_WALL_SE, CLIP_WALL_SW}},
    }};

    // Edge-pair contract: a wall edge on a tile is the same physical wall as the
    // opposing edge on the neighbour across it. Used to reflect a whole mask.
    struct PairRule
    {
        std::uint32_t flag;
        std::uint32_t opposite;
    };

    constexpr std::array<PairRule, 8> kPairRules{{
        {CLIP_WALL_N,  CLIP_WALL_S },
        {CLIP_WALL_S,  CLIP_WALL_N },
        {CLIP_WALL_E,  CLIP_WALL_W },
        {CLIP_WALL_W,  CLIP_WALL_E },
        {CLIP_WALL_NE, CLIP_WALL_SW},
        {CLIP_WALL_SW, CLIP_WALL_NE},
        {CLIP_WALL_NW, CLIP_WALL_SE},
        {CLIP_WALL_SE, CLIP_WALL_NW},
    }};

    // Every edge of `mask` replaced by the edge it reflects onto its neighbour.
    std::uint32_t reflectMask(std::uint32_t mask)
    {
        std::uint32_t out = 0u;
        for (const PairRule &rule : kPairRules)
        {
            if ((mask & rule.flag) != 0u)
            {
                out |= rule.opposite;
            }
        }
        return out;
    }

    // 90-degree CW turn of an edge bit. In RuneScape coordinates +Y is north and
    // the shape table steps rot 0 = W -> rot 1 = N -> rot 2 = E -> rot 3 = S,
    // which is W->N->E->S: a CW step in north-up view.
    // CW on cardinals: N->E, E->S, S->W, W->N. CW on diagonals: NW->NE->SE->SW.
    std::uint32_t rotateFlagCw(std::uint32_t flag)
    {
        std::uint32_t out = 0u;
        if ((flag & CLIP_WALL_N)  != 0u) { out |= CLIP_WALL_E;  }
        if ((flag & CLIP_WALL_E)  != 0u) { out |= CLIP_WALL_S;  }
        if ((flag & CLIP_WALL_S)  != 0u) { out |= CLIP_WALL_W;  }
        if ((flag & CLIP_WALL_W)  != 0u) { out |= CLIP_WALL_N;  }
        if ((flag & CLIP_WALL_NW) != 0u) { out |= CLIP_WALL_NE; }
        if ((flag & CLIP_WALL_NE) != 0u) { out |= CLIP_WALL_SE; }
        if ((flag & CLIP_WALL_SE) != 0u) { out |= CLIP_WALL_SW; }
        if ((flag & CLIP_WALL_SW) != 0u) { out |= CLIP_WALL_NW; }
        return out;
    }

    // Check 1 — production agrees with the vendored reference values, and every
    // shape outside 0..3 owns no wall edge at any rotation.
    int checkAgainstReference()
    {
        int failures = 0;
        for (int shape = 0; shape < kShapeCount; ++shape)
        {
            for (int rot = 0; rot < kRotationCount; ++rot)
            {
                const std::uint32_t got =
                    doorEdgeMask(static_cast<std::uint8_t>(shape),
                                 static_cast<std::uint8_t>(rot));
                const std::uint32_t want =
                    kExpected[static_cast<std::size_t>(shape)][static_cast<std::size_t>(rot)];
                if (got != want)
                {
                    std::printf("walltest: FAIL reference — shape=%d rot=%d doorEdgeMask=0x%08x,"
                                " expected 0x%08x\n", shape, rot, got, want);
                    ++failures;
                }
            }
        }
        for (int shape = 4; shape <= 22; ++shape)
        {
            for (int rot = 0; rot < kRotationCount; ++rot)
            {
                const std::uint32_t got =
                    doorEdgeMask(static_cast<std::uint8_t>(shape),
                                 static_cast<std::uint8_t>(rot));
                if (got != 0u)
                {
                    std::printf("walltest: FAIL reference — non-wall shape=%d rot=%d owns"
                                " edges 0x%08x\n", shape, rot, got);
                    ++failures;
                }
            }
        }
        return failures;
    }

    // Check 2 — reflection. A shape's edges reflected onto their neighbours are
    // the same edges the shape turned 180 degrees owns on its own tile, so a
    // single-side regression (one rotation's entry edited alone) breaks the pair.
    int checkReflectionInvariant(int shape)
    {
        int failures = 0;
        for (int rot = 0; rot < kRotationCount; ++rot)
        {
            const std::uint32_t mask =
                doorEdgeMask(static_cast<std::uint8_t>(shape), static_cast<std::uint8_t>(rot));
            const std::uint32_t opposed =
                doorEdgeMask(static_cast<std::uint8_t>(shape),
                             static_cast<std::uint8_t>((rot + 2) & 3));
            const std::uint32_t reflected = reflectMask(mask);
            if (reflected != opposed)
            {
                std::printf("walltest: FAIL reflection — shape=%d rot=%d mask=0x%08x reflects to"
                            " 0x%08x, but rot=%d owns 0x%08x\n",
                            shape, rot, mask, reflected, (rot + 2) & 3, opposed);
                ++failures;
            }
            if (mask == 0u)
            {
                std::printf("walltest: FAIL reflection — shape=%d rot=%d owns no wall edge\n",
                            shape, rot);
                ++failures;
            }
        }
        return failures;
    }

    // Check 3 — rotational symmetry. Rotation N is rotation 0 turned CW N times,
    // and the rotation field is two bits wide, so a full turn is the identity.
    int checkRotationInvariant(int shape)
    {
        int failures = 0;
        std::uint32_t expected = doorEdgeMask(static_cast<std::uint8_t>(shape), 0);
        for (int rot = 1; rot < kRotationCount; ++rot)
        {
            expected = rotateFlagCw(expected);
            const std::uint32_t got =
                doorEdgeMask(static_cast<std::uint8_t>(shape), static_cast<std::uint8_t>(rot));
            if (got != expected)
            {
                std::printf("walltest: FAIL rotation — shape=%d rot=%d doorEdgeMask=0x%08x,"
                            " CW-derived from rot 0 is 0x%08x\n", shape, rot, got, expected);
                ++failures;
            }
        }
        for (int rot = 0; rot < kRotationCount; ++rot)
        {
            const std::uint32_t base =
                doorEdgeMask(static_cast<std::uint8_t>(shape), static_cast<std::uint8_t>(rot));
            const std::uint32_t wrapped =
                doorEdgeMask(static_cast<std::uint8_t>(shape), static_cast<std::uint8_t>(rot + 4));
            if (base != wrapped)
            {
                std::printf("walltest: FAIL rotation — shape=%d rot=%d did not wrap to rot=%d\n",
                            shape, rot + 4, rot);
                ++failures;
            }
        }
        return failures;
    }
}

int runWallShapeTests()
{
    std::printf("walltest: %d (shape, rotation) cases under test against ww::data::doorEdgeMask\n",
                kShapeCount * kRotationCount);

    const int refFail = checkAgainstReference();
    std::printf("walltest: vendored reference values — %d/%d cases ok\n",
                (kShapeCount * kRotationCount) - refFail, kShapeCount * kRotationCount);

    int reflectOk = 0;
    int rotOk = 0;
    int reflectFail = 0;
    int rotFail = 0;
    for (int shape = 0; shape < kShapeCount; ++shape)
    {
        const int r = checkReflectionInvariant(shape);
        reflectFail += r;
        reflectOk += (r == 0) ? 1 : 0;
        const int t = checkRotationInvariant(shape);
        rotFail += t;
        rotOk += (t == 0) ? 1 : 0;
    }
    std::printf("walltest: reflection invariant — %d/%d shapes ok\n", reflectOk, kShapeCount);
    std::printf("walltest: rotational symmetry — %d/%d shapes ok\n", rotOk, kShapeCount);

    const int totalFail = refFail + reflectFail + rotFail;
    if (totalFail == 0)
    {
        std::printf("walltest: PASS (all invariants hold)\n");
        return 0;
    }
    std::printf("walltest: FAIL (%d failures)\n", totalFail);
    return 1;
}
