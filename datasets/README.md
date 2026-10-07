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
| `f2p_zones.json`       | build time   | `wwbuild build` → baked as the F2pZones section; see "Free-to-play" below |

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

## Free-to-play

A host that walks a free-to-play account sets `WW_RESTRICT_FREE_TO_PLAY` in the
`disabledMoves` mask (`c_api/worldwalker_c.h`). The planner then refuses every
members-only transition, keeps walks on free-to-play land, and fails a goal off
that land at once. Members accounts never set the bit and plan as before.

**The land is an allowlist.** `f2p_zones.json` lists the boxes of free-to-play
land (`zones`) and the members pockets inside them (`members_holes`), each
`{name, source, min_x, min_y, max_x, max_y, plane | plane_min + plane_max}`,
inclusive. Every tile no zone covers is members land, so land nobody vouched
for keeps a free player out rather than in. The cache has no members map (the
gates are server-side), so every box is drawn from the RS3 wiki and the
loader refuses a box without a `source`. The evidence for each box, and the
borders that are inferred rather than read, are in `docs/f2p-zones.md`. A
change here needs a re-bake: the boxes travel in the artifact.

A transition is members-only when (format::isMembersOnly, in this order):

- its row says `"members": true` (or it is never members when the row says
  `"members": false`);
- it is a charter ship, gnome glider, fairy ring, spirit tree or magic carpet,
  wherever it stands;
- it starts (a local transition) or lands off free-to-play land.

**The `"members"` row flag** may sit on any `transport_links`,
`spell_teleports` or `item_teleports` row and on a lodestone destination (its
routes inherit it). It is a row-level key, so a loader older than it ignores
it; anything but `true` or `false` faults the row like a bad requirement. Use
it for what geography cannot see: a members teleport that lands on free land.

**The teleport flags are generated, not typed.** `tools/f2p/gen_members_flags.py
<item.json> <struct.json>` (NXTCacheLibrary dumper output) rewrites them:

- a spell is members when its ability struct (param 2794 is its name) has
  param 2809 `COMBATV2_ABILITY_IS_MEMBERS` = 1, or any rune it uses is a
  members item (item opcode 16); this is what catches Ancient Magicks landing
  on Ice Mountain and House Teleport landing in Rimmington;
- an item teleport is members when every item that can cast it is a members
  item;
- a lodestone is free when cache enum 12260 lists it, members when 12261 does,
  and otherwise takes `CURATED_LODESTONES` in the script (Fort Forinthry
  members; City of Um and Wendlewick free).

The script writes `true` and never `false` on spells and item teleports, so
their landing still decides through `f2p_zones.json`; lodestones get both. Re-run
it after editing either file or after a game update; `--check` exits 1 if a
file would change.

**The bake checks it.** `wwbuild build` reopens the artifact with the runtime
teleports appended, prints members / total per family and every flag that
overrides geography, and fails (removing the artifact) when a charter row
classifies free, a free lodestone (enum 12260) classifies members or lands off
free land, a members lodestone (12261) classifies free, or another lodestone
has no flag. `wwcli f2p [<artifact> [<datasets>]]` tests the rest.

What it does not do yet: options that are members-only on a loc free players
can otherwise use (`members_action_N`), and a members account on a free world.

## Gates and routes

Every entry may carry a `requirements` object. `skill` is one `{id, level}` or
an array of them (all must pass), `id` being the stat id the host reports
(0 Attack, 2 Strength, 6 Magic, 7 Cooking, 10 Fishing, 12 Crafting,
14 Mining, ...); `items` is an array of `{id, count}` and passes when **any** of them is held on
an item teleport; `varbit` and `varp` are either one `{id, value}` or an
**array** of them, and each is an exact-value match; `varbit_at_least` and
`varp_at_least` take the same two spellings and pass at or above `value`
(which defaults to 1). The array spelling is how one entry demands more than
one var — an unlock *and* a setting, or the unlocks at both ends of a route —
without the loader having to know what those vars mean. `varp_bit` takes the
same two spellings of `{id, bit}` and passes when bit `bit` (0..31, default 0)
of the varp is set, for a varp the game packs as a bitfield with no varbit
over the bit (the key ring's stored keys, varp 2673).

**Gates are ANDed; an OR is two rows.** Outside item teleports, every gate on
an entry must pass. A crossing that opens for either of two things (a key in
the pack, or the same key on the key ring) is one row per alternative. The
bake keeps one transition per (kind, origin, dest), so each alternative row
lands one tile apart, as the Brass key door below does.

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

A destination's `component` is its button on the lodestone map (interface
1092), and those numbers move when Jagex inserts a component: a seasonal hub
slot (`THAS_LODESTONE_MAP__BEACH`, comp 39) pushed Wendlewick from 40 to 41,
and the pick on 40 then hit the Halloween hub, which the server answers with
"This teleport has vanished." After a game update, check every `component`
against the current gameval (`THAS_LODESTONE_MAP__<NAME>`); a real lodestone
button carries two ops, `Teleport` and `Quick Teleport`, a hub slot only one.

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
  5183, 5186 and 5172 (Fenkenstrain's castle), and 34819 and 34822, the
  double front door of the East Ardougne church at 2615-2616,3303, whose
  interior could not be reached at all (live 2026-09-29). Each crossing has a
  row per direction with the door's own shape and rotation. Fixing the
  library would derive these and probably others: the decoder defaults
  `interactType` to 0 and sets it only from opcode 19, where the client
  treats an absent opcode 19 as interactive when the loc has an option. A
  cache scan (2026-09-29) found 1913 placements of 183 wall/door-shape loc
  ids with an `Open` option and `interactType` 0.
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
  rows clicked an option naming the other direction; each was checked
  against the locspawn scan and fixed by hand (below).

### Rows whose option named the other direction (2026-09-27)

| Row (loc, origin -> dest) | Loc at the origin | Now |
|---|---|---|
| 36770 `Climb-down`, 3229,3214,1 -> 3229,3214,2 (Lumbridge Castle) | 36769 (`Climb`, `Climb-up`, `Climb-down`); 36770 is the plane-2 ladder | Removed: the derived 36769 op 1 `Climb-up` edge from 3229,3213,1 is the same crossing |
| 4627 `Climb-up`, 2205,4935,1 -> 2207,4938,0 (Burthorpe games room) | 4627 `Climb-up` leads out to Burthorpe; the room's down stairs are 4620 `Climb-down` at 2207,4935,1 | Removed: the derived 4620 op 0 edge lands in the same area |
| 1740 `Climb down`, 2777,4684,1 -> 2778,4684,2 | 1740 `Climb down`, above the 2x2 staircase 1738 at 2776,4683,0; up is ladder 1750 at 2779,4684 | Lands at 2776,4682,0 (as the same tower at 2518,3430 does); the derived 1750 op 0 edge covers 1 -> 2 |
| 40262 `Climb-up`, 2524,5832,1 -> 2525,5835,0 | 40262 climbs to plane 2 (its own row does); the jump down from this ledge is 40849 `Jump-down` at 2525,5835,1 | Clicks 40849 from its anchor 2525,5835,1, as its twin at 2529,5835 does |
| 4627 `Climb-up`, 2206,4934,1 and 2205,4935,1 -> 2892,3567,0 | 4627, out of the games room to Burthorpe | Unchanged: right loc and option. The dest is a different region, so its plane says nothing about up or down |
| 18833 `Climb-down`, 2812,3668,0 -> 2831,10076,2 (Troll ladder) | 18833 | Unchanged, for the same reason |
| 6504 `Climb-up`, 2913,4953,3 -> 3233,2898,0 | 6504 | Unchanged, for the same reason |

Seen while checking, left as they are: 4622 `Climb-up` at 2206,4936,0 has a
row to Burthorpe (2893,3567,0) though it climbs to the games room's plane
1; 40849 `Jump-down` at 2529,5835,1 has a row up to plane 2; and the 1738
staircase at 2776,4683,0 has no up row (the deriver does not pair it, since
1740 above it is anchored a tile away).

## Tree Gnome Stronghold east stile (2026-09-27)

The two rows on loc 91457 (`GNOME_AREA_IMP_FENCE_STYLE`, "Stile", op 0
`Climb over`, anchored at 2496,3414) cross the Stronghold's east fence
between 2497,3414 and 2495,3414. The loc has no morph and no clientscript
reads it, so whatever lock it has is the server's.

Live on 2026-09-27 an account with neither The Grand Tree nor Tree Gnome
Village was walked from east of the fence (2494,3437) to Blurberry's Bar.
Each of seven tries clicked the stile, got a plain message box (1186
`MESBOX_V2`, continued twice), and stayed east. The executor took that as a
landing, because the far side is two tiles from the click and inside
`kLandingSlack`, so it never excluded the stile: the next walk, to
2484,3444, went STUCK, the re-plan chose the stile again, and the walk
FAILED.

The executor now also judges a local transition's landing by area: if the
player is not in the area of the baked grid that holds the transition's
destination (still in the one they clicked from, say), the transition missed
however near the far side is. It is excluded and re-planned around after one
attempt, the same as a transition that lands off course by distance
(`Executor::hasMissedLanding`). A crossing whose two sides share an area, or
whose tiles the grid cannot place, falls back to which side of the crossing
the player stands on. So a refused crossing no longer depends on its row
being gated to be routed around; the gate below still saves the wasted click.

Both rows now need varp 2661 >= 9 and varp 2740 >= 160, the spirit trees'
gate. That is a guess bounded by the evidence, not a known requirement: the
refusal only proves that an account without either quest is turned away,
and the message text, which would name the requirement, is not in any dump.
The host supplies both varps, so the gate is read live. An account without
them enters by the main gate, loc 68983 (`GNOME_AREA_IMP_GATE`, op 0 `Open`)
at 2459,3383, whose rows 2460,3382 <-> 2461,3385 carry no gate.

**Not verified:** that the stile opens for an account with both quests. If
one is still refused, read the message box's text and gate on what it names.
The other two stiles on the same loc (2368,3425 and 2380,3467) have no rows.

## Shantay Pass (2026-09-27)

Live on 2026-09-27, Boric's Task II walked from 3303,3117 (north of the
pass) to the Agility Pyramid mine at 3323,2875 and failed every time. The
host logged `interact: loc 76546 absent at (3304,3118,0); assuming already
open/removed, skipping`, the next walk south went STUCK, and the re-plan
chose the same row, four times per walk.

**The loc.** 76546 is `DESERT2012_PERIMETER_WALL_SHANTAY_GATE` ("Shantay
Pass", ops `Go-through` / `Look-at`, 2x5), anchored at 3302,3116 with
rotation 1 (locspawn scan). The southbound row's origin, 3304,3118, is two
tiles from that anchor, so the host never found it. Because the row is a
same-floor crossing, the executor took the miss for an open door and walked
on into the wall. The gate has two lanes, x 3303 and x 3305, with a wall
between y 3117 and 3116. Each lane also has a `SHANTAY_PASS_CLICKZONE` (loc
12774, `Go-through`) anchored at 3303,3116 and 3305,3116. The rows keep
76546, the loc the upstream export recorded.

**The rows now:**

| Row | Origin | Dest | Gate |
|---|---|---|---|
| south, into the desert | 3303,3117 (was 3304,3118) | 3304,3115 | none (was: holds a Shantay pass, item 1854) |
| north, out of the desert | 3303,3115 | 3303,3118 (was 3304,3118, a blocked tile) | none |

Both origins are within a tile of the anchor. Since 2026-10-03 the
southbound row clicks the west lane's clickzone, 12774 at 3303,3116, and
confirms the desert warning (below); the northbound row still clicks 76546.

**No pass (2026-10-03).** The southbound row used to need item 1854, the
Shantay pass. RS3 removed the pass and the toll on 26 September 2012 (wiki
update: "Shantay Pass no longer requires a toll or pass ... Members can pass
the gate straight through by walking from north to the south"), so the gate
held every foot route out of the desert's reach: on 2026-10-03 a walk from
Lumbridge (3215,3258) to Crondis failed with "no way into the desert without
a Shantay pass". The row is now ungated. Crossing south is members-only, but
the dataset has no membership gate and the bots play on members' worlds (the
op check counts members-only options for the same reason), so nothing stands
in for it.

**Before the fix.** `wwcli path 3303 3117 0 3323 2875 0 --ungated --item
1854=0` used to climb the Lumbridge house stairs (45481, 3194,3253), cross
the empty sky on plane 1, and come down Pollnivneach's stairs (108803,
3353,2958), cost 752.4. That route was never real; the void fence (see
"Upper-plane void" below) removed it, which left no foot route in at all
until the pass gate went.

**Magic carpets.** The Shantay carpet (3306,3109) is south of the gate, and
every carpet station is in the same area of the baked grid as the desert
around it. The bake used to drop every transition whose two ends share an
area, so no carpet ride was ever planned inside the desert. It now keeps one
whose landing is more than 32 tiles from its origin (`kMinIntraAreaHopTiles`,
131 edges, `intra-kept` in the bake log), and the walk-aware area search
gives each such edge a node of its own, so a route can ride and ride on.
Shorter same-area hops are still dropped. With no teleports,
3303,3117 to the Agility Pyramid mine (3323,2875) now takes the gate and the
Shantay carpet to South Pollnivneach, cost 171.5, where it walked 337.6.

**The executor.** A same-floor crossing whose loc is missing is still skipped
as an open door. If the walk right after the skip stalls with the player on
the side they skipped from, the crossing is now excluded with its loc's rows,
the same as a missing ladder, and the run re-plans around it. Before, it
re-planned onto the same crossing until the stuck budget ran out. Harness
tests 4o and 4p cover this.

**The desert warning (2026-10-03).** Live, the ungated row never crossed.
Every attempt logged two clicks on 76546 three seconds apart and then a
walk to 3304,3115, and the player stood still on 3303,3117 throughout. Going
south raises the Content Warning System's desert page, interface 565
(`CWS_WARNING_10`), and the player waits there until it is confirmed. The
executor confirmed nothing. It re-clicked the gate when the player had not
moved, and its fallback walk closed the page. The script's own click on
12774 fared no better, since the walker clicked again 1.1 s later. The only
crossing on record is a lone script click on 12774 on 2026-10-01, with
nothing else clicked for 76 s afterwards.

The southbound row now clicks 12774, the loc that crossed, and carries a
chain: `wait 2`, then `action [30, 0, -1, 37027842]`. That is a DIALOGUE
click on 565:2 (`CWS_WARNING_10__WARN1`), copied from the confirm V1
intercepted from a real click (`TouristTrapQuest.java`, PathWithUs). It is
sent blind. V1 measured `isOpen(565)` as false while the page was up, so a
`wait_interface` would time out, and a confirm on a closed interface is
dropped by the game, so an account that has turned the warning off (varbit
1143, the 565:5 toggle) loses nothing. When the player is still short of a
crossing, the executor's retry now re-runs the chain after re-clicking, so
the confirm goes out again. The bare re-click it used to make only raised
the warning again. Harness test 4ab covers both cases: a warning that is up
on time, and one whose first confirm arrives too early.

**Not verified live:** that 565:2 is the proceed button (its name is
`WARN1`), and that two ticks are enough for the page to be up before the
confirm is sent. If the walk still stalls, read the open interfaces at
3303,3117 after the click. The northbound row (76546, from 3303,3115) is
unchanged and has not been checked live either.

## Upper-plane void (2026-09-27)

A bake change, not a dataset one, recorded here because it decides which
rows can land anywhere.

**The fault.** NXTCacheLibrary's clip leaves a tile with no floor under it
all-zero, the same word as open ground. On planes 1..3 that is most of the
world, so the area fill joined every upper floor with an open edge into one
area per plane: 17.7 million tiles on plane 1, spanning 6016x7232. Any two
buildings whose upper floors touched it were one walk apart, which is how
the Shantay no-pass route crossed the sky.

**The fence.** wwbuild reads each square's terrain file itself (index 5,
file 3, through `nxt_read_file_raw`) and keeps which tiles are painted (an
overlay or an underlay). Then, on planes 1..3, it floods the standable tiles
with no paint within one tile, the way the area fill joins tiles (walls
respected, across squares). A stretch of more than 4096 such tiles is void
and is blocked; a smaller one is kept. Paint alone would not do: part of the
City of Um's plane-1 street (around 1150..1165, 1826..1832) is unpainted
ground walled in by blocked terrain, and the rock crossings at 3430,4261 and
3434,4261 land a tile off the painted floor. `wwbuild floor <cache> <sqx>
<sqy> <plane>` prints a square's paint; `wwcli areastats <wwa>` lists the
upper-plane areas spanning more than 256 tiles.

| | before | after |
|---|---|---|
| upper-plane areas spanning > 256 tiles | 45 | 11 |
| tiles in them | 91.5 M | 0.27 M |
| largest upper-plane area | 19.4 M tiles | 57,875 tiles |
| transitions | 13668 | 13597 |

The 11 left are painted template regions (whole squares painted on an upper
plane), not joins between places. 71 transitions are gone: 52 landed in the
sky, and 19 start from a plane-1 tile that was only ever joined to the sky
(derived climbs whose top has no painted floor, Stronghold of Security
ladders, and two at Jatizso, 2363,3799). If one of those is a real open-air
platform floored by a loc, it needs a live check and a hand row.

Over the 1854 quest walks (two starts, every quest target), 36 routes
changed and all 36 lost a route that crossed the sky: 17 Ape Atoll targets
reached from the Gnome Stronghold's plane 2, and 3405,4283,1 from Port
Phasmatys. No route got dearer or cheaper.

**The proper fix** belongs in NXTCacheLibrary: `maps::MapSquare`'s
`decodeMapTerrain` already walks this stream and drops the underlay id, so
it could hand over a per-tile floor bit (or set a clip bit for an unfloored
upper-plane tile) and WorldWalker would stop decoding the terrain itself.

## Isafdar forest (2026-09-29)

The forest between Port Tyras and the Tirannwn lodestone (x 2140..2340,
y 3100..3300) is crossed through "Dense forest" strips (3937, 3938, 3939,
3998, 3999, option 0 "Enter") and traps: Tripwire 3921 "Step-over", Sticks
3922 "Pass", Leaves 3924 "Jump". None of these locs carries an interact type,
so the bake derives nothing for them and every crossing is a row here.

**One click crosses one strip.** Strips come in groups of two to four, each
3 tiles thick with a one-tile pocket between them. "Enter" moves the player
through the one strip clicked, 3 tiles. The source of truth is V1's
navigation data, `rs3-nav-api` `src/main/resources/mapdata.zip`
(`areas.dat`, decoded by `pathfinder/data/Area.java`), which has one link
per strip per direction. Its only multi-strip links need the Tirannwn
quiver (33722) worn and are not carried here.

**What was wrong.** 13 "Interactive scenery" rows claimed one strip crossed
two (dest 4 tiles past the loc's far edge, e.g. 3999 at 2187,3163 to
2188,3168). They were the cheapest way in, so the walker took them, landed a
strip short, judged the landing off course and replanned. 9 more rows
clicked a tree (70060, 70063, "Chop down") or an animica rock (113017,
"Mine") beside a trap; each had a correct twin on the trap itself. All 22
are removed.

**The crawl is slow.** In the host log of a live run on 2026-09-29, the
player's tile changed 3.0 to 4.4 s after an "Enter" click and not before. The executor's landing check
(2 settle ticks, then 3 still polls) gave up after 3.0 s, called the
crossing refused, and planned again from the near side while the player was
still crawling; the next click on the same strip then crawled them back.
Every dense-forest row now carries `"chain": [{"wait": 5}]`, so the check
starts 4.2 s after the click. The wait also puts the crawl's real time into
the cost (3 -> 8).

**Rows added** from the same V1 data, each anchor checked against the cache's
placements: both directions of the tripwires at 2251,3168 and 2294,3243 and
the sticks at 2275,3163 and 2257,3227, and the two groups of strips V1 had
and this file lacked, the row at y 3219 (3938, 3939, 3937, 3939 anchored at
2227/2230/2233/2236,3218) and the column at x 2279 (3938, 3937, 3939 anchored
at 2278,3223/3226/3229). Log balances (3931..3933, plane 1) and the pit
climbs out of a failed Leaves jump (3927) are still missing.

**Not verified live:** the 5-tick wait, the traps' timings (no wait added;
their forced moves have not been timed), and the added y 3219 row (V1 walks
to 2238,3219 before the westbound 3939 at 2236,3218; this file's row already
starts there).

## Wendlewick - Amberfell bridge (2026-09-30)

Secrets of Amberfell repairs the bridge east of Wendlewick, the only way on
foot to Amberfell. The two rows click loc 137303
(`WENDLE_AMBERFELL_BRIDGE_CROSS_MULTI`, op 0 `Cross`), anchored at 3653,1589:
3653,1589 -> 3659,1589 eastbound and 3659,1589 -> 3653,1589 westbound.

- **One loc, both ends.** The cache places 137303 once (shape 10, rotation 1,
  stored plane 1 on a bridge column, so plane 0). Its size is 1x7, so turned it
  covers x 3653..3659 on y 1589. There is no separate east-end loc. The host
  matches a row's tile against the whole footprint, so both row tiles sit on
  the loc. 137304 (`..._CROSS_ACTIVE`) is not a placement. It is the morph that
  carries the name and the op.
- **Landings.** Row y 1589 is walkable on the west bank up to x 3653 and on
  the east bank from x 3659. x 3654..3658 is blocked water, so the rows land
  on the loc's two end tiles, which are also its first walkable tiles.
  Agility dbrow 3901 (loc param 6668) is empty in the cache and in the dumps,
  so no dbrow gives a landing tile.
- **Gate.** 137303 morphs on varbit 60919 (`WENDLE_AMBERFELL_BRIDGE_COMPLETE`,
  varp 12867 bit 19, so only 0 or 1): 0 -> nothing, 1 -> 137304 "Bridge"
  [Cross] (rs3-cs2-dumps `locations.json` `morphs_1`; the NXTCacheLibrary
  decoder drops morphs). The broken bridge has no Cross loc at all. So
  `varbit_at_least 60919 >= 1` is the game's own condition for the op to
  exist. The repaired deck, 137301 -> 137302, morphs on the same varbit.
- **Wait.** Each row carries `"chain": [{"wait": 5}]`, the same as the
  Isafdar crawls. If the crossing commits the player's tile only at the far
  end, as a forced move does, then without the wait the landing check would
  call it refused after about 5 ticks. The bridge is the only route, so the
  walk would then fail.

**Not verified live:** the crossing's timing and landing tiles, and that the
game takes the click from the east end.

## Highweald mine (2026-10-01)

Hearts of Sanguine opens the Highweald mine north of Wendlewick; its interior
(Havenmine: phasmatite, necrite and havensilver rocks) lies 6400 tiles north,
from y 8066. The cache already gives the interior's collision (one area from
the exit up through the rocks at 3482..3497, 8083..8103), but no row joined it
to the surface, so nothing inside could be planned to.

- **Entrance.** Loc 136468 (`WENDLE_MINE_ENTRANCE_MULTI`), op 0 `Enter`. The
  cache places it once: 3513,1694, shape 10, rotation 0, 7x7, so it covers
  3513..3519, 1694..1700. The only standable tile against it is 3517,1693 on
  its south edge, so the row sits on the footprint at 3517,1694 (the host
  matches the whole footprint) and approaches from there; the anchor has no
  standable tile within the approach radius.
- **Exit.** Loc 136471 (`WENDLE_MINE_EXIT`), op 0 `Exit`, placed once at
  3486,8066, shape 10, rotation 2, 5x5 (3486..3490, 8066..8070). The cave opens
  north onto 3487..3488, 8071, so the row sits at 3488,8070.
- **Landings.** Neither the cache nor the dumps give one. Each row lands on
  the standable tile in front of the other loc's mouth: 3488,8071 inside (the
  exit's centre column), 3517,1693 outside. Both are already standable, so the
  bake snaps neither; a real landing up to 5 tiles off is still inside the
  executor's slack and in the same area.
- **Gate.** 136468 morphs on varbit 60597 (`QUEST_WENDLE_SANGUINE_MAIN`, varp
  12736 bits 0..7): 0..50 -> 136469 "Ancient boulder" (no ops), 51..75 ->
  136470 "Cave entrance" [Enter], anything above 75 -> the boulder again
  (rs3-cs2-dumps `locations.json` `morphs_1`). Quest 527 ends at 75 (its
  `endvalue` in `quests.json`), so `varbit_at_least 60597 >= 51` is exactly
  the game's condition for the op to exist, and stays true once the quest is
  done. 51 is the quest's "mine cave" stage, where Gidon moves inside.
- **Exit ungated.** 136471 has no morph table; anyone inside can leave.

**Not verified live:** both landing tiles and whether either move needs a wait
before the landing check.

## Amberfell south-east barricade (2026-10-01)

The barricade on Amberfell's south-east side (Ash guards it) splits the town
from the ground east of it, towards Berylbrook. Heralds of Crimson opens a
Climb over on it. Before these rows, area 2556 (west) and area 2559 (east) had
no transition between them, so a walk from one side to the other had no route
except the long way round, by teleport and the Wendlewick bridge.

- **Loc.** 141015, op 0 `Climb over`. The cache places it once: 3762,1559,
  shape 11, rotation 2, 2x5, so it covers 3762..3763, 1559..1563. 121189 (the
  old id, on the same tile in the 9-16 spawn table; the wiki lists it as
  `histid`) is no longer placed there. No row named either id before this one.
- **Rows.** The host matches the whole footprint, and the bake joins a row to
  every area within one tile of the row's tile. So each row tile is the
  footprint tile that touches only its own side. Eastbound: 3762,1562, next to
  west tiles 3761,1562 / 3761,1563 / 3762,1563. Westbound: the anchor,
  3762,1559, next to east tiles 3763,1559 / 3763,1560. 3762,1561 touches both
  sides, so neither row uses it.
- **Landings (estimates).** One tile past the footprint on each side: 3764,1559
  east and 3761,1562 west. Both are standable, so the bake snaps neither. Live
  on 2026-10-01 the player clicked from 3760,1561, and the climb never
  completed (see below), so no landing has been observed.
- **Gate.** 141015 morphs on varbit 62090, the progress varbit of quest 533
  Heralds of Crimson (start 5, end 215 in `quests.json`). Its `morphs_1` ids
  are `[141162, 141016]`: 0 -> 141162 "Barricade" (Examine only), anything
  else -> 141016 "Barricade" [Climb over]. The last entry is the default, as
  in 136468's table above, and live the Climb over was offered at 10, 25 and
  30. So `varbit_at_least 62090 >= 1` is the game's own condition for the op
  to exist, and it stays true after the quest (the wiki: "During and after the
  quest"). Both rows click the same loc from either side, so both are gated.
- **Quest refusal is not gated.** At 62090 = 30, Anya stops the climb ("Wait!
  We must first deal with these interlopers!"). That is quest dialogue on an op
  that exists, not the loc's condition, so the rows do not encode it. A quest
  script at that stage has to fight first.
- **No wait.** Like the other Climb over rows, these have no chain wait.

**Not verified live:** both landing tiles, the climb's timing, and that the
game takes the click from the east side.

## Exalted Quarry broken ladders (2026-10-02)

The Exalted Quarry (Havenhythe Part II, 28 September 2026), south of
Heathervein, is a pit in three tiers. The ground around it is area 2559, the
same area as the ground east of the Amberfell barricade. The middle terrace is
area 2936. The floor, with the exalted colossus (140913, 3850,1627, 6x6) and
Aurora, is area 2937. Cliff walls in the cache's own collision separate the
tiers. For example, 3865,1628 has a wall east and 3866,1628 a wall west; 3855,1621 has
a wall north and 3855,1622 a wall south. So this is not missing collision or a bake
artifact. Before these rows, nothing could be planned to the quarry floor, and
a Heralds of Crimson step that walks from Wendlewick to the colossus had no
route.

- **Locs.** Four locs cross the cliffs. They are all agility obstacles (param
  6668 points at a table-97 dbrow), and no row named any of them.
  The bake derives ladders only between planes, and all four stand on plane 0,
  so it made no transition for them. The cache places each once:
  - 140908 `Broken ladder`, op 0 `Traverse`: 3865,1628, shape 10, rotation
    3, 1x3 turned, so 3865..3867 on y 1628. Ground (2559) to terrace (2936),
    east side.
  - 140907 `Broken ladder`, op 0 `Traverse`: 3855,1620, stored plane 1 on a
    bridge column (so plane 0), shape 10, rotation 0, 1x3, so 3855 on
    1620..1622. Terrace (2936) to floor (2937), south side.
  - 140910 `Ladder`, op 0 `Climb`: 3836,1631 (stored plane 1). Ground to
    terrace, west side. 140909 `Ladder`, op 0 `Climb`: 3849,1638. Terrace to
    floor, north side.
- **Only the broken ladders.** The quest guide's route goes "to the lowest level by traversing the
  broken ladders". The wiki says talking to Aurora on the floor is what
  "unlock[s] access to the mine". Nothing in the cache shows whether the two
  `Climb` ladders work before that. If they were rows, the planner would take the west
  ladder from Amberfell, because it is nearer. So they are left out until someone
  checks them live.
- **Rows.** The host matches the whole footprint. The bake joins a row to every
  area within one tile, except tiles walled off from the row tile. So each row tile is a
  footprint tile whose walkable neighbours are all on its own side. Down:
  3867,1628 -> 3864,1628 and 3855,1620 -> 3855,1623. Up: 3865,1628 ->
  3868,1628 and 3855,1622 -> 3855,1619.
- **Landings (estimates).** One tile past the footprint on the far side. All
  four are standable, so the bake snaps none of them. The landings are not
  observed.
- **No gate.** 140907 and 140908 have no morph table (`varbitId`/`varpId` -1
  in NXTCacheLibrary, no `morphs_1` in the 9-29 `locations.json`), and no loc in
  that dump morphs into either. So the game gives no condition for the op
  to exist, and the rows carry none. The wiki says the quarry needs "partial
  completion" of Heralds of Crimson. Whatever enforces that is server-side and
  is not encoded. On foot from the west, the route already crosses the
  barricade, which is gated on 62090 >= 1.
- **No Agility level.** Dbrows 20149 and 20150 are empty in the cache and in
  the dumps, the same as 137303's 3901. The quest guide gives Agility levels
  for other shortcuts on this route (72 for the stepping stones) but none for
  the broken ladders. If the game does need a level, these rows do not encode it.
- **No wait.** Like the other `Traverse` rows, these have no chain wait.
- **Category.** None of the four is in `MoveCategoryLocs.inc`, so the rows
  classify as doors, as 141015 does. Fixing that means regenerating the table
  and rebuilding the library.

**Not verified live:** the landing tiles, the traverse timing, whether either
broken ladder needs an Agility level or a quest stage, and whether the
`Climb` ladders could replace them.

## Draynor Sewers (2026-10-03)

Nothing reached the Draynor Sewers (the zombies around 3119,9647) before
these rows: no row and no derived link touched the sewer's area (48201), so
`wwcli path` from Lumbridge said "no route".

- **Locs.** The cache places one entrance and one exit:
  - 6434 `Trapdoor`, op 0 `Open`: 3118,3244, shape 10, rotation 0, 1x1,
    blockwalk. It has no morph table; opening it swaps it for 6435
    `Trapdoor`, op 0 `Climb-down`, on the same tile. 6435 has no `Close`,
    so it reverts on the server's timer.
  - 26518 `Ladder` (`POG_SEWER_PIPE_SIDE_LADDER`), op 0 `Climb-up`:
    3118,9643, shape 4 (wall decoration), rotation 3. The tile is
    standable and in area 48201.
- **Rows.** Down, the 6434 row clicks `Open`, waits 3 ticks and then clicks
  6435's `Climb-down` with a raw loc action, `[3, 6435, 3118, 3244]` (action
  3 is loc op 1; the host's own loc click uses the same world-tile params).
  A second row clicks 6435 directly, for a trapdoor someone left open. It
  lands on 3118,9644 rather than 9643 only because the bake keeps one
  transition per endpoint tuple and kind, so two rows with the same ends
  collapse into the first. Its `extra_cost: 5` (8 against the Open row's 6)
  puts the closed trapdoor, the resting state, first. Whichever row meets
  the wrong state finds its loc missing, and the executor routes onto the
  other. Up, 26518 lands on 3118,3245, the open tile north of the trapdoor.
- **No gate.** Neither loc has a morph table or a quest var in the dump.

**Not verified live:** the landing tiles (3118,9643 under the ladder and
3118,3245 north of the trapdoor), the 3-tick wait between `Open` and
`Climb-down`, and how long the trapdoor stays open.

## Brass key door and the Edgeville Dungeon (2026-10-03)

Live on 2026-10-03 a walk to the Edgeville Dungeon (3119,9862) failed over
and over at transition 7509: the door 1804 at 3115,3449, derived from the
cache with no gate, into the house over the dungeon (ladder 12389 at
3116,3452). The account had no Brass key, and the game keeps that door
locked without one.

- **The key.** Obj 983 `EDGEVILLEDUNGEONKEY`, "Brass key"; its keyring
  description (param 7511) is "Allows access to Edgeville Dungeon." It can
  be stored on the Steel key ring (obj 4446): clientscript 2261 puts 983 in
  slot 0 of the ring, and 2388 reads slot n as bit n of varp 2673
  (`favour_keyring`). No varbit covers that bit, hence `varp_bit`. The ring
  on the tool belt is varbit 40074 (`toolbelt_keyring`).
- **Door 1804** (`Door`, op 0 `Open`, shape 0, rotation 1, at 3115,3449) has
  no morph; the lock is the server's. Six rows replace the two derived hops
  (a row on a door's origin tile suppresses the derived hop from it), three
  each way, one per way of holding the key:
  - Brass key in the pack or worn: `items` 983. Lands on 3115,3450 going in,
    3115,3449 going out.
  - Brass key on the ring, ring on the tool belt: `varp_bit` 2673 bit 0 and
    `varbit_at_least` 40074. Lands on 3116,3450 / 3116,3449.
  - Brass key on the ring, ring in the pack: `varp_bit` 2673 bit 0 and
    `items` 4446. Lands on 3114,3450 / 3115,3448.
  The odd landings are only there to survive the endpoint dedup; each is the
  next standable tile of the same area, inside the executor's landing slack.
  Leaving is gated too: whether the door opens from inside without a key is
  not in any dump, and gating it keeps the planner from climbing up into a
  house it cannot leave.
- **Live, only the pack row passes today.** The executor reads item counts
  (pack and worn) for every requirement, but varps only from the host's
  `readCapability`, and the host's varp list (`REQUIREMENT_VARPS` in
  `WorldWalkerCallbackBridge`) does not hold 2673, so both ring rows read 0
  and are denied until the host adds it. A reader older than `varp_bit`
  denies those rows too.
- **The other ways in.** Without the key the walker uses:
  - Edgeville trapdoor 26933 (`Trapdoor`, op 0 `Open`, shape 22, rotation 1)
    at 3097,3468, opening to 26934 (`Climb-down`, `Close`). As with the
    Draynor trapdoor: `Open`, wait 3 ticks, raw loc action
    `[3, 26934, 3097, 3468]`, landing 3096,9867; a second row clicks an
    already open 26934, landing 3096,9868 with `extra_cost` 5. Out: ladder
    29355 (`Climb-up`) at 3097,9868 to 3096,3468. Both landings are in the
    dungeon's area that holds 3119,9862.
  - Varrock manhole 881 (`Manhole`, op 0 `Open`) at 3237,3458, opening to
    882 (`Climb down`, `Close`), the same two-row shape, landing 3237,9865
    and 3236,9865. Out: ladder 128335 (`Climb-up`) at 3237,9866 to
    3237,3459. The sewer is not walkable to the Edgeville Dungeon in the
    collision, so this does not serve 3119,9862; it is here because nothing
    reached the Varrock Sewers before.
- **No gate** on the trapdoor or the manhole: neither has a morph table or a
  quest var in the dump.

**Not verified live:** every landing tile above (they are the nearest
standable tiles, not observed), the 3-tick waits, whether the door needs the
key from inside, and whether a key on a ring left in the bank is refused.

## Guild doors (2026-10-03)

Live on 2026-10-03 a walk to 2917,3289 failed at transition 6982: the
Crafting Guild's north door, loc 133375 (`Guild door`, shape 0, rotation 3)
at 2935,3292, derived with no gate. The account had Crafting below 40. The
goal is not a field outside the guild: in the collision the whole stretch
from x 2919 to the guild hall (area 9782 in that bake) is walled in, and the
north door is its only way in from outside (the east `Guild door` at
2938,3272 opens onto a pocket reachable only through the guild). 2917,3289
itself is not standable and the planner ends on 2919,3291, inside.

The levels come from the cache, not memory. Each guild's "Enter the ...
Guild" achievement lists what entry needs (`skill_reqs_8`, `op_9`), and the
skill guide's "Access to the ... Guild" structs agree on the levels:

| Guild | Crossing | Gate | Evidence |
|---|---|---|---|
| Crafting | door 133375 at 2935,3292, new row | Crafting 40 | achievement 4094; skill guide struct 6085 |
| Cooks' | door 2712 at 3143,3443, three new rows | Cooking 32 and one of: Chef's hat 1949, Varrock armour 3 (11758), Varrock armour 4 (19757) | achievement 4699; Varrock armour 3 reward "Access the Cooking Guild without a chef's hat" |
| Mining | ladder 2113 at 3019,3339 and its four adjacent-tile rows; door 2112 at 3046,9757 from the Dwarven Mine | Mining 60 | achievement 683; struct 42965 |
| Fishing | gate 49016 at 2614,3386, the only land way into the grounds | Fishing 68 | achievement 4587; struct 45992 |
| Wizards' (Yanille) | door 1601 at 2584,3087 | Magic 66 | achievement 488; skill guide struct 5791 |
| Warriors' | door 15653 at 2877,3542, three new rows | Attack 65 + Strength 65, or Attack 99, or Strength 99 | achievement 4089 lists 65 + 65 |
| Champions' | door 1805 at 3191,3363, new row | `varp_at_least` 1297 (`qp`) 33 | achievement 955: "Quest Points: 33 (to enter the Champions' Guild)", var 1297 |
| Heroes' | double door 2624 / 2625 at 2917,3513-3514, new rows | `varp_at_least` 2618 (`heroquest`) 15 | quests.json: Heroes' Quest ends at 15 on varp 2618 |

Only the way in is gated; leaving is left free, so an account that is
inside can always walk out. Rows with alternatives land one tile apart (see
"Gates are ANDed"). The Brown apron shown on the Crafting Guild's skill
guide entry is an icon only: neither the struct nor the achievement asks for
it, so no row does.

- **Warriors' Guild.** The game's rule is Attack + Strength of 130, or 99 in
  either; a row cannot add two levels, so the first row asks for the
  achievement's 65 + 65 and the other two for a 99. An account at, say,
  70 + 60 is refused a door it could open. The 99 rows are from the wiki,
  not the cache.
- **Cooks' Guild.** The hat must be worn, and the gate passes on one held in
  the pack. The cooking capes, which the wiki says also admit, have no row.
- **Champions' and Heroes' Guilds are varp gates**, so the live bot refuses
  them until the host adds varps 1297 and 2618 to `REQUIREMENT_VARPS`. Until
  then a walk into either guild has no route.
- **Left alone:** the Legends' Guild gate 2391 / 2392 and doors 2896 / 2897
  (who may pass depends on the stage of Legends' Quest, and no dump says
  which stage opens which); the Ranging Guild, which has no route in at all
  (the fence door at 2658,3438 bakes no transition), so there is nothing to
  gate.

**Not verified live:** every gate above; the alternative landings at the
Cooks' and Warriors' doors.

## Lumbridge Swamp dark hole (2026-10-04)

Live on 2026-10-04 (02:19-02:26) a walk to the Lumbridge Swamp Caves
(3150,9555) failed over and over at transition 314. Each time the host
logged `interact: loc 91021 matched at (3163,3166,0) for row tile
(3162,3167,0); 3x3 footprint 1 tile(s) away`, the walk re-planned, and it
picked the same row for seven minutes. The user had also seen the bot walk
around the hole to its far side before it climbed down.

**The loc.** 91021 (`TMF_GOBLIN_CAVE_ENTRANCE_MULTI`) is anchored at
3163,3166 with rotation 1 and covers 3163-3165 x 3166-3168. It has no
options of its own. It morphs on varbit 4291 (`tmf_main`) into 5947
`Dark hole under tree` (`Climb down`) or 91020, the same hole after The
Mighty Fall (`Climb-down`). Both forms carry `forceapproach` 7 (north, east
and south blocked). Rotated once, that leaves only the north side open. The
collision agrees: every tile next to the west, south and east edges is
blocked, and on the north edge 3164,3169 and 3165,3169 are floor. The climb
back up (78630) also lands on 3164,3169.

**Why the walk circled.** The row's origin, 3162,3167, is a blocked tile
west of the footprint. The planner stood on the nearest floor within a tile
of it, 3161,3167. From there the hole cannot be clicked, so the game walked
the player south, east and north around the hole to reach the north edge.

**The row now:**

| Row | Origin | Dest | Gate |
|---|---|---|---|
| `Climb down` 91021 | 3164,3169 (was 3162,3167) | 3169,9571 | varbit 16231 (`swamp_caves_roped_entrance`, varp 3145 bit 3) = 1 |

- **Origin.** 3164,3169 is floor on the open north side, one tile from the
  footprint. The host matches the loc by footprint, so it still finds
  91021. A walk from 3165,3203 now comes down the east side
  (3167,3173 -> 3165,3171 -> 3164,3169) and climbs from where it stops.
- **Gate.** A player has to use a Rope (item 954) on the hole once before
  they can climb down. `swamp_caves_roped_entrance` records that the rope
  is tied, and no loc morphs on it, so the tied rope does not show on the
  hole. Without the varbit, `wwcli path` now says "no route" into the caves
  instead of the executor failing the climb forever.
- **No rope row.** Tying the rope is item-on-loc: `SELECT_COMPONENT_ITEM`
  on the rope's backpack slot, then `SELECT_OBJECT` on the hole. A chain
  cannot do that today. `click_item` resolves the slot at run time but only
  sends `COMPONENT` / `COMPONENT_SPECIAL`, and a raw `action` step would
  fix the slot when the row is written. Until the executor has an
  item-on-loc step, the rope has to be tied by a script or by hand.

**Not verified live:** that the server sends varp 3145 to the client (the
host reads varbits from the client's own copy of the varp). If it does not,
the varbit reads 0 even after the rope is tied, and a roped account also
gets "no route". Also not verified live: that the climb is accepted from
3164,3169 with no extra step.

## Port Sarim to Musa Point boat (2026-10-07)

The "Boats" row from the Port Sarim dock (3027,3219) to Musa Point
(2956,3146) named `object_id` 0 and no NPC, so the bake dropped it (no-loc) and
nothing reached Karamja from Port Sarim but the charter. The boat is crewed:
Captain Tobias (NPC 376, op 0 "Travel (Karamja)", standing at 3028,3216 per
the NPC location dump), so the row is now an `npc` origin with a 10-tick
wait for the crossing. The free-to-play routing needs it: north Karamja is
free land, and the charter is members-only.

Left as they were, and still dropped: the near-duplicate row one tile east
(3028,3219), and both rows back from Musa Point, whose Customs officer (NPC
380, "Travel (Port Sarim)") may ask questions on the way that no dialog zone
answers yet. Not run against a live client: the wait and whether the fare
raises a conversation are unverified.

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
