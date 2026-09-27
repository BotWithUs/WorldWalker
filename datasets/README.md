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
| `dialog_zones.json`    | build time   | `wwbuild build` → baked as the DialogZones section; see below |

## Dialog zones

`dialog_zones.json` is an array of boxes where walking can raise a
conversation that asks a question: `{name, min_x, min_y, max_x, max_y, plane
(or plane_min + plane_max), answers: [text, ...]}`. While walking or waiting
to land, the executor continues any plain chat page anywhere; the option list
(1188) it answers **only** inside a zone, picking the first option whose text
contains an answer, one answer per try in the listed order. Each answer is
1..36 bytes, the size of the nine int slots it travels to the host in.

A zone rather than a field on a link, because the ground is usually open: the
Citharede Abbey road is one area on both sides of where the hunter stops you,
so a link across it would be an intra-area edge the bake drops.

Picking by text is done by the host (the `DialogueAnswer` chain step, kind 5),
which needs the framework bridge to support it.

## Gates and routes

Every entry may carry a `requirements` object. `skill` is one `{id, level}`;
`items` is an array of `{id, count}` and passes when **any** of them is held on
an item teleport; `varbit` and `varp` are either one `{id, value}` or an
**array** of them, and each is an exact-value match; `varbit_at_least` and
`varp_at_least` take the same two spellings and pass at or above `value`
(which defaults to 1). The array spelling is how one entry demands more than
one var — an unlock *and* a setting, or the unlocks at both ends of a route —
without the loader having to know what those vars mean.

**A varp gate is denied by the live bot today.** The executor reads every
varbit a requirement names on each plan, but it only learns varps from the
host's `readCapability`, and the Java bridge
(`WorldWalkerCallbackBridge.readCapability`) sends skill levels and nothing
else, so every varp reads 0. That is the safe direction: a varp-gated row is
never planned live until the host supplies the varp. Use a varbit when one
carries the same fact; use `varp_at_least` for quests the game keeps only in a
varp (Tree Gnome Village, Cabin Fever, Regicide), and have the host add those
varps to its capability snapshot to admit them.

A `lodestones` destination may also carry `routes`: an array of
`{requirements?, chain}` pairs, each an alternative way to reach that same
destination. A route inherits the destination's identity and unlock gate and
ANDs its own gates on top. The shipped dataset gives every lodestone one route —
the cast from the Magic ability book (`1461:1`, sub = the spell's `param 2793`
slot), gated on the book's lodestone filter varbit `50990` being `0`.

The config-built lodestone-map chain is **always** emitted alongside the routes
and is deliberately left ungated. It is the fallback for any player the routes
do not describe, and a route that cannot complete must never be a destination's
only candidate — the executor would fail it and re-plan onto the same edge
forever.

## NPC origins and disabled rows

A `transport_links` row normally names the loc it clicks (`object_id`), and the
host finds that loc within one tile of the row's `x, y`. A row whose origin is
an NPC instead carries `npc: {first_id, last_id?, radius?}` and no `object_id`:
the loader opens its chain with a `ClickNpc` step (kind 6), which the host
resolves to the nearest NPC whose type id is in `[first_id, last_id]` within
`radius` (default 8) of the row's tile, clicked with the row's `option_index`
(0-based). The rest of the chain is the usual `wait_interface` / `click` steps.
If no NPC is found, or its interface never opens, the executor routes around
the row as it does around a missing loc. This needs a host that implements
`ClickNpc`; one that predates it rejects the kind and fails the walk loudly.

`disabled: true` keeps a row in the file but out of the bake. A row that cannot
be executed yet is worse than no row, because the planner routes through it.

The charter ship rows are disabled. Every port but Catherby named object_id 0,
since you charter by talking to a Trader Crewmember, and none of them had a
chain to pick the destination, so no charter could complete. What is known:

- NPCs: Trader Stan 4650 and Trader Crewmember 4651..4656 all have `Charter` as
  option 0 (cache NPC defs). Which of those stands at which port is server-side,
  so a row should take the whole range: `"npc": {"first_id": 4650, "last_id": 4656}`.
- Interface: 95, SAILING_TRANSPORT_WORLD_MAP. Per port it has a marker
  (components 1..11, Menaphos 36), a name (12..22, Menaphos 37) and a GO_
  component (23..34). Which of these takes the click that sails, and whether a
  confirmation follows, needs a live read; the cache decode of its ops is not
  trustworthy.

To enable a port: add the `npc` block, a chain of `{"wait_interface": 95}`, the
verified `{"click": [95, <component>, <option>, -1]}` (and any confirmation), a
closing `{"wait": N}`, then drop `disabled`.

## Spirit trees

Each of the 11 trees has a row to each of the other 10. A row clicks the
tree's `Teleport` op (option_index 1; op 0 is `Talk to`), waits for
SPIRITTREE2 (interface 1145), clicks the destination with op 1, and waits 5
ticks. Sources:

- **Locs and anchors:** `nxtcache-dumper --type locspawn` over the live cache.
  The row origin is the loc's anchor, because the host looks for the loc within
  one tile of it. Two rows used to name flowers (1189 Daisies, 1202 Jungle
  flower) and seven named 0.
- **Destinations:** enum 2536, the coordinate per destination index that
  script9765 compares with `spirittree_coord` (varp 2662). The names come from
  enum 2535.
- **Components:** enum 8839 maps index to component, and script9760 gives an
  unlocked one `IF_SETOP(1, "Teleport")`. The component names come from
  gameval `component.json`.
- **Unlocks:** script9765 gates the patch trees and the quest trees. The base
  requirement is Tree Gnome Village, whose reward text reads "Access to spirit
  tree teleportation" (quest 267 in `quests.json`: varp 2661 `treequest`,
  1..9).

| idx | Tree | Loc @ anchor | Comp | Lands | Unlock beyond Tree Gnome Village |
|---|---|---|---|---|---|
| 0 | Tree Gnome Village | 68974 @ 2543,3168 | 0 | 2542,3169 | none |
| 1 | Gnome Stronghold | 68973 @ 2460,3447 | 13 | 2462,3444 | The Grand Tree, varp 2740 >= 160 (*) |
| 2 | Battlefield of Khazard | 1317 @ 2553,3257 | 15 | 2557,3259 | none |
| 3 | Grand Exchange | 1317 @ 3186,3509 | 17 | 3187,3507 | none |
| 4 | Mobilising Armies (enum: Warforge) | 1317 @ 2417,2848 | 19 | 2416,2851 | none |
| 5 | Port Sarim patch | 8338 @ 3059,3257 | 21 | 3058,3257 | varbit 64 == 20 (fully grown) |
| 6 | Etceteria patch | 8382 @ 2612,3857 | 23 | 2613,3855 | varbit 66 == 20 |
| 7 | Brimhaven patch | 8383 @ 2801,3202 | 25 | 2800,3203 | varbit 68 == 20 |
| 8 | Poison Waste | 26723 @ 2338,3110 | 27 | 2338,3109 | varbit 10479 >= 3 |
| 9 | Prifddinas | 92994 @ 2267,3368 p1 | 29 | 2275,3371 p1 | varbit 25045 == 1 |
| 10 | Manor Farm patch | 114572 @ 2658,3382 | 34 | 2661,3383 | varbit 45329 == 20 |

Every row requires `varp_at_least 2661 >= 9` plus the unlocks of both its
trees. The patch value 20 is the stage that morphs to 8355
`SPIRIT_TREE_FULLYGROWN`, the only stage with a Teleport op.

(*) The client does not check The Grand Tree for the Stronghold tree. The gate
is a conservative guess, not a fact from the data.

Index 11 (your house, 2940,3223, varbit 61257) has no rows.

Because Tree Gnome Village lives in a varp, the live bot plans no spirit tree
until the host supplies varp 2661 (see "Gates and routes"). No varbit carries
it: `latest_varbits.json` has no player varbit on varp 2661, and quest
achievement 120 is derived from the quest itself.

## Gnome gliders

The 16 glider rows that name a loc are disabled. Loc 187 (Gnome glider) and
5814 (Landing light) have no ops in the cache, since you fly a glider by
talking to its gnome pilot, so they could never execute. The other 48 rows
name 0. To bring gliders back, give them an `npc` origin and a GLIDERMAP
(interface 138) chain, gated as script10747 gates the buttons:

- ogre: varp 2671 >= 200 (One Small Favour)
- tgv: varbit 9547 >= 120
- elfcity: varbit 25043 == 1 and varbit 23198 >= 400
- elr1: varbit 33889 >= 1

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
