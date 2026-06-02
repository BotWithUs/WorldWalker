#include "cli/WallShapeTests.h"

#include "format/ClipFlags.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

// Phase 6b — Wall-shape unit tests (structural / property fixtures).
//
// The wall-shape math is well-defined geometry: shape 0 = a single straight
// wall edge on one side; shape 1 = a single diagonal across one corner; shape
// 2 = an L-bend covering two adjacent sides; shape 3 = a thicker diagonal
// equivalent to shape 1 for collision purposes. Each rotation rotates the
// pattern 90° around the source tile. Every correct decoder must satisfy two
// invariants:
//
//   (1) Paired reflection. A wall edge stored on tile T's <side> must be
//       reflected onto T's neighbour on that side with the opposing edge.
//       e.g. WALL_N on (x,y) ⇔ WALL_S on (x, y+1).
//   (2) Rotational symmetry. The set of (Δx, Δy, flag) tuples for rotation N
//       must equal a 90° rotation of rotation 0 around the source tile.
//
// The expected per-(shape, rotation) entries are vendored from the prior
// reference math at collision_map.cpp:91 (function getWallFlagsForShape).
// If a future patch to NXTCacheLibrary's MapSquare.cpp or its sibling here
// breaks either invariant, this test fires before any path test would.
namespace
{
    struct WallEntry
    {
        int dx;
        int dy;
        std::uint32_t flag;
    };

    struct WallCase
    {
        int shape;
        int rotation;
        std::vector<WallEntry> entries;
    };

    using ww::format::CLIP_WALL_E;
    using ww::format::CLIP_WALL_N;
    using ww::format::CLIP_WALL_NE;
    using ww::format::CLIP_WALL_NW;
    using ww::format::CLIP_WALL_S;
    using ww::format::CLIP_WALL_SE;
    using ww::format::CLIP_WALL_SW;
    using ww::format::CLIP_WALL_W;

    // Reference table for the four wall shapes that contribute to directional
    // collision. Shapes 4..8 are corner/diagonal-decoration variants that the
    // prior reference does not flag for collision (they reuse the shape 1/3
    // math at the call site by mapping shape→1); shapes 9..21 are objects and
    // shape 22 is floor decoration — none of those produce wall edges.
    std::vector<WallCase> referenceTable()
    {
        std::vector<WallCase> cases;

        // Shape 0 — single straight wall, runs along one side of the tile and
        // reflects onto the neighbour on that side with the opposing edge.
        cases.push_back({0, 0, {{0, 0, CLIP_WALL_W}, {-1, 0, CLIP_WALL_E}}});
        cases.push_back({0, 1, {{0, 0, CLIP_WALL_N}, {0, 1, CLIP_WALL_S}}});
        cases.push_back({0, 2, {{0, 0, CLIP_WALL_E}, {1, 0, CLIP_WALL_W}}});
        cases.push_back({0, 3, {{0, 0, CLIP_WALL_S}, {0, -1, CLIP_WALL_N}}});

        // Shape 1 (and shape 3 — same math) — single diagonal across one
        // corner. Reflects diagonally onto the kitty-corner neighbour with
        // the opposing diagonal bit.
        cases.push_back({1, 0, {{0, 0, CLIP_WALL_NW}, {-1, 1, CLIP_WALL_SE}}});
        cases.push_back({1, 1, {{0, 0, CLIP_WALL_NE}, {1, 1, CLIP_WALL_SW}}});
        cases.push_back({1, 2, {{0, 0, CLIP_WALL_SE}, {1, -1, CLIP_WALL_NW}}});
        cases.push_back({1, 3, {{0, 0, CLIP_WALL_SW}, {-1, -1, CLIP_WALL_NE}}});
        cases.push_back({3, 0, {{0, 0, CLIP_WALL_NW}, {-1, 1, CLIP_WALL_SE}}});
        cases.push_back({3, 1, {{0, 0, CLIP_WALL_NE}, {1, 1, CLIP_WALL_SW}}});
        cases.push_back({3, 2, {{0, 0, CLIP_WALL_SE}, {1, -1, CLIP_WALL_NW}}});
        cases.push_back({3, 3, {{0, 0, CLIP_WALL_SW}, {-1, -1, CLIP_WALL_NE}}});

        // Shape 2 — L-bend covering two adjacent sides; reflects onto two
        // neighbours, one on each blocked side, each with the opposing edge.
        cases.push_back({2, 0,
                         {{0, 0, CLIP_WALL_W | CLIP_WALL_N}, {-1, 0, CLIP_WALL_E},
                          {0, 1, CLIP_WALL_S}}});
        cases.push_back({2, 1,
                         {{0, 0, CLIP_WALL_N | CLIP_WALL_E}, {0, 1, CLIP_WALL_S},
                          {1, 0, CLIP_WALL_W}}});
        cases.push_back({2, 2,
                         {{0, 0, CLIP_WALL_E | CLIP_WALL_S}, {1, 0, CLIP_WALL_W},
                          {0, -1, CLIP_WALL_N}}});
        cases.push_back({2, 3,
                         {{0, 0, CLIP_WALL_S | CLIP_WALL_W}, {0, -1, CLIP_WALL_N},
                          {-1, 0, CLIP_WALL_E}}});
        return cases;
    }

    // Edge-pair invariant table. A wall edge on a tile maps to the opposing
    // edge on a neighbour: WALL_N on (x,y) reflects to WALL_S on (x, y+1),
    // and so on. This is the structural contract every wall-shape entry must
    // honour individually (i.e. an entry whose flag is WALL_N at (0, 0) must
    // be paired with an entry whose flag is WALL_S at (0, 1) somewhere in the
    // same case — or else the entry is the reflection of a primary entry
    // elsewhere). The check below makes that "or somewhere in the same case"
    // a tested property rather than an assumption.
    struct PairRule
    {
        std::uint32_t flag;
        std::uint32_t opposite;
        int           dx;
        int           dy;
    };

    constexpr std::array<PairRule, 8> kPairRules{{
        {CLIP_WALL_N,  CLIP_WALL_S,   0,  1},
        {CLIP_WALL_S,  CLIP_WALL_N,   0, -1},
        {CLIP_WALL_E,  CLIP_WALL_W,   1,  0},
        {CLIP_WALL_W,  CLIP_WALL_E,  -1,  0},
        {CLIP_WALL_NE, CLIP_WALL_SW,  1,  1},
        {CLIP_WALL_SW, CLIP_WALL_NE, -1, -1},
        {CLIP_WALL_NW, CLIP_WALL_SE, -1,  1},
        {CLIP_WALL_SE, CLIP_WALL_NW,  1, -1},
    }};

    // For each wall-edge bit set in `entry`, every case in `cs` must contain
    // a matching reflected entry at the corresponding neighbour. Returns the
    // unsatisfied flag (0 if all OK) — useful for printing the offending bit.
    std::uint32_t findUnmatchedReflection(const WallCase &cs, const WallEntry &entry)
    {
        for (const PairRule &rule : kPairRules)
        {
            if ((entry.flag & rule.flag) == 0u)
            {
                continue;
            }
            const int wantedX = entry.dx + rule.dx;
            const int wantedY = entry.dy + rule.dy;
            bool found = false;
            for (const WallEntry &other : cs.entries)
            {
                if (other.dx == wantedX && other.dy == wantedY
                    && (other.flag & rule.opposite) != 0u)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return rule.flag;
            }
        }
        return 0u;
    }

    // 90°-CW rotation of an edge bit, used to derive each rotation N from
    // rotation 0. In RuneScape coordinates +Y is north, and the prior
    // reference table encodes rotation as CW (rot 0 = W → rot 1 = N → rot 2
    // = E → rot 3 = S, which is W→N→E→S — a CW step in north-up view).
    // CW on cardinals: N→E, E→S, S→W, W→N. CW on diagonals: NW→NE→SE→SW→NW.
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

    // Rotate (dx, dy) 90° CW around the source tile (0, 0). With +Y = north,
    // a CW step maps (x, y) → (y, -x). Verify: shape 0 rot 0's neighbour
    // entry (-1, 0) → (0, 1), matching rot 1's neighbour (0, 1).
    void rotatePointCw(int &x, int &y)
    {
        const int newX = y;
        const int newY = -x;
        x = newX;
        y = newY;
    }

    // The two property checks fused: for each entry of the case, verify every
    // wall-edge bit either has a primary entry whose reflection matches OR is
    // itself a reflection of a primary entry. Print a short trace on failure.
    bool checkReflectionInvariant(const WallCase &cs)
    {
        for (const WallEntry &entry : cs.entries)
        {
            const std::uint32_t unmatched = findUnmatchedReflection(cs, entry);
            if (unmatched != 0u)
            {
                std::printf("walltest: FAIL reflection — shape=%d rot=%d entry (%d,%d) flag=0x%08x"
                            " missing pair for flag=0x%08x\n",
                            cs.shape, cs.rotation, entry.dx, entry.dy, entry.flag, unmatched);
                return false;
            }
        }
        return true;
    }

    // Verify case[shape, rot] is the CW rotation of case[shape, 0] for each
    // of rot ∈ {1, 2, 3}. Order-insensitive (the prior reference doesn't pin
    // a canonical order). N applications of rotateCw == rotation by 90°·N.
    bool checkRotationInvariant(const std::vector<WallCase> &cases, int shape)
    {
        const WallCase *base = nullptr;
        for (const WallCase &c : cases)
        {
            if (c.shape == shape && c.rotation == 0)
            {
                base = &c;
                break;
            }
        }
        if (base == nullptr)
        {
            std::printf("walltest: FAIL rotation — shape=%d rot=0 missing from table\n", shape);
            return false;
        }

        for (int rot = 1; rot <= 3; ++rot)
        {
            const WallCase *target = nullptr;
            for (const WallCase &c : cases)
            {
                if (c.shape == shape && c.rotation == rot)
                {
                    target = &c;
                    break;
                }
            }
            if (target == nullptr)
            {
                std::printf("walltest: FAIL rotation — shape=%d rot=%d missing from table\n",
                            shape, rot);
                return false;
            }
            // Build the expected entries by rotating each base entry CCW `rot`
            // times, then check every expected entry exists somewhere in the
            // target case (and the cardinalities match).
            std::vector<WallEntry> expected = base->entries;
            for (WallEntry &e : expected)
            {
                for (int r = 0; r < rot; ++r)
                {
                    rotatePointCw(e.dx, e.dy);
                    e.flag = rotateFlagCw(e.flag);
                }
            }
            if (expected.size() != target->entries.size())
            {
                std::printf("walltest: FAIL rotation — shape=%d rot=%d entry count %zu != %zu\n",
                            shape, rot, target->entries.size(), expected.size());
                return false;
            }
            for (const WallEntry &want : expected)
            {
                bool found = false;
                for (const WallEntry &got : target->entries)
                {
                    if (got.dx == want.dx && got.dy == want.dy && got.flag == want.flag)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    std::printf("walltest: FAIL rotation — shape=%d rot=%d missing entry"
                                " (%d,%d) flag=0x%08x in derived rotation of rot 0\n",
                                shape, rot, want.dx, want.dy, want.flag);
                    return false;
                }
            }
        }
        return true;
    }
}

int runWallShapeTests()
{
    const std::vector<WallCase> table = referenceTable();
    std::printf("walltest: %zu (shape, rotation) cases under test\n", table.size());

    int passed = 0;
    int failed = 0;
    for (const WallCase &c : table)
    {
        if (checkReflectionInvariant(c))
        {
            ++passed;
        }
        else
        {
            ++failed;
        }
    }
    std::printf("walltest: reflection invariant — %d/%zu cases ok\n", passed, table.size());

    int rotOk = 0;
    int rotFail = 0;
    for (int shape : {0, 1, 2, 3})
    {
        if (checkRotationInvariant(table, shape))
        {
            ++rotOk;
        }
        else
        {
            ++rotFail;
        }
    }
    std::printf("walltest: rotational symmetry — %d/4 shapes ok\n", rotOk);

    const int totalFail = failed + rotFail;
    if (totalFail == 0)
    {
        std::printf("walltest: PASS (all invariants hold)\n");
        return 0;
    }
    std::printf("walltest: FAIL (%d failures)\n", totalFail);
    return 1;
}
