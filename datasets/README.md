# WorldWalker datasets (canonical, version-controlled)

This directory is the **single source of truth** for the externally-curated
*Transition* datasets WorldWalker depends on. Per `docs/adr/0003`, collision is
derived fresh from the RS3 cache every build, but **transitions** (ladders,
stairs, doors, fairy rings, boats, npc/object teleports, spell/item/lodestone
teleports) are not in the cache — they come from these curated files (originally
the Gibson transport data / `nav_data` export).

Previously these lived only in gitignored `build/` subdirectories
(`build/teleports_dataset/`, `build/doors_dataset/`, `build/dung_only/`), so they
vanished on a clean and weren't tracked. They now live here, in the source tree.

## Files

| File                   | Used at      | Consumed by                                            |
|------------------------|--------------|--------------------------------------------------------|
| `transport_links.json` | build time   | `wwbuild build` → baked into the `.wwa` as point-to-point transitions |
| `spell_teleports.json` | build + run  | baked by `wwbuild`; global-origin subset also loaded **at runtime** by `worldwalker.dll` (`loadGlobalTeleportsInto`) |
| `item_teleports.json`  | build + run  | same as above (`lodestones` + generic item `teleports`) |
| `teleport_chains.json` | build time   | *(optional, currently absent)* — the loader skips it if missing |

## How they're consumed

- **Offline bake:** `.\scripts\bake.ps1` — one command, from tracked inputs, on a
  machine that has never run this. It wraps
  `wwbuild build <cache_dir> <this_dir> <out.wwa> --live`, which ingests all of the
  above and bakes the transition graph into the artifact. **See
  `docs/artifact-delivery.md`** for where the cache comes from, what the bake
  records about itself, which `.wwa` is canonical, and how to get a dataset change
  reviewed and shipped.
- **Runtime:** the planner loads the *global* teleports (spell + lodestone/item)
  from this directory at artifact-open time, so they're editable without
  re-baking. `transport_links` / `teleport_chains` are baked-only and not read at
  runtime.

## Changing a dataset

Edit the JSON, re-bake to a scratch path, run the acceptance gate, and open a PR
carrying **the JSON diff plus the regenerated sidecar** — a reviewer reads two text
diffs and never opens the 10 MB binary:

```powershell
.\scripts\bake.ps1 -Out build\scratch.wwa
.\build\Release\wwcli.exe check build\scratch.wwa datasets
```

A datasets-only change should move `transitions`, `edges` and `datasetHash` in the
sidecar while leaving `squares` and `collisionFingerprint` **identical**. A moved
`collisionFingerprint` means the bake used different game data, which is visible at
a glance and is usually not what the author intended.

## Provenance caveat

`transport_links.json` was consolidated from `build/doors_dataset/` and the
teleport files from `build/teleports_dataset/` — these were separate test bakes
and may not be the exact same export vintage. For a guaranteed-consistent set,
re-export all four from the authoritative `nav_data` source into this directory.

## Attribution

These files are **not original to WorldWalker**. They derive from the
community-maintained "Gibson" transport data and the `nav_data` export, since
reformatted, consolidated, and extended for WorldWalker's schema. Credit for the
original survey work belongs to those upstream maintainers.

The MIT license in `LICENSE` covers WorldWalker's own source. It is not a claim
of authorship over the upstream data these files descend from — see `NOTICE`. If
you are an upstream maintainer and want the attribution worded differently, or
the files removed, please open an issue.

What is in here is factual game data: tile coordinates, interface and varbit
ids, tick costs. No RuneScape game assets, no cache contents, no keys.

### What a provenance check turned up (2026-09-15)

Recorded so nobody repeats the search. The upstream terms for these files could
**not** be established:

- The files carry no licence, author, or version field of their own.
- Nothing on disk preserves the original export — the pre-`datasets/` copies in
  `build/doors_dataset/` and `build/teleports_dataset/` are byte-identical to
  these, with no accompanying licence or README.
- The repository history begins at the consolidation commit. There is no import
  commit recording where the files came from.
- Public search does not identify a "Gibson" RS3 transport dataset as a
  published, licensed project.

So the attribution above is written from the ADR and the commit message, which
are the only records that exist. **Treat the upstream terms as unknown, not as
permissive.** The MIT licence in `LICENSE` is WorldWalker's grant over its own
source; do not read it as a licence over these files.

Worth knowing for calibration: the nearest comparable public dataset,
[`Psyrcuit/rs3-pathfinder-data`](https://github.com/Psyrcuit/rs3-pathfinder-data),
ships under **CC BY-NC-SA** — non-commercial and share-alike, which would not be
compatible with an MIT repo. That says nothing about this data's terms, but it
does show the licence question is a real one in this space rather than a
formality.

If you know the actual origin, please record it here and replace this section.
