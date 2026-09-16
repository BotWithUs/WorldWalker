# Contributing to WorldWalker

Thanks for helping. This document tells you which changes you can make with
nothing but a clone, which ones need a maintainer, and what a reviewable PR
looks like.

## Start here: what do you want to change?

| Change | What you need | Bake required? |
|---|---|---|
| A spell or item teleport | A released `.wwa` + `wwcli` | **No** |
| The planner, executor, C ABI, `wwcli` | MSVC + CMake + vcpkg | **No** |
| `transport_links.json` / `teleport_chains.json` | A maintainer bake | Yes |
| The collision builder (`src/build/`) | A maintainer bake | Yes |

The first two rows are the whole library minus the cache decoder. Baking needs
`NXTCache.dll` from a sibling repository that is not yet public — see
[`docs/artifact-delivery.md`](docs/artifact-delivery.md) §7 for why, and what
the options for closing that gap are. If your change lands in the bottom two
rows, open the PR anyway with the JSON or source diff; a maintainer bakes it.

## Before you write code

Read [`CONTEXT.md`](CONTEXT.md). It is short, and it defines every term this
codebase uses — Tile, Mapsquare, Loc, Transition, Capability snapshot — along
with the synonyms we deliberately avoid. A PR that calls a Mapsquare a "region"
will get review comments about vocabulary before anyone looks at the logic, and
that wastes everyone's round trip.

If your change reverses or complicates an architectural decision, check
[`docs/adr/`](docs/adr/) first. Each ADR records what was chosen, what was
rejected, and what it cost. Several also name the conditions under which they
should be reopened — if you have hit one of those, say so in the PR and we will
reopen the ADR rather than route around it.

## Building

From a `vcvars64` shell:

```powershell
cmake -B build
cmake --build build --config Release
```

Without NXTCacheLibrary present, `wwbuild` is dropped from the build and the two
commands above still produce `worldwalker.dll` and `wwcli.exe`. That is the
expected contributor setup, not a degraded one — no flags needed. Two knobs if
you need them:

| Flag | Effect |
|---|---|
| `-DWORLDWALKER_BUILD_WWBUILD=ON\|OFF` | Force the baker on or off instead of auto-detecting |
| `-DNXTCACHE_BUILD_DIR=<dir>` | Look for `<CONFIG>/NXTCache.{lib,dll}` somewhere other than `..\NXTCacheLibrary\build` |

Asking for `=ON` without the sibling fails at **configure** time with a message
naming the paths it searched, rather than as a link error later.

`cmake.toml` is the **single source of truth**. `CMakeLists.txt` is generated
from it by [cmkr](https://github.com/build-cpp/cmkr) — never hand-edit it. Both
files are committed; if you add a source file, re-run the configure step so the
globs refresh, and commit the regenerated `CMakeLists.txt` with your change.

The build is `/W4 /WX` on MSVC. Warnings are errors; please do not disable them
locally to get a build through.

## Changing a teleport (no cache needed)

`spell_teleports.json` and `item_teleports.json` are read at artifact-open time,
not baked in, so you can edit them against a released artifact:

```powershell
.\build\Release\wwcli.exe check worldwalker.wwa datasets
```

That appends every global teleport to the artifact in memory and verifies each
destination lands in a seedable area. A teleport whose destination is not in any
area is silently never used by the planner, and shows up downstream as "pathing
ignores my teleport" rather than as a bad dataset — which is exactly why this
check exists and why it is the gate.

See [`datasets/README.md`](datasets/README.md) for each file's schema and role.

## Changing a baked dataset

`transport_links.json` and `teleport_chains.json` are compiled into the area
graph, so verifying one means baking:

```powershell
.\scripts\bake.ps1 -Out build\scratch.wwa
.\build\Release\wwcli.exe check build\scratch.wwa datasets
```

Open the PR with **the JSON diff plus the regenerated sidecar**. Nobody can
review a 10 MB binary; a reviewer reads two text diffs and never opens the
artifact.

One property makes those diffs worth reading:

> A datasets-only change should move `transitions`, `edges` and `datasetHash`,
> and leave `squares` and `collisionFingerprint` **identical**.

A moved `collisionFingerprint` on a datasets-only PR means you baked against
different game data. That is visible at a glance and is almost never what the
author intended.

## Tests

`wwcli` carries the correctness suites. Run the ones your change touches:

```powershell
.\build\Release\wwcli.exe walltest              # wall shape/rotation table
.\build\Release\wwcli.exe instance              # dynamic-region chunk remap
.\build\Release\wwcli.exe scripted <artifact>   # end-to-end path categories
.\build\Release\wwcli.exe doors <artifact>      # door/crossing traversal
.\build\Release\wwcli.exe bench <artifact>      # planner latency
```

If you change planner behaviour, include `bench` numbers before and after.
[ADR 0002](docs/adr/0002-hpa-star-hierarchical-pathfinding.md) records the
measurements the current design was chosen on, and a regression against those is
a real finding.

CI runs the cache-free half of this for you on every PR — it builds without
NXTCacheLibrary, runs `walltest` and `instance`, checks that `CMakeLists.txt`
still matches `cmake.toml`, and runs the acceptance gate against the latest
published artifact. What it cannot do is **bake**, so anything that changes
collision or a baked dataset still needs a local run and a maintainer.

## Style

Match the file you are editing. Across the codebase:

- C++23, four-space indent, braces on their own line.
- Comments explain **why**, not what. The existing comments are load-bearing
  — several record a trap that cost real debugging time. Do not strip them.
- Public C ABI functions are documented in the header, not the `.cpp`.
- No absolute paths, drive letters, or usernames in tracked files. The bake
  already strips usernames from provenance records on purpose.

## Provenance: one hard rule

**Never invent a provenance record.** If an artifact has no provenance, the
honest state is "unknown" — `wwcli` says so and `wwcli check` fails. Do not
retro-stamp an artifact with a record assembled after the fact: an invented
record is worse than an absent one, because nothing downstream can tell it from
a real one. Re-bake instead.

## Reporting a bug

Include the artifact's `collisionFingerprint` and `datasetHash` from the
sidecar, plus the exact `wwcli` invocation. Most "pathing is wrong" reports turn
out to be a stale artifact, and those two values settle it immediately.

## License

By contributing, you agree that your contributions are licensed under the MIT
License (see [LICENSE](LICENSE)). If you are contributing data rather than code,
note the third-party attribution in [NOTICE](NOTICE) and tell us where your data
came from.
