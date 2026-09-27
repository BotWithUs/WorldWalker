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
Say why in `disabled_reason`, free text the loader ignores, so the next person
knows what would bring the row back.

A row may also carry `extra_cost`, an integer from 0 to 1000 added to the
transition's cost in the planner's units, where a tile of walking costs about
1. It is for what a transition costs the player that no wait expresses, such
as a fare. A `wait` would tell the planner the same thing but make the bot
actually stand still for it.

## Op check

A row's `option_index` must name an option its origin has: for a loc row, an
option of `object_id` or of a loc it morphs into; for an `npc` row, an option
of an NPC in `[first_id, last_id]` or of one it morphs into. A row that fails
clicks nothing, and the planner keeps choosing it anyway. Members-only options
count (`members_action_N` on a loc, `members_actions` on an NPC).

`wwbuild` checks every enabled transport row against the loc and NPC
definitions in a checkout of rs3-cs2-dumps (`locations.json`, `npcs.json`),
which `bake.ps1` finds at `..\rs3-cs2-dumps` or takes from `-OpDefs`:

- `wwbuild build ... --op-defs <dir>` drops the failing rows and lists them by
  family. Without `--op-defs` the check is skipped and the bake says so.
- `--strict-ops` (`bake.ps1 -StrictOps`) fails the bake instead.
- `wwbuild opcheck <dataset_dir> <defs_dir>` runs the check alone in about a
  second, with no cache, and exits 1 on any failing row. Run it on every
  change to `transport_links.json`.

An id the export does not know is kept and counted as unverified: an export
older than the cache is no evidence that the option is missing. A row naming
`object_id` 0 is not checked; the bake drops it as naming no loc.

The definitions come from the export and not from the cache the bake reads,
because NXTCacheLibrary's decoder currently loses every loc and NPC morph
table (and decodes some locs, such as ladder 5492, to defaults), and 89
enabled rows are valid only through a morph: 50 of them are spirit-tree
patches, which gain `Teleport` only once grown.

### Rows that named the wrong loc (2026-09-27)

When the op check first ran, 231 enabled rows failed it. Most named decorative
scenery beside the real crossing (a crate, tulips, a barrel), a few named the
right loc with the wrong option. They were repaired from the cache, not from
memory:

- **Option only.** Where the row's own loc has an option whose text is the
  row's `name` ("Inspect", "Bottom floor", "Travel Imperial District"), the
  row takes that option.
- **Wrong loc.** Otherwise the row takes the loc that `nxtcache-dumper --type
  locspawn` places within 4 tiles of its origin with a crossing option
  (Enter, Climb-down, Cross, Squeeze-through, Jump to, ...) that fits the
  row's direction. Where two fit, the one toward `dest` was picked by hand.
- **Origin.** The host finds the loc within one tile of the row's `x, y`,
  measured to the loc's anchor, and the planner needs a standable tile within
  one tile of the origin. A repaired row's origin is moved as little as
  possible to satisfy both.
- **Disabled.** Rows with no such loc nearby are disabled with a
  `disabled_reason`: crossings that start at an NPC or an item, the level-27
  Wilderness obelisk (loc 65625 has no options), and three Shifting tombs rows
  whose 3x5 loc has no standable tile near its anchor.

## Origin within a tile of the anchor

The host finds a row's loc by id on the row's plane within one tile
(Chebyshev) of the row's `x, y`, measured to the loc's own tile
(`WorldWalkerCallbackBridge.resolveLocTile`). For a multi-tile loc that is its
anchor, the south-west corner the map places it by, not its middle. A row
two or more tiles from the anchor clicks nothing: the executor fails the
transition and drops every row on that loc for the rest of the walk. Doric's
Task II failed that way on the Burthorpe Castle stairs (origin 2901,3564,
steps 66970 anchored at 2901,3561).

The planner has the opposite need: a standable tile within one tile of the
origin, in the area the player approaches from. A row satisfies both when its
origin is on the anchor or beside it and one of the origin's neighbours is
floor on the right side.

Audited on 2026-09-27 against a locspawn scan of every loc with an option
(`nxtcache-dumper --type locspawn`, 108k placements):

- **Before:** 1,497 enabled loc rows, 144 with the origin 2 to 4 tiles from the
  nearest anchor of their loc, 26 further.
- **Moved:** 117 rows. Each origin moved to the tile within one of the anchor
  whose standable neighbours are in the same area as the old origin's, the
  nearest such tile to the old origin; destinations are unchanged. On a
  vertical row the area is the one beneath the landing, the one the bake pins
  the approach to. The spill areas that fill the empty sky on planes 1 to 3
  (areas that span the whole map) do not count as a side. Five stair rows
  whose old origin touched both a room and the ground outside it were placed
  by hand, on the room's side: Rimmington (71903), Draynor Manor (47657),
  Draynor (2347), Lumbridge (45483) and Port Sarim (40059).
- **Verified:** for every moved row, `wwcli path` from a tile beside the new
  origin to its destination plans through that row, or through another row on
  the same loc that also passes this check.
- **Bridges:** the scan reports a loc on the plane it is stored on. On a
  bridge tile that is one above the plane it stands on (the Taverley mill's
  stairs 66637 are stored on plane 1 at 2890,3426 and are its ground floor),
  so a row with no placement on its own plane is compared with the plane
  above.
- **Left:** 28 rows 2 to 4 tiles off and 26 further. For most of the 28 no
  tile beside the anchor touches the approach side (among them 11 of the
  Isafdar forest obstacles, locs 3921 to 3924, and the Ardougne house stairs
  34498, whose only tiles beside the anchor are outside the house), so only a
  host that measures to the loc's footprint rather than its anchor can click
  them. The 26 further ones name a loc placed elsewhere or a different loc id
  and need the repair described in the section above.

## Quest floors and entrances (2026-09-27)

Every quest walk target on an upper floor or underground was planned from the
nearest lodestone with `wwcli path --ungated`. The rows added for the ones
with no route fall into three kinds, and the first two are gaps in what the
bake derives from the cache:

- **Doors the bake skips.** NXTCacheLibrary makes a wall a door only when its
  loc has `interactType > 0` (`MapSquare.cpp`), and some doors with an `Open`
  option carry none: 2546 and 2548 (Ardougne castle, Bravek's house), 34825,
  21814 (Tower of Life), 25638 (Camelot), 31808 (the Clock Tower dungeon),
  5183, 5186 and 5172 (Fenkenstrain's castle). Each crossing has a row per
  direction with the door's own shape and rotation. Fixing the library would
  derive these and probably others.
- **Stairs anchored on different tiles.** The vertical deriver pairs a loc on
  plane p with one on p + 1 only when both stand on the same tile, so a
  staircase whose upper half is anchored a tile away has no link. Where a
  single-tile upper stair has a `forceapproach`, its landing is on the open
  side that gives.
- **Entrances with no row.** Cave mouths, trapdoors and ladders into quest
  dungeons (Goblin Cave, Hazeel's cave, the Elemental Workshop, the
  Asgarnian Ice Dungeon, Melzar's Maze, the Experiment Cave, Paterdomus).

Gates added from the quests' own vars: the Elemental Workshop stairs on
varbit 13254 >= 1 (the book read), the Monk's Friend ladder on varbit 11094
== 1 (the multiloc's visibility bit), the trapdoor under the Grand Tree on
varp 2740 >= 140 (The Grand Tree's tunnel stage), Fenkenstrain's tower door
on varbit 12891 == 1.

Disabled: 2224, which clicked the Fenkenstrain grave (5168, op 0 `Read`) and
not the Memorial beside it (5167, `Push`) that the new row uses; 95 and 96,
the Paterdomus statue jump, whose loc 102085 is anchored 5 tiles from both
origins with no floor within 3 of the anchor.

**Not verified offline:** every landing tile (each is a standable tile beside
the partner loc, not a capture); whether the Tower of Life door or the Tower
guard at the Watchtower stop a player before their quest; the warning a
player may get at the Entrana dungeon ladder (2408).

Left without a route, with the reason: the Underground Pass, the Ape Atoll
dungeon, the Dig Site caves, the stomach in Song from the Depths and the
low-level Runespan (quest mechanics, instances, or a portal the quest clicks
itself); Ashdale and the Death Plateau cave (reached only through gated
rows); the Phoenix Gang chest, Alomone's chamber, Gu'Tanoth, Ana's passage
and the Fenkenstrain mausoleum (quest doors, rafts, carts and keys); One
Small Favour's tile at 2623,9834, which is solid rock; and the Entrana
dungeon from Taverley (the Port Sarim boat to Entrana is a row with
`object_id` 0).

## Dig sites map

The eight `View` rows click loc 116436 (`ARCH_PLANNING_MAP`, "Dig sites map"),
whose only option is op 0 `View`, from 3328,3374 (the loc's anchor is
3327,3373). That opens interface 667 `ARCH_SITE_MAP`, and the chain clicks the
site's icon: `[667, 11, 1, <sub>]`, then waits 5 ticks.

- **Icons.** Script 14794 builds one child of 667:11
  (`ICON_CONTROL_LAYER`) per row of enum 14057 (dbtable 86), so the sub is
  the enum index. Each gets op 1 `Fast travel` and op 2 `Info`; the client
  handles only op 2 (a local popup), so op 1 is the server-side teleport.
- **Gates.** Table 86 holds each site's Archaeology level and qualification;
  the qualification is varbit 46468 (`ARCH_QUALIFICATION`, 2 = Assistant,
  3 = Associate). Sites without one require the tutorial, varbit 46463 >= 100.
  The map greys nothing out: every lock is the server's.

| sub | Site | Level | Gate | Lands |
|---|---|---|---|---|
| 0 | Kharid-et | 5 | 46463 >= 100 | 3345,3194 |
| 1 | Everlight | 42 | 46463 >= 100 | 3697,3206 |
| 2 | Infernal Source | 20 | 46463 >= 100 | 3271,3504 |
| 3 | Stormguard Citadel | 70 | 46468 >= 2 | 2680,3403 |
| 4 | Warforge | 76 | 46468 >= 2 | 2409,2824 |
| 5 | Orthen | 90 | 46468 >= 3 | 5457,2339 |
| 6 | Senntisten | 60 | 46463 >= 100 | 1784,1296 |
| 7 | Daemonheim | 73 | 46463 >= 100 | 3428,3699 |

The Orthen and Senntisten rows had each other's gates (Orthen at level 52,
Senntisten at 90 with Associate); the landing tiles were already right, so
the gates moved. Moonrise (sub 8, level 52) has no row: its landing tile is
not in the data.

**Not verified offline:** that `View` opens 667 (it is the only dig-site map
interface), whether the server confirms the teleport, and the landing tiles
(each sits in its site, beside that site's entrance locs). Senntisten's
plane is doubtful: the site's locs dump on planes 1 and 2, the row lands on 0.


## Balloons

Each of the six stations has a 2x2 basket loc that morphs, on the station's
unlock varbit, from rock or frame into loc 19129 `Basket` (op 0 `Fly`;
Entrana's is 19128, op 0 `Use`). A row clicks its basket's op 0, waits for
interface 469 `ZEP_BALLOON_MAP`, clicks the destination's button with op 1
(each button has one op, its place name, read from the raw cache group; the
decoded interface reads text as ops here, as it does for 95), and waits 6
ticks. No clientscript touches 469, so any locking is the server's.

| Station | Basket (anchor) | Button | Unlock gate |
|---|---|---|---|
| Castle Wars | 19137 (2463,3109) | 15 | varbit 12532 == 1 |
| Grand Tree | 19139 (2481,3457) | 16 | varbit 12533 == 1 |
| Crafting Guild | 19141 (2924,3301) | 17 | varbit 12534 == 1 |
| Entrana | 19133 (2806,3355) | 18 | varbit 12529 >= 91 |
| Taverley | 19135 (2928,3411) | 19 | varbit 12529 >= 200 |
| Varrock | 19143 (3296,3481) | 20 | varbit 12535 == 1 |

Varbit 12529 is Enlightened Journey (quest 315, complete at 200); the per-
route gates are the values achievement 2119 "Around the World in Six Ways"
and clientscript 13281 test. A row carries the gates of both its stations.
The Varrock origin moved from 3298,3482 to 3297,3481, within a tile of its
basket.

**Not in any gate:** the log each flight burns. Operators store logs as
charges (varbit 43747 `ZEP_CHARGES`, NPC ops `Store logs` / `Check
charges`), so a player may fly holding none, and an `items` gate would turn
them away. Entrana's weapon and armour ban is not expressible either.

**Not verified offline:** a confirm step after the button, the flight's
length, and the landing tiles (the dataset's, each within 4 tiles of the
station's balloon).


## Magic carpets

A carpet row starts at the station's Rug merchant (`npc`, op 2 `Travel`),
waits for interface 1928 `MAGIC_CARPET`, clicks the destination's
`*_BUTTON_ACTIVE_LAYER` with op 1, and waits 6 ticks. Each button has one op,
`Select`, in the raw cache group (the decoded interface misreads it), and
clientscript 2403 binds op 1 to exactly these nine; it is the only script that
touches 1928, so any locking is the server's. The fare is coins (varp 7638
`magic_carpet_cost`); its amount is not in the data, so the rows carry no
`extra_cost`.

| Station (row origin) | Button | Gate |
|---|---|---|
| Shantay Pass (3306,3109) | 28 | none |
| North Pollnivneach (3348,3000) | 60 | none |
| South Pollnivneach (3353,2940) | 68 | none |
| Nardah (3402,2918) | 76 | none |
| Bedabin Camp (3181,3048) | 44 | none |
| Uzer (3466,3112) | 52 | The Golem, varbit 13639 >= 10 |
| Monkey Colony (3229,2988) | 36 | Do No Evil, varbit 9324 >= 315 |
| Menaphos (3244,2820) | 84 | The Jack of Spades, varbit 36140 >= 100 |
| Sophanem (3322,2823) | 92 | Icthlarin's Little Helper, varbit 10987 >= 26 |

Gates are from quest reward text ("Magic carpet route to the ruins of Uzer",
"New magic carpet station at the monkey colony", "Access to the city of
Sophanem", "Access to Menaphos") and apply at both ends.

**NPCs.** Rug merchants 2291..2300 all list `Travel` at op 2, directly or
through a morph: 2295 becomes 2296 on varbit 13639 (The Golem, so Uzer's),
2297 and 2299 become 2298 and 2300 on varbit 10979. Eight stations take the
whole range. The Monkey Colony takes 13238, spawned at 3226,2984. Merchant
3020 is outside the range and could not be placed; widening to it would
take in some 340 unrelated NPCs.

**Not verified offline:** which merchant stands at each station other than
the Monkey Colony, whether the server asks for a fare confirmation, the
flight's length, and the landing tiles (the dataset's).


## Charter ships

A charter row starts at a Trader Crewmember (`npc`, op 0 `Charter`). Its chain
waits for SAILING_TRANSPORT_WORLD_MAP (interface 95), clicks the destination's
GO_ layer with op 1, then waits 6 ticks. Each row carries `extra_cost: 80`:
fares run from hundreds to a few thousand coins, and 80 keeps a free lodestone
ahead even from the charter dock. From Port Sarim's dock, Catherby is 89 by
charter and 73 by lodestone.

**Interface 95.** Component names come from gameval `component.json`. The ops
were read from the raw cache group (`js5-3.jcache`, group 95); the decoded
interface dump reads text as ops and is not reliable for this interface.

- Each `GO_<port>` layer carries one op, `Ok`, and no script beyond a hover
  model swap (script67). Its marker (a model) and name (text) sit inside it and
  have no ops of their own, so the click belongs to the GO_ layer:
  `[95, <GO>, 1, -1]`.
- No clientscript builds or reads interface 95, so it has no client-side lock.
- Comp 35 is `CLOSE_BUTTON`.

| Port | Marker | Name | GO_ (click) | Crew NPC (spawn) | Gate at either end |
|---|---|---|---|---|---|
| Port Tyras | 1 | 12 | 23 | 4654 (2145,3122) | Regicide: varp 2102 >= 15 |
| Port Phasmatys | 2 | 13 | 24 | 4652 (3701,3503) | **disabled**: lock unknown |
| Catherby | 3 | 14 | 25 | 4656 (2794,3407) | none |
| Shipyard | 4 | 15 | 26 | 4654 (3001,3034) | **disabled**: lock unknown |
| Karamja (Musa Point) | 5 | 16 | 27 | 22692 (2954,3156) | none |
| Brimhaven | 6 | 17 | 28 | 4651 (2760,3239) | none |
| Port Khazard | 7 | 18 | 29 | 4654 (2675,3144) | none |
| Port Sarim | 8 | 19 | 30 | 4653 (3042,3190), Stan 4650 (3033,3190) | none |
| Mos Le'Harmless | 9 | 20 | 31 | 4655 (3672,2930) | Cabin Fever: varp 2326 >= 140 |
| Crandor | 10 | 21 | 32 | - | no rows |
| Oo'glog | 11 | 22 | 33 | 7065 (2621,2857) | As a First Resort: varbit 14042 == 1 |
| Menaphos | 36 | 37 | 34 | 24731 (3143,2662) | The Jack of Spades: varbit 36140 >= 100 |

**NPCs.** From `npcs.json` in the cs2 dump:

- Trader Stan 4650 and Trader Crewmember 4651..4656 and 24730..24731 all list
  `Charter` at op 0. 22697 (Talk to only) is not a charter NPC.
- Oo'glog's crew is multinpc 7065, `ids [-1, 4654, 0]` on varbit 14042
  (`afr_complete`), so it only exists once As a First Resort is complete.
- Musa Point's crew is multinpc 22692, `ids [4655, 0]` on varbit 31276.
- Spawns come from `rs3-cache-toolkit/data/npc_spawns.toml`. Every row's
  origin is within 3 tiles of its crew.

Rows take the whole 4650..4656 range at the seven ordinary ports. Oo'glog and
Musa Point take their multinpc id, on the strength of the framework's
`Npc.typeId()` being the base type id. If the host reports the morphed id
(4654 / 4655) instead, those two ports find no NPC and are routed around.

**Gates.** From quest reward text in `quests.json`:

- Regicide: "Charter ship access to Port Tyras".
- Cabin Fever: "Access to Mos Le'Harmless". Also struct text: "You can take a
  charter ship to Mos Le'Harmless from Port Sarim's docks after completing
  Cabin Fever".
- As a First Resort: "Ability to travel to Oo'glog using charter ships".
- The Jack of Spades: "Access to Menaphos".

A gate applies at both ends of a trip. Cabin Fever and Regicide live only in
varps, so the live bot takes neither port until the host supplies those
varps. Nothing in the data says what locks Port Phasmatys or the Shipyard, so
every row touching them stays disabled.

**Not verified offline.** Three things need a live read:

- **Confirm step.** Whether a fare confirmation follows the click. Varbit
  36902 `sailing_dontaskagain` exists and no clientscript reads it, which
  suggests the server asks. As a hedge, `dialog_zones.json` gives each
  enabled port's dock a zone that answers an option list containing `Yes`.
  If the prompt is not an option list, or says something else, the charter
  lands off course and is routed around.
- **Voyage length.** The executor waits 6 ticks, then up to 8 for the
  landing, and gives up after 3 still ticks.
- **Landing tiles.** They are the dataset's own; neither the cache nor the
  client scripts give them.

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

You fly a glider by its pilot, not the glider: loc 187 (Gnome glider) and 5814
(Landing light) have no options. Every glider row starts at the pilot (`npc`,
radius 8), waits for GLIDERMAP (interface 138), clicks the destination's
`<SITE>_BUTTON_ACTIVE_LAYER` with op 1, and waits 6 ticks.

- **Buttons.** Read from the raw cache group: each ACTIVE_LAYER has exactly
  one op, `Select`, encoded as the charter map's `Ok` is, and
  clientscript 10748 binds op 1 to exactly these. The `<SITE>_GO` / `_BACK`
  components are flight animations with no ops (script10747 plays them).
- **Gates.** script10747 hides a button when its gate fails: ogre varp 2671
  >= 200 (One Small Favour), tgv varbit 9547 >= 120 (The Prisoner of
  Glouphrie), elfcity varbit 25043 == 1 and varbit 23198 >= 400 (Plague's
  End), elr1 varbit 33889 >= 1. A gated station is gated at both ends.
- **The Grand Tree.** Every glider row also needs varp 2740 >= 160: The
  Grand Tree complete (quest 269 in `quests.json`: varp 2740, 5..160). No
  clientscript checks it (the other test in script10747, script20932, reads
  varp 12314), so the lock is the server's. It was missing from every row
  but the Grand Tree pilot's until 2026-09-27, when an account without the
  quest clicked Captain Dalbur's `Glider` at Al Kharid, got no 138, and was
  routed on to walk to the Karamja pilot instead. The host supplies varp
  2740, so the gate is read live.
- **Pilots.** Op indices are 0-based; `Glider` is a members-only option on
  most of them. Morphing pilots are named by their base id, as the charter
  rows name theirs.

| Station (row origin) | Pilot | Glider op | Button | Gate |
|---|---|---|---|---|
| Grand Tree (2465,3501,3) | 3811 (morphs to 17126 at varp 2740 >= 160) | 0 | 40 | none beyond The Grand Tree |
| White Wolf Mountain (2850,3494,1) | 3810 Captain Bleemadge | 0 | 72 | none beyond The Grand Tree |
| Al Kharid (3284,3211,0) | 3809 Captain Dalbur | 0 | 88 | none beyond The Grand Tree |
| Karamja (2971,2969,0) | 3812 Captain Klemfoodle | 0 | 64 | none beyond The Grand Tree |
| Feldip Hills (2548,2969,0) | 1800 Gnormadium Avlafrim | 0 | 56 | varp 2671 >= 200 |
| Tree Gnome Village (2496,3191,0) | 6562 (morphs to 6563 at varbit 9547 >= 120) | 2 | 48 | varbit 9547 >= 120 |
| Prifddinas (2208,3445,1) | 20299 Captain Muggin | 0 | 96 | varbit 25043 == 1, 23198 >= 400 |
| Tuai Leit (1772,11920,0) | 23528 (morphs to 23529 at varbit 33889 == 1) | 0 | 104 | varbit 33889 >= 1 |
| Digsite (lands 3321,3430,0) | - | - | 80 | none; arrival only (its pilot 5249 has no Glider op) |

Pilot spawns come from `rs3-cache-toolkit/data/npc_spawns.toml`; every pilot
is within 8 tiles of its row's origin.

**Not verified offline:** that `Glider` opens 138 without a conversation first
(no clientscript opens it; a dedicated op suggests a direct open, as
`Charter` does), the landing tiles, the flight length, whether the host
reports a morphing pilot by its base id (the charter question again), and the
Crash Island glider (pilot 1407, `Travel`, op 2), which has no row: its
flight plays as an animation on 138 with no click. The host does not supply
varp 2671, so the Feldip end is denied live until it does.

**The Grand Tree's ladder.** Loc 69271 on planes 1 and 2 (2466,3495) lists
`Climb`, `Climb-up`, `Climb-down`; the bake paired it by op 0 `Climb`, which
opens a chooser nothing answers. It has no rows: the bake now derives op 1
up and op 2 down itself (see "Climb direction" below), the same four edges
the hand rows gave, with the same landings.

## Climb direction

NXTCacheLibrary gives a climbable loc one option for both directions, the
first that says `climb`. On a loc listing `Climb`, `Climb-up`, `Climb-down`
that is op 0 `Climb`, which opens an up/down chooser (1188, `CHOICE_V2`) the
executor never answers, so the walk stalls at the ladder or re-plans away
from it. In the artifact live on 2026-09-27, 972 derived transitions over 29
locs clicked it (37212 alone, the Stairs at 4235,3933 and around, had 775).

So with `--op-defs` the bake reads each loc's option text from
`locations.json` and a derived ladder or stair clicks the option that names
its direction: going up, the lowest slot reading `Climb-up`, `Climb up`,
`Go-up` or `Walk-up` (any case, `-`, `_` or space); going down, the `down`
spellings. Only the loc's own options count, not a morph's. A loc with no
such option keeps the cache's. The bake prints the counts as `climb ops:`.

- **Result:** 972 -> 0 transitions click a bare `Climb` on a loc that has
  its direction's option. 974 transitions in the artifact changed option, all
  `Climb` -> the direction's, except two up edges on ladder 34286
  (2667,3694 and 4540,5934, options `Climb-down`, `Climb-up`) that the
  cache had given `Climb-down`.
- **Bare `Climb` alone.** 120 derived transitions over 34 locs click a loc
  whose only option is `Climb`. All but one of their 117 origins are
  derived one way only, so the loc has no other way to offer. The exception is the Anchor 31563 at
  3795,9937 on plane 1, derived both down and up. If it opens a chooser, a
  chain cannot answer it today: `DialogueAnswer` is sent by the executor only
  inside a dialog zone, never baked into a chain, and a zone cannot tell up
  from down. The chooser's text is the server's, so it is not in the dumps.
- **Rows are not touched.** A dataset row names its option. Eight vertical
  rows click an option naming the other direction (Lumbridge Castle
  3229,3214 plane 1 -> 2 clicks 36770 `Climb-down`, the top floor's ladder;
  4627, 1740, 40262, 18833, 6504); they are left for review.

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
