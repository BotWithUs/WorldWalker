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

Two things block the public workflow, both bigger than any one bake:

1. **WorldWalker cannot be built without a private sibling.** `cmake.toml` imports
   `NXTCache.{lib,dll}` from `..\NXTCacheLibrary`, and that repo is private. An
   outside contributor cannot configure the project, let alone bake. Options:
   publish NXTCacheLibrary, ship prebuilt `NXTCache.dll` + headers as a release
   asset, or keep `wwbuild` internal and have a maintainer bake contributed
   dataset changes.
2. **There is no CI.** Nothing validates a PR automatically. A bake needs a cache
   and network access, so CI would have to be self-hosted, or limited to JSON
   schema and lint checks with the bake done by a maintainer.

One thing that is already right: nothing in `src`, `docs`, `datasets`,
`cmake.toml` or `CONTEXT.md` contains an absolute path, a drive letter, or a
username. `cacheId` records the cache directory's **basename only**, permanently
and not just while the repo is private — a file written under one policy outlives
the era it was written in.
