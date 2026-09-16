# Baking, provenance, and where the artifact lives

How `worldwalker.wwa` gets made, how you tell what any given copy was made from,
which file is the real one, and how it reaches the people who run it.

This document exists because none of those questions had an answer for three
months. A June test fixture (`build/test.wwa`) shipped as the production
navigation artifact, because the deploy payload renamed it to `worldwalker.wwa`
on the way into `data.zip` — so the published name always looked right. That was
not a slip anyone could have caught by reading the payload table. It was the
predictable result of the shipping artifact being untracked build output, sitting
in an ignored directory among seven other `.wwa` files, produced by a hand-run
nobody recorded.

---

## 1. Reproducing the artifact

```powershell
.\scripts\bake.ps1
```

That is the whole thing. It bakes `build\worldwalker.wwa` from tracked inputs,
writes the provenance sidecar beside it, and runs the acceptance check.

### What it needs, and where that comes from on a machine that has never run this

| Input | Where it comes from |
|---|---|
| **RS3 cache** | `C:\ProgramData\Jagex\RuneScape` — the **NXT client's own cache**. Any machine with RS3 installed already has it. Nothing needs to be handed over. |
| **Datasets** | `datasets\` in this repo. Tracked, version-controlled, human-editable. See `datasets/README.md`. |
| **`wwbuild.exe` / `wwcli.exe`** | `cmake -B build && cmake --build build --config Release`, from a vcvars64 shell. |
| **`NXTCache.dll`** | Built by the sibling **NXTCacheLibrary** repo; `cmake.toml` imports it from `..\NXTCacheLibrary\build\<CONFIG>\`. |

The cache is a directory of **`js5-<N>.jcache` SQLite databases**. It is *not* the
old Java client's `main_file_cache.dat2` layout — nothing in this workspace reads
that, and the one place the name appears is a stale comment in
`deriveCacheRevision` (see §3).

### `--live` is the default

`wwbuild` takes `--live`, which lets NXTCacheLibrary complete missing reference
tables and archives from the live JS5 servers on demand (`RSCache::enableLiveFallback`,
and the fallback arms in `Index::Index` / `Index::archive`). `bake.ps1` passes it
by default. Two reasons:

- **A player's local cache only holds what that player has downloaded.** A
  local-only bake is therefore contributor-dependent: two people bake the same
  datasets, get different artifacts, and nothing says why. `--live` is what makes
  every contributor's bake converge on the same complete world.
- **It removes the multi-GB hand-off.** A contributor does not need anyone to
  send them a cache dump, which is the precondition for outside contributions
  working at all.

`-NoLive` bakes exactly what is on disk. Use it deliberately, not by accident. A
cold cache under `--live` means tens of thousands of archive fetches and a long,
network-bound run; a normal player's cache only needs the gaps filled. The script
warns when it sees an empty cache directory.

JS5 is unauthenticated public content distribution — the same thing the game
client does on every launch. No account is involved.

A bake against a complete local cache takes **seconds**, not minutes: measured at
~11 s for a full `build` and ~6 s for `collision`. Budget for a long run only when
the cache is cold and `--live` has to fetch the world.

### One cosmetic trap when capturing output

`wwbuild` writes a benign warning to stderr when an optional dataset is absent
(`dataset not found, skipping: teleport_chains.json` — `teleport_chains.json` is
legitimately not in the tree). Run in a terminal it is one plain line. Captured
through **PowerShell 5.1**, native stderr gets wrapped in a `NativeCommandError`
record, so that one warning renders as a red multi-line error block and `$?` goes
`$false` even though the process exited 0. Nothing is wrong; do not go looking for
a bug, and do not branch on `$?` around a native call. Branch on `$LASTEXITCODE`,
which is what `bake.ps1` does.

---

## 2. Provenance: what a given artifact was baked from

Every bake stamps the artifact with its own origin, in two places:

- **Inside the `.wwa`**, as a `Provenance` section (section id 6).
- **Beside it**, as `<artifact>.json` — the same document, pretty-printed.

Both carry:

| Field | Meaning |
|---|---|
| `builtAtUtc`, `command` | When, and whether this was a full `build` or a collision-only bake |
| `sourceVersion`, `datasetVersion` | git revisions, supplied by `bake.ps1`; `null` when unrecorded |
| `wwbuildBuiltAt` | compile stamp of the `wwbuild` binary that produced it |
| `cacheId`, `cacheKind` | cache directory **basename** and `local` / `local+live` |
| `collisionFingerprint` | **FNV-1a 64 over every baked clip word** — see below |
| `datasetHash`, `datasetFiles` | the aggregate hash plus per-file name/size/fingerprint |
| `counts` | squares, transitions, areas, edges, landmarks, teleport zones |
| `artifactSha256`, `artifactBytes` | added to the sidecar by `bake.ps1` after the write |

### Why the section needs no format bump

`kArtifactFormatVersion` is a **hard gate**: the reader refuses a mismatch
outright (ADR 0005), so bumping it strands every client already in the field.
Provenance does not bump it, and must not.

`ArtifactReader::decodeSection` has skipped unknown section ids since its first
commit — out-of-range ids return early, in-range-but-unmapped ids take the
`default` arm. Every reader that has ever shipped therefore loads a provenanced
artifact unchanged. This was verified by experiment before the code was written:
injecting a section into the real shipping artifact left the full `wwcli` harness
output byte-identical, for both the in-range and out-of-range paths.

One detail worth keeping in mind if you touch the writer: the reader
bounds-checks a section entry *before* deciding to skip it, so a malformed
offset/length still throws on an old client. Write them correctly.

### `collisionFingerprint` is the number that means something

It is FNV-1a 64 over the clip words of every baked square, in square order. It is
machine-independent, immune to mtimes, `--live` and sparse local caches, and
equal across two bakes of the same game data. **This is the value to compare when
asking "is this artifact stale".**

### Never invent a provenance record

If an artifact has no provenance, the honest state is "unknown" — `wwcli` says so,
and `wwcli check` fails. Do **not** retro-stamp an existing artifact with a record
assembled after the fact: an invented record is worse than an absent one, because
nothing downstream can tell it from a real one. Re-bake instead.

---

## 3. `cacheRevision` in the header is not what its name says

ADR 0005 describes a safety property: a cache-revision mismatch soft-warns that
the artifact predates the running game data. **That has never worked.**

`deriveCacheRevision` hashes the mtimes of `main_file_cache.dat2` and
`main_file_cache.js5`. Those files **do not exist** — they are the old Java
client's layout, the NXT cache is `js5-<N>.jcache`, and those two names appear
nowhere else in this workspace. Both stamps read 0 on every machine, always have,
so the value collapses to a hash of the cache directory's own mtime: it moves
when the client merely runs, and can sit still when the map data changes.

The field is still written, unchanged, so nothing downstream shifts behaviour.
`cacheDirMtime` in the provenance record preserves its raw input, honestly
labelled. Repointing the runtime's soft-warn at `collisionFingerprint` is the real
fix and is tracked separately — it is a behaviour change, not a provenance change.

Two revisions that would be genuinely meaningful exist in NXTCacheLibrary and are
not exposed over its C ABI: `Index::version` (the map index's reference-table
version, available offline) and `ServerConfig::serverVersionMajor` (the actual
RS3 build number, available under `--live`). `cacheIndexVersion` and
`serverVersion` are reserved for them and record as `null` until the ABI offers
them. They are corroboration; `collisionFingerprint` is the signal.

---

## 4. Which `.wwa` is canonical

1. **`build\worldwalker.wwa` is the only name a bake writes and the only name the
   deploy payload reads.** `bake.ps1` defaults to it; writing anywhere else takes
   an explicit `-Out`.
2. **Everything else under `build\` is scratch.** Not tracked, not shipped, not
   promised, and safe for anyone to delete. `build\` is gitignored, so it cannot
   hold anything anyone is meant to rely on — that, not the file count, was the
   root cause.
3. **Regression fixtures do not live in `build\`.** A deliberate fixture belongs
   somewhere tracked, named for what it pins.
4. **A scratch bake is a scratch bake.** Pass `-Out` and it stays out of the way
   of the shipping name. The failure this prevents is exactly the one that
   happened: a fixture becoming the shipping artifact because the payload renamed
   it at publish time.

---

## 5. Where the shipped artifact lives

**A GitHub release asset on `BotWithUs/WorldWalker`, with the sidecar tracked in
`artifacts\worldwalker.wwa.json`.**

Not tracked in-repo. The repo's entire history is ~3 MB; the artifact is ~10 MB of
already-compressed zlib blobs that git can neither delta nor compress further.
Committing it would make one bake several times the size of all the source ever
written, and every rebake would add another ~10 MB to every clone, forever — in a
repo whose whole purpose is to be cloneable by contributors.

Not object storage either, though the rest of the v2 delivery chain uses it:
`data.zip` is the delivery path to **players**, while `worldwalker.wwa` is a build
input for **maintainers and contributors**. Keeping it next to the code that
produced it, readable by anyone who can read the repo, keeps working on the day
this repo goes public.

### Publishing

```powershell
.\scripts\bake.ps1 -ReleaseTag wwa-2026-09-13
gh release create wwa-2026-09-13 build\worldwalker.wwa -R BotWithUs/WorldWalker
git add artifacts\worldwalker.wwa.json
```

`-ReleaseTag` stamps the tag into the sidecar and copies it to the tracked
manifest. It uploads nothing: publishing is a deliberate act, which is why it
takes a flag rather than happening on every bake.

### What the deploy side expects

`BotWithUs-Launcher\scripts\deploy-data-zip.ps1` reads
`WorldWalker\build\worldwalker.wwa` as a mandatory payload row and already SHA256s
every payload file for its input fingerprint. The intended follow-up is a fetch
step ahead of it that downloads the pinned release asset into `build\` when the
file is absent or its SHA256 does not match `artifacts\worldwalker.wwa.json`. That
closes the bus-factor-of-one without changing the payload table.

The same reasoning applies to `gameval.sqlite` — also cache-derived, also
regenerable, also currently homeless (it is not in the payload table at all).

---

## 6. Contributing a transition change

The contribution surface is **`datasets\*.json`** — tracked, human-editable, and
already the documented source of truth (`datasets/README.md`).

1. Edit the JSON.
2. `.\scripts\bake.ps1 -Out build\scratch.wwa`
3. `.\build\Release\wwcli.exe check build\scratch.wwa datasets`
4. Open a PR with the JSON diff **and the regenerated sidecar**.

### What makes it reviewable

Nobody can review a 10 MB binary. A reviewer reads **two text diffs** — the JSON
change and the sidecar — and never opens the artifact. The sidecar's `counts` is
the bake summary, and it has a property worth knowing:

> A datasets-only change should move `transitions`, `edges` and `datasetHash`,
> and leave `squares` and `collisionFingerprint` **identical**.

A `collisionFingerprint` change on a datasets-only PR means the contributor baked
against different game data. That is visible at a glance and is the single most
useful line in the diff.

### The acceptance gate

`wwcli check <artifact> [dataset_dir]` exits non-zero on any failure:

| Check | Asserts |
|---|---|
| `load` | opens, and the format version matches |
| `sections` | all five content sections present — a collision-only bake is not shippable |
| `populated` | squares, transitions, areas and edges are all non-zero |
| `provenance` | a provenance record exists |
| `teleports` | every global teleport's destination lands in a seedable area |

The `teleports` check earns its place: a global teleport whose destination is not
in any area is silently never used by the planner, and reads downstream as
"pathing ignores my teleport" rather than as a bad bake.

---

## 7. Before this repo can be opened to contributors

Three things stood between this repo and contributors. The third is settled; the
first blocks **less than half** of the contribution surface, which is worth
being precise about.

1. **Only `wwbuild` needs the private sibling.** `cmake.toml` links
   `NXTCache::NXTCache` into the `wwbuild` target alone; `worldwalker` and `wwcli`
   link only zlib and nlohmann_json. Confirmed against the built binaries rather
   than the build files — `dumpbin /dependents` shows `NXTCache.dll` imported by
   `wwbuild.exe` and by neither `worldwalker.dll` nor `wwcli.exe`. So **the runtime
   library and the harness neither link nor load NXTCacheLibrary at all.** What a
   contributor without it cannot do is *bake* — decode the cache.

   **How the build behaves when the sibling is gone — now scoped, and measured:**
   `cmake-after` probes for `../NXTCacheLibrary/src/c_api/nxtcache_c.h` and a
   `<CONFIG>/NXTCache.lib` under `NXTCACHE_BUILD_DIR`, and sets the
   `WORLDWALKER_BUILD_WWBUILD` option from what it finds. When the probe misses,
   the imported target is never declared and the `wwbuild` target is not emitted
   at all (cmkr condition `build-wwbuild`); `worldwalker` and `wwcli` build as
   normal. Verified on this machine, all three paths:

   | Configuration | Result |
   |---|---|
   | Sibling absent, no flags | Configure + `--build` succeed; `worldwalker.dll` + `wwcli.exe` produced, no `wwbuild` |
   | Sibling absent, `-DWORLDWALKER_BUILD_WWBUILD=ON` | `FATAL_ERROR` at configure naming both searched paths |
   | Sibling present, no flags | All three targets build, `wwbuild.exe` included |

   Before this, the sibling's absence was survivable at configure time only by
   accident — CMake does not check an imported target's `IMPORTED_LOCATION` until
   something links it, so `cmake -B build` passed and the default `cmake --build`
   then failed on a `wwbuild` link error. A contributor following the README hit
   that failure and had nothing telling them it was expected.

   The check is deliberately **scoped to the case where `wwbuild` is actually
   going to be built**, which is what an earlier revision of this section warned
   was the whole difficulty: an unconditional existence check would delete the
   split the table below describes, stopping a contributor who touches only the
   runtime-loaded teleport files at configure time over a dependency they never
   use. Auto-detect plus a target condition keeps the split intact — which is why
   it is an option defaulted from a probe, and not a hard `find_package`.

   Cross that with how the datasets are consumed and the contribution surface
   splits in two:

   | Dataset | Needs a bake? | Contributable without the private repo? |
   |---|---|---|
   | `spell_teleports.json` | **No** — loaded at runtime | **Yes** |
   | `item_teleports.json` | **No** — loaded at runtime | **Yes** |
   | `transport_links.json` | Yes — baked into the area graph | No |
   | `teleport_chains.json` | Yes — baked | No |

   The two runtime-loaded files are appended to an already-built artifact at
   open time, so a contributor with a released `.wwa`, `wwcli`, and no cache at all
   can edit them and run `wwcli check <artifact> datasets` end to end. That is not
   theoretical: the gate appends 151 global teleports to an existing artifact and
   verifies every destination is seedable, with no cache involved.

   And that half is exactly the data whose absence caused the incident this
   document opens with. Options for the baked half: publish NXTCacheLibrary, ship
   prebuilt `NXTCache.dll` + headers as a release asset, or keep `wwbuild` internal
   and have a maintainer bake contributed `transport_links` changes.

2. **~~There is no CI.~~** `.github/workflows/ci.yml` now runs on every push and
   PR, on hosted Windows runners, in three jobs:

   | Job | What it proves |
   |---|---|
   | `build` | The repo configures and builds **with no NXTCacheLibrary on the machine** — the contributor configuration — and asserts `wwbuild.exe` was skipped rather than silently required. Then runs `wwcli walltest` and `wwcli instance`, both cache-free. |
   | `acceptance` | Downloads the newest published `worldwalker.wwa` and runs `wwcli check <artifact> datasets` — the teleport-contribution gate. Skips cleanly when no release exists. |
   | `cmkr-drift` | Regenerates with cmkr v0.2.44 and fails if `CMakeLists.txt` or `vcpkg.json` differ from what is committed. |

   The split predicted here held: a hosted runner covers everything except the
   bake. `build` needs no cache and no private sibling, and `acceptance` needs
   only a release asset. **A full bake is still not in CI** and would need a
   self-hosted runner with a cache — that part is unchanged.

   The `build` job doubles as the regression test for item 1. If the NXTCache
   dependency is ever made unconditional again, the contributor build breaks and
   that job is what reports it.

3. **~~No license, no README, no attribution.~~** Settled in the pre-publication
   pass: MIT (`LICENSE`), a root `README.md`, `CONTRIBUTING.md`, and a `NOTICE`
   carrying both the third-party dataset attribution and the Jagex trademark
   disclaimer. GitHub treats an unlicensed public repo as all-rights-reserved,
   so this one blocked reuse outright rather than merely looking untidy.

### On absolute paths

`cacheId` records the cache directory's **basename only**, permanently and not
just while the repo is private — a file written under one policy outlives the era
it was written in. The same rule now applies to tracked text.

An earlier revision of this section claimed that nothing in `src`, `docs`,
`datasets`, `cmake.toml` or `CONTEXT.md` contained an absolute path, a drive
letter, or a username. **That was not true when it was written.** A
pre-publication audit found four in `docs/adr/0002` and one in
`src/cli/WallShapeTests.h`, all naming the maintainer's local drive layout or the
prior in-house nav stack. They are now repo-relative paths or prose. The claim
holds because it was checked, not because it was asserted.

Three drive-letter strings remain on purpose and should stay:
`C:\ProgramData\Jagex\RuneScape` (where the NXT cache actually lives), the
`C:\Program Files\Git\...` probes in `bake.ps1`, and the `C:\Users\<name>`
mentions in `Provenance.h` / `build/main.cpp` — comments explaining *why* the
username is stripped.

The same audit found no credentials, tokens, keys, internal hostnames or IP
addresses, in the working tree or in any of the 54 commits of history.
