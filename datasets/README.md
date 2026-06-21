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

- **Offline bake:** `wwbuild build <cache_dir> <this_dir> <out.wwa>` ingests all
  of the above and bakes the transition graph into the artifact.
- **Runtime:** the planner loads the *global* teleports (spell + lodestone/item)
  from this directory at artifact-open time, so they're editable without
  re-baking. `transport_links` / `teleport_chains` are baked-only and not read at
  runtime.

## Provenance caveat

`transport_links.json` was consolidated from `build/doors_dataset/` and the
teleport files from `build/teleports_dataset/` — these were separate test bakes
and may not be the exact same export vintage. For a guaranteed-consistent set,
re-export all four from the authoritative `nav_data` source into this directory.
