# f2p_zones.json: evidence and doubts

Draft of 2026-10-07. Files in this directory:

- `f2p_zones.json` is the deliverable: 51 zones and 9 members holes.
- `make_zones.py` regenerates it. The boxes, sources and notes are written there: `python make_zones.py f2p_zones.json`.
- `check_zones.py` checks the schema, the test points and the lodestones. A tile counts as F2P when it is in some zone and in no hole, with planes taken into account.

## Method

1. **Wiki facts.** I read raw wikitext through `runescape.wiki/api.php` (RS3 wiki only), about 150 pages. From each page I took:
   - the `members =` infobox field;
   - the explicit members/F2P sentences;
   - the `{{Map|x=..|y=..}}` anchors, which are real game tiles for mapID 28 and for dungeon map IDs.
2. **Borders from the bake.** The wiki gives centres, not borders. I took the borders from the walkable-area grid in `../ww-f2p/shipped.wwa`. My scratch tool reimplements `areas.py`.
   - Where a members region has its own area id I put the box edge just inside it: Kandarin 5446, Morytania 8032, the south desert 5047, Brimhaven 6141, south Karamja 6330, Entrana 10381, Death Plateau 12593 and the Troll Country areas.
   - A validator walks every tile of every zone minus holes on planes 0-3. Result: **0 tiles of any known members area are counted F2P.** The catch-all areas 0, 1733 and 44414 are ignored, as are the solid filler blocks 49628 and 45229.
3. **Coverage.** The zones cover:
   - 126,681 of the 131,111 tiles of mainland area 7209 (96.6%);
   - 123,983 of 124,164 tiles of Wilderness area 12709.

   The tiles of 7209 that are left out are deliberate: Fort Forinthry and the Woodcutters' Grove, the strip east of Paterdomus, and land north-west of Burthorpe.

## Per box evidence

Each box's `source` and `note` in the JSON hold the same evidence in short form. Quotes are from the RS3 wiki.

### Mainland (planes 0-3)

| box | anchors used | evidence |
|---|---|---|
| Misthalin and Asgarnia core 2880..3398 x 3158..3519 | Lumbridge 3200,3215; Varrock 3212,3443; Falador 3000,3353; Draynor 3103,3259; Edgeville 3090,3490; Port Sarim 3033,3220; Rimmington 2950,3222; Taverley 2904,3447; Goblin Village 2956,3502; Ice Mountain 3010,3486; Edgeville Monastery 3052,3490; Barbarian Village 3081,3426; Draynor Manor 3113,3356; Champions' Guild 3193,3357; Crafting Guild 2929,3280; Archaeology Campus 3352,3378; Varrock Dig Site 3356,3428; Exam Centre 3359,3351; Silvarea 3369,3508; Jolly Boar Inn 3281,3498 | All of these have `members = No`. Edges: x=2880 keeps Entrana out (its walkable tiles reach x=2878); x=3398 is west of the first Morytania tile (3400,3364); y=3519 is the Wilderness wall (the gates at y=3520 lead to y=3523). |
| East-bank strips (5 boxes, x 3399..3452) | Kharid-et 3367,3189; River Salve 3415,3355; Paterdomus 3411,3492 | I measured each row on the bake, so every edge stays at least 1 tile west of the first Morytania tile. The Paterdomus strip stops at x=3416, the temple's east wall. Area 7209 reaches x=3426 behind the temple. I left x 3417..3426 out because the wiki does not map where Morytania starts. Priest in Peril has been F2P since 9 October 2017 (Free-to-play page). |
| South coast 2960..3195 x 3106..3157 | Mudskipper Point 2994,3116; Wizards' Tower 3104,3160; Lumbridge Swamp 3193,3167 | `members = No`. West edge x=2960 keeps south Karamja out (6330 reaches x=2945). East edge x=3195: tiles of the members desert reach x=3200 at y<=3128. |
| Lumbridge Swamp east / Al Kharid west 3196..3240 x 3129..3157 | (fills the gap) | Desert tiles occur only at y<=3128 here. |
| Al Kharid south strip 3241..3328 x 3136..3157; Shantay Pass north side 3290..3314 x 3117..3133; Kharid-et south approach 3359..3452 x 3146..3157 | Al Kharid 3300,3168; Shantay Pass 3305,3123; pass transition 3303,3117 -> 3304,3115 | Kharidian Desert page: "Al Kharid is accessible by all players alike, but the desert south of the Shantay Pass is restricted to members only." The walkable desert tiles that join the southern desert run north of y=3118 along both sides of Al Kharid: x<=3290 at y<=3133, x 3331..3358 at y<=3157, and x>=3390 at y<=3145. The three boxes stop short of them. |
| Taverley west, 3 stepped boxes, x 2852/2860/2865..2879, plane 0 only | Land of Snow portal 2853,3461; Burthorpe mine 2873,3502; loc 66990 at 2879,3459 | White Wolf Mountain page: it "forms part of the border between the free-to-play and members areas". The steps follow Kandarin area 5446, which comes within 1-3 tiles. They are plane 0 only because plane 1 here holds the White Wolf Mountain gnome glider landing (area 12137, members). |
| Burthorpe 2826..2943 x 3520..3577 | Burthorpe 2898,3540; Burthorpe Castle 2898,3563; Warriors' Guild 2857,3542; Troll Cave 2878,3575 | `members = No`. Taverley page: "Burthorpe and Taverley have been opened up to free players". The north edge keeps Death Plateau (2860,3591, `members = Yes`, "the plateau itself is inaccessible in free-to-play") out. |

### Wilderness, Daemonheim, Karamja

| box | anchors | evidence |
|---|---|---|
| Wilderness (south) 2944..3389 x 3520..3599 | Wilderness Crater 3133,3709; Black Knights' Fortress 3017,3559; Graveyard of Shadows 3225,3682 | Wilderness `members = No`. |
| Wilderness (north) 2944..3440 x 3600..4000 | Red Dragon Isle 3203,3830; Lava Maze 3075,3853; Deserted Keep 3154,3930; Rogues' Castle 3287,3933; Frozen Waste Plateau 2961,3921; Mage Arena 3105,3933 | Deep Wilderness: "originally accessible to members only, but following an update on 9 October 2017, it was opened to free players." Red Dragon Isle "is now accessible to free players". West edge x=2944 is the Troll Country border (Troll Country `members = Yes`; Troll Country walkable areas end at x<=2936). |
| Daemonheim peninsula 3379..3520 x 3600..3800 | Daemonheim 3449,3714 | `members = No`. The Daemonheim Dig Site (3449,3724) has `members = Yes`, but it is an Archaeology activity on land free players walk on, so I made no hole. |
| Musa Point 2816..2970 x 3136..3210 | Musa Point 2889,3178; Karamja Volcano 2850,3172 | Karamja: "Musa Point is the north eastern part of Karamja, and is the only part of Karamja that free-to-play players can access, apart from the Karamja-Crandor dungeon." The edge sits on Brimhaven area 6141 (x<=2815). |
| Crandor 2820..2866 x 3230..3312 | Crandor 2838,3266 | `members = No`, but "A few tiles on the western side of the island aren't accessible in free-to-play worlds." I pulled the west edge in by 6 tiles from the walkable x=2814. The exact members strip is only an image on the wiki. |

### Dungeons (plane 0, y + 6400) and separate map regions

| box | anchors | evidence |
|---|---|---|
| Elvarg's lair 2826..2868 x 9547..9601 | entrances 2834,3258 and 2856,3167 | "Karamja Volcano Dungeon" redirects here; `members = No`. |
| Dwarven Mine 2971..3062 x 9755..9854 | map centre 3012,9792 | `members = No`. The west edge x=2971 keeps Taverley Dungeon (area 48618, x<=2970, `members = Yes`) out. |
| Mining Guild 3015..3057 x 9730..9756 | map centre 3036,9745 | `members = No`. |
| Edgeville Dungeon 3078..3152 x 9816..9929, minus 2 holes | map centre 3120,9913; members gate locs at 3131-3132, 9917-9918 | `members = No`. Wilderness page: "the north part of Edgeville dungeon ... can be accessed by passing a members gate". The holes cover area 49667 behind that gate. |
| Varrock Sewers 3226..3290 x 9855..9920 | map centre 3218,9889; manhole 3237,3458 -> 3237,9865 | `members = No`. I left the west section (area 49672, deadly red spiders, x 3153..3224) out. In the bake it is reachable only through the members pipe or a resource dungeon. GUESS that it is the members section the wiki trivia mentions. |
| Draynor Sewers 3078..3128 x 9641..9699 | map centre 3104,9670 | `members = No`. |
| Lumbridge Castle cellar 3206..3221 x 9613..9627 | trapdoor 3209,3216 | Lumbridge Castle `members = No`. The Dorgesh-Kaan tunnels need The Lost Tribe (members) and are not included. |
| Draynor Manor basements 3075..3120 x 9743..9789 | stairs 3115,3355; ladder 3092,3361 | `members = No`. |
| Asgarnian Ice Dungeon, 2 boxes, 2984..3069 x 9540/9559..9600 | trapdoor 3008,3150 | `members = No`. The skeletal wyvern chamber (area 47801, y<=9558) "can be entered only by members". |
| Forinthry Dungeon, 2 boxes | map centre 3075,10115 | `members = No` (revenants). Split around a solid filler block. |
| Lava Maze Dungeon east 3061..3072 x 10249..10262 | ladder 3017,3849 | "The smaller eastern area of the dungeon, accessible by free players". |
| Paterdomus mausoleum 3401..3409 x 9893..9909 | Mausoleum 3405,3505 | Silvarea: free players can go "as far as the Holy barrier in the Mausoleum under Paterdomus, which can only be passed by members". |
| War's Retreat 3268..3325 x 10117..10155 | portal 3105,3310 | "The hub is accessible to free-to-play players." |
| Lumbridge Catacombs 3850..4035 x 5455..5570, planes 0-2 | entrance 3247,3198 -> 3877,5526 | "a free-to-play dungeon that was released along with The Blood Pact". |
| King Black Dragon's Lair 2253..2291 x 4677..4714 | map 2272,4696 | `members = No`. The Free-to-play page says KBD achievements "can be completed in free-to-play". |
| Mage Arena bank 2527..2549 x 4709..4724 | lever 3089,3957 | "Unlike the Mage Arena minigame itself, this bank area is accessible to free-to-play players." |
| Burthorpe mine, Burthorpe Troll Cave, Burthorpe Games Room | entrances 2873,3502; 2878,3575; 2893,3569 | Each has `members = No`. |
| Air, Water, Earth, Fire, Mind, Body altars | the exit-portal and ruins transitions in the bake | Each has `members = No`. |
| Yeti Town 4979..5077 x 9590..9699 | Land of Snow portal 2853,3461 -> 5024,9662 | Yeti Town `members = No` (released with Violet is Blue, 2018). |
| Ashdale 2430..2562 x 2622..2754, planes 0-3 | lodestone 2474,2709 on plane 2 | `members = No`; "Ashdale contains the southernmost point on the map accessible in free-to-play". |
| City of Um 1020..1180 x 1700..1852 | lodestone 1083,1768 on plane 1; portal 3104,3312 -> 1025,1762 | `members = No`. The Free-to-play page lists "Necromancy skill up to level 20 ... City of Um". |
| Havenhythe: Wendlewick 3400..3600 x 1420..1610 | Wendlewick lodestone 3461,1520 | Havenhythe `members = No`. Free-to-play quests: Visions of Havenhythe, Hearts of Sanguine. See the doubts for how I converted the coordinates. |

### Holes

| hole | evidence |
|---|---|
| Fort Forinthry 3273..3344 x 3523..3577 | `members = Yes`. Lodestone page: "the only members-only lodestone to be located in the free-to-play part of the world" (object 3298,3528). The fort sits in what used to be the Wilderness (Grieving Vale) and is walk-connected to Varrock in the bake, so it is a walk-only members crossing. Bounds come from the fort's walkable tiles. |
| Woodcutters' Grove, 2 boxes, 3345..3398 x 3523..3590 | `members = Yes`, map 3360,3544, "just outside the east wall" of the fort. |
| Wilderness Agility Course and its entrance pipe | `members = Yes`, map 2998,3949, entrance 2998,3916. "Logging into the Wilderness agility course in a free to play world will now remove you from the area." Bounds are areas 13998, 13999 and 14001. |
| Heroes' Guild 2905..2916 x 3510..3517 | `members = Yes`, map square 2910,3513. |
| Mage Training Arena 3355..3371 x 3298..3324 | `members = Yes`, map 3363,3309. |
| Edgeville Dungeon, Wilderness part, 2 boxes | See the Edgeville Dungeon row above. |

## Test points, extra checkpoints and lodestones

This is the output of `python check_zones.py`. Exit code 1 means one expectation failed: the Digsite. See the conflicts below.

51 zones, 9 holes

| expect | point | x,y,plane | result | zones / holes |
|---|---|---|---|---|
| F2P | Lumbridge | 3222,3218,0 | PASS | Misthalin and Asgarnia core |
| F2P | Varrock | 3212,3428,0 | PASS | Misthalin and Asgarnia core |
| F2P | Draynor | 3093,3245,0 | PASS | Misthalin and Asgarnia core |
| F2P | Edgeville | 3094,3492,0 | PASS | Misthalin and Asgarnia core |
| F2P | Falador | 2965,3380,0 | PASS | Misthalin and Asgarnia core |
| F2P | Taverley | 2895,3450,0 | PASS | Misthalin and Asgarnia core |
| F2P | Taverley cave entrance (loc 66990) | 2879,3459,0 | PASS | Taverley west (to White Wolf Mountain foot) |
| F2P | Burthorpe | 2899,3544,0 | PASS | Burthorpe |
| F2P | Port Sarim | 3010,3220,0 | PASS | Misthalin and Asgarnia core |
| F2P | Port Sarim dock | 3029,3217,0 | PASS | Misthalin and Asgarnia core |
| F2P | Wilderness | 3100,3680,0 | PASS | Wilderness (north, incl. Deep Wilderness) |
| F2P | Shantay Pass north side | 3303,3120,0 | PASS | Shantay Pass north side |
| F2P | Musa Point | 2918,3176,0 | PASS | Misthalin and Asgarnia core; Musa Point (north-east Karamja) |
| members | South of Shantay Pass | 3304,3110,0 | PASS | - |
| members | Digsite | 3360,3420,0 | FAIL | Misthalin and Asgarnia core |
| members | Brimhaven | 2760,3238,0 | PASS | - |
| members | Taverley Dungeon | 2875,9880,0 | PASS | - |
| members | Catherby | 2810,3440,0 | PASS | - |
| members | White Wolf Mountain | 2848,3498,0 | PASS | - |
| members | Death Plateau | 2865,3590,0 | PASS | - |
| members | Canifis | 3500,3490,0 | PASS | - |
| members | Ardougne | 2660,3305,0 | PASS | - |

Extra wiki-derived checkpoints:

| expect | point | x,y,plane | result | zones / holes |
|---|---|---|---|---|
| F2P | Grand Exchange | 3165,3485,0 | PASS | Misthalin and Asgarnia core |
| F2P | Exam Centre | 3359,3351,0 | PASS | Misthalin and Asgarnia core |
| F2P | Silvarea | 3369,3508,0 | PASS | Misthalin and Asgarnia core |
| F2P | Paterdomus temple | 3411,3492,0 | PASS | Paterdomus and Silvarea east end |
| F2P | Kharid-et | 3367,3189,0 | PASS | Misthalin and Asgarnia core |
| F2P | Het's Oasis | 3361,3232,0 | PASS | Misthalin and Asgarnia core |
| F2P | Edgeville Monastery | 3052,3490,0 | PASS | Misthalin and Asgarnia core |
| F2P | Black Knights' Fortress | 3017,3559,0 | PASS | Wilderness (south) |
| F2P | Goblin Village | 2956,3502,0 | PASS | Misthalin and Asgarnia core |
| F2P | Ice Mountain | 3010,3486,0 | PASS | Misthalin and Asgarnia core |
| F2P | Barbarian Village | 3081,3426,0 | PASS | Misthalin and Asgarnia core |
| F2P | Draynor Manor | 3113,3356,0 | PASS | Misthalin and Asgarnia core |
| F2P | Wizards' Tower | 3104,3160,0 | PASS | Misthalin and Asgarnia core |
| F2P | Mudskipper Point | 2994,3116,0 | PASS | Asgarnia and Misthalin south coast |
| F2P | Rimmington | 2957,3214,0 | PASS | Misthalin and Asgarnia core |
| F2P | Crafting Guild | 2929,3280,0 | PASS | Misthalin and Asgarnia core |
| F2P | Warriors' Guild | 2857,3542,0 | PASS | Burthorpe |
| F2P | Land of Snow portal | 2853,3461,0 | PASS | Taverley west (to White Wolf Mountain foot) |
| F2P | Jolly Boar Inn | 3281,3498,0 | PASS | Misthalin and Asgarnia core |
| F2P | Crandor | 2838,3266,0 | PASS | Crandor |
| F2P | Karamja Volcano | 2850,3172,0 | PASS | Musa Point (north-east Karamja) |
| F2P | Daemonheim | 3449,3714,0 | PASS | Daemonheim peninsula |
| F2P | Red Dragon Isle | 3203,3830,0 | PASS | Wilderness (north, incl. Deep Wilderness) |
| F2P | Mage Arena | 3105,3933,0 | PASS | Wilderness (north, incl. Deep Wilderness) |
| F2P | Deserted Keep | 3154,3930,0 | PASS | Wilderness (north, incl. Deep Wilderness) |
| F2P | Varrock Sewers | 3237,9865,0 | PASS | Varrock Sewers |
| F2P | Edgeville Dungeon | 3097,9867,0 | PASS | Edgeville Dungeon |
| F2P | Dwarven Mine | 3020,9800,0 | PASS | Dwarven Mine |
| F2P | Mining Guild | 3036,9745,0 | PASS | Mining Guild |
| F2P | Draynor Sewers | 3104,9670,0 | PASS | Draynor Sewers |
| F2P | Asgarnian Ice Dungeon | 3009,9550,0 | PASS | Asgarnian Ice Dungeon (west) |
| F2P | Elvarg's lair | 2856,9567,0 | PASS | Elvarg's lair (Crandor and Karamja Dungeon) |
| F2P | Forinthry Dungeon | 3075,10115,0 | PASS | Forinthry Dungeon (north) |
| F2P | War's Retreat | 3294,10127,0 | PASS | War's Retreat |
| F2P | Lumbridge Catacombs | 3877,5526,1 | PASS | Lumbridge Catacombs |
| F2P | Yeti Town | 5024,9662,0 | PASS | Yeti Town (Land of Snow) |
| members | Fort Forinthry courtyard | 3305,3553,0 | PASS | Wilderness (south) / holes: Fort Forinthry |
| members | Woodcutters' Grove | 3360,3544,0 | PASS | Wilderness (south) / holes: Woodcutters' Grove (south) |
| members | Wilderness Agility Course | 2998,3949,0 | PASS | Wilderness (north, incl. Deep Wilderness) / holes: Wilderness Agility Course |
| members | Heroes' Guild | 2910,3513,0 | PASS | Misthalin and Asgarnia core / holes: Heroes' Guild |
| members | Mage Training Arena | 3363,3309,0 | PASS | Misthalin and Asgarnia core / holes: Mage Training Arena |
| members | Entrana | 2838,3360,0 | PASS | - |
| members | Edgeville Dungeon chaos druids (Wilderness part) | 3108,9940,0 | PASS |  / holes: Edgeville Dungeon - Wilderness part (north) |
| members | Lumbridge Swamp Caves | 3170,9570,0 | PASS | - |
| members | H.A.M. Hideout | 3149,9651,0 | PASS | - |
| members | Taverley Dungeon entrance cellar | 2886,9795,0 | PASS | - |
| members | Asgarnian Ice Dungeon wyvern chamber | 3040,9548,0 | PASS | - |
| members | Dominion Tower (south of the pass) | 3374,3108,0 | PASS | - |
| members | Kalphite Hive | 3226,3109,0 | PASS | - |
| members | Troll Stronghold path | 2918,3601,0 | PASS | - |
| members | Morytania west bank (Mort Myre) | 3440,3380,0 | PASS | - |
| members | White Wolf Mountain glider (plane 1) | 2850,3493,1 | PASS | - |
| members | Wendlewick fish farm (GUESS coords) | 3371,1503,0 | PASS | - |

| lodestone | x,y,plane | expect | tile | 5x5 around | result |
|---|---|---|---|---|---|
| Bandit Camp | 3214,2955,0 | members | members | 0/25 F2P | PASS |
| Lunar Isle | 2088,3912,0 | members | members | 0/25 F2P | PASS |
| Al Kharid | 3297,3185,0 | F2P | F2P | 25/25 F2P | PASS |
| Ardougne | 2634,3349,0 | members | members | 0/25 F2P | PASS |
| Burthorpe | 2899,3545,0 | F2P | F2P | 25/25 F2P | PASS |
| Catherby | 2811,3450,0 | members | members | 0/25 F2P | PASS |
| Draynor Village | 3105,3299,0 | F2P | F2P | 25/25 F2P | PASS |
| Edgeville | 3067,3506,0 | F2P | F2P | 25/25 F2P | PASS |
| Falador | 2967,3404,0 | F2P | F2P | 25/25 F2P | PASS |
| Lumbridge | 3233,3222,0 | F2P | F2P | 25/25 F2P | PASS |
| Port Sarim | 3011,3216,0 | F2P | F2P | 25/25 F2P | PASS |
| Seers' Village | 2689,3483,0 | members | members | 0/25 F2P | PASS |
| Taverley | 2878,3443,0 | F2P | F2P | 25/25 F2P | PASS |
| Varrock | 3214,3377,0 | F2P | F2P | 25/25 F2P | PASS |
| Fort Forinthry | 3298,3526,0 | members | members | 0/25 F2P | PASS |
| Menaphos | 3216,2717,0 | members | members | 0/25 F2P | PASS |
| Anachronia | 5431,2339,0 | members | members | 0/25 F2P | PASS |
| Yanille | 2529,3095,0 | members | members | 0/25 F2P | PASS |
| Canifis | 3517,3516,0 | members | members | 0/25 F2P | PASS |
| Eagles' Peak | 2366,3480,0 | members | members | 0/25 F2P | PASS |
| Fremennik Province | 2712,3678,0 | members | members | 0/25 F2P | PASS |
| Karamja | 2761,3148,0 | members | members | 0/25 F2P | PASS |
| Oo'glog | 2532,2872,0 | members | members | 0/25 F2P | PASS |
| Tirannwn | 2254,3150,0 | members | members | 0/25 F2P | PASS |
| Wilderness Crater | 3143,3636,0 | F2P | F2P | 25/25 F2P | PASS |
| Ashdale | 2474,2709,2 | F2P | F2P | 25/25 F2P | PASS |
| Prifddinas | 2208,3361,1 | members | members | 0/25 F2P | PASS |
| City of Um | 1083,1768,1 | F2P | F2P | 25/25 F2P | PASS |
| Wendlewick | 3461,1520,0 | F2P | F2P | 25/25 F2P | PASS |

failures: 1

## Conflicts with the brief (stated plainly)

- **Digsite (3360,3420): the wiki says F2P, the brief says members.** I left it F2P, and that test point FAILS by design.
  - Evidence: the Varrock Dig Site and the Archaeology Campus both have `members = No`. The Dig Site map is centred at 3356,3428.
  - The Free-to-play page lists "Archaeology skill up to level 20, on 30 March 2020" and the Archaeology Guild.
  - The 2019 patch note "free-to-play players gaining members-only items at The Digsite" implies free players are there.
  - The earlier peer marked the Digsite as members only as a GUESS. CERTAIN that the wiki says F2P.
- **Lumbridge Swamp Caves (3170,9570): the brief lists it as F2P, the wiki says members.** `members = Yes`, so it is not in any zone. CERTAIN.
- **Paterdomus and Silvarea: the brief says members, the wiki says F2P.** Silvarea: "Eventually it was made free-to-play". Paterdomus has `members = No`. Both are in zones. CERTAIN.
- **Lumber yard east of Varrock: gone.** It was F2P after 9 October 2017, except the sawmill. Its page says "The Lumber Yard has been removed" (13 February 2023, New Foundations). That area is now Fort Forinthry, which is members, and is a hole. CERTAIN.
- **Corsair Cove: not RS3.** There is no RS3 wiki page (the API returns missing). It is an Old School location, so there is no box. CERTAIN that the page is missing.
- **White Wolf Mountain: the infobox says `members = No`.** But the page calls the mountain the F2P/members border, and the cave on the Taverley side leads to the members side. The test point (2848,3498) is outside every zone, as the brief expects. INFERRED.

## Fort Forinthry, City of Um, Wendlewick

| place | verdict | evidence | tag |
|---|---|---|---|
| Fort Forinthry | members | Fort `members = Yes`; the lodestone page says it is "the only members-only lodestone to be located in the free-to-play part of the world". The lodestone and the fort are in a hole. | CERTAIN |
| City of Um | F2P | `members = No`; listed on the Free-to-play page under Necromancy (7 August 2023). | CERTAIN for the city as a whole. INFERRED that the whole 160x150 box, on all planes, is F2P. |
| Wendlewick | F2P | Wendlewick and the Wendlewick lodestone have `members = No`; Havenhythe was added to F2P on 23 March 2026. | CERTAIN for the town. The box is a GUESS (see below). |

## Doubts, tagged honestly

**CERTAIN: read on the wiki or measured on the bake.**
- No walkable tile of Kandarin, Morytania, the southern desert, Brimhaven or south Karamja, Entrana, Death Plateau, Troll Country, Taverley Dungeon, Lumbridge Swamp Caves, the H.A.M. Hideout, the Edgeville Dungeon members section or the ice-dungeon wyvern chamber falls in zones minus holes. This is measured on the shipped bake.
- All 11 free lodestones are F2P, with a full 5x5 ring around each tile.
- All 15 members lodestones, plus Fort Forinthry, have 0 of 25 ring tiles F2P.

**INFERRED: follows from the wiki plus the bake, without a wiki line that draws the border.**
- **Shantay Pass and the Al Kharid desert edge.** The border is where desert tiles stop being connected to Al Kharid without the gate. The wiki only says south of the pass is members.
- **River Salve and Paterdomus.** The east edges follow the river as baked. The strip x 3417..3426 east of the temple (y 3456..3511) is left out because the wiki does not map where the members gate is.
- **Taverley and White Wolf Mountain west edge.** It follows the Kandarin area. On plane 0 the F2P land reaches x=2852, at the Land of Snow portal.
- **Burthorpe.**
  - The north edge y=3577 drops some walkable land west of the Death Plateau (x 2826..2850, y 3578..3595). I could not confirm that land is F2P.
  - The area west of x=2826 (x 2805..2822, y 3556..3577) is also out.
- **Crandor west strip.** I pulled the edge in by 6 tiles; the wiki shows the members boundary only as an image.
- **Varrock Sewers west section** (area 49672) is left out. Whether it is F2P is a GUESS. The bake reaches it only through a members pipe.
- **Wilderness spots I did not make holes.** These are members content on F2P land, not members land:
  - the Mage Arena minigame enclosure. The page says `members = Yes` but also "Mage Arena was made free-to-play on 9 October 2017".
  - the Dragonkin Laboratory, `members = Yes`. Its entrance tile 3368,3890 is not walkable; the lab is a separate instance.
  - the Daemonheim Dig Site and Het's Oasis activities.
- **Upper planes.** The surface boxes cover planes 0-3. A few unreachable upper-plane areas fall inside, with no transition into them; for example area 12704, plane 2, 30 tiles at the Troll Country edge. I left them alone.
- **Yeti Town is F2P,** although the Land of Snow page says `members = Yes`. Yeti Town's own page says `members = No`, and Myths of the White Lands has been F2P since June 2010.

**GUESS: low confidence; check in game.**
- **Havenhythe / Wendlewick box.** The RS3 wiki draws Havenhythe at display coordinates. The Wendlewick lodestone object is at 4164,3311 on the wiki and the game tile is 3461,1520, so I used an offset of about (-703,-1791).
  - That puts these members places outside the box: the Wendlewick fish farm (~3371,1503), Moonsylvar Wood (~3446,1635) and Heathervein (~3648,1363).
  - It puts the Shrine of Inanna (~3557,1424, F2P) inside.
  - The September 2026 members areas Berylbrook, Fenmoor and Shuruk-Ba have no coordinates on the wiki, and the bake may predate them.
  - The box is a small core, 3400..3600 x 1420..1610. Much F2P Havenhythe is left out, for example Highweald Forest at about 3507,1659.
- **Where the members gate into Morytania is,** beyond Paterdomus.
- **The bake's walkable-area model may disagree with the live server** at fences or gates that the bake treats as open.

## Not boxed (F2P on the wiki, but no usable coordinates or no route in the bake)

- **Stronghold of Security.** `members = No`, but its wiki maps use a local mapID 24 frame, and the bake has no transition at the Barbarian Village entrance (3081,3421).
- **Wizards' Tower basement and the Rune Essence mine.** The bake has no ladder or teleport transition into either.
- **Falador Mole Lair.** `members = No`, but I could not tie it to an area id. Area 48627, entered at 2991,3410, is unidentified and excluded.
- **Runecrafting Guild.** `members = No`, map at mapID 109 coordinates 1697,5469, with no matching area.
- **The new Tutorial Island.** `members = No`, but the coordinates are unknown.

Missing these fails closed: a free player cannot be routed there, which is safe.
