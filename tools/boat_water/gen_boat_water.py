"""Generate src/runtime/BoatWaterRuns.inc: the tiles a rowing boat can cross.

Lake Mnemosyne, round the City of Um, is rowed in Ed's boat during The Spirit of
War. The cache marks none of it as water: its tiles carry no water overlay, and
their clip word is a plain CLIP_BLOCKED, the same as rock or a building. What
does set the lake apart is its terrain: on plane 1 the open water is underlay 1
with no overlay, while the city's ground uses other underlays. The islands in
the lake are underlay 1 on plane 1 as well; what tells them apart is plane 0
below: under open water lies the lake's surface overlay (112, invisible and not
flagged as water), under an island real ground (live 2026-11-21 the boat stuck
at 1098,1846 against the island north-east of the city, where plane 0 turns
from overlay 112 to ground). So a tile is taken as lake when plane 1 is
underlay 1 with no overlay and no object, and plane 0 carries the surface
overlay. Neither the decoded clip nor the bake's says this: the decoder blocks
some open water by its render rule (1095,1840, which the boat crossed), and the
bake's void fence (src/build/TerrainFloor.cpp) blocks all of it.

The same terrain also lies in strips between the city's buildings, which join
the lake and are no water a boat crosses, so only open water is kept: the
tiles at the centre of an all-lake square of side 2 * OPEN_RADIUS + 1, grown
back by OPEN_RADIUS within the lake tiles (a morphological opening), which
keeps the shore and drops every channel narrower than the square. The lake is
the 4-connected set of those tiles reached from a seed the boat was seen on
live, clipped to a box round the four soul spots so the unrendered ground
beyond the lake is left out. Each row of it is written as runs:

    { plane, y, firstX, lastX },

sorted by plane, y and firstX, for the runtime lookup (src/runtime/BoatWater.cpp).

    python tools/boat_water/gen_boat_water.py <NXTCache.dll> <cache dir>

Re-run after a game update that changes the lake.
"""

import ctypes
import pathlib
import sys

MAP_INDEX = 5
TERRAIN_FILE = 3
SQUARE = 64
PLANES = 4

CLIP_OBJECT = 0x100
# Half the side of the all-water square open water is kept by. The city's
# strips run 3 to 6 tiles wide; the open lake is far wider.
OPEN_RADIUS = 4

LAKES = [
    {
        'name': 'Lake Mnemosyne',
        'plane': 1,
        'underlay': 1,
        # The overlay plane 0 carries under the lake's open water.
        'surface_overlay': 112,
        'box': (975, 1670, 1220, 1910),
        # Read live 2026-11-21 on the boat (the gear log showed the Rowboat
        # worn): 1023,1882 on the way to the north-west soul spot.
        'seed': (1023, 1882),
    },
]


def open_cache(dll_path, cache_dir):
    lib = ctypes.CDLL(dll_path)
    lib.nxt_cache_open_local.restype = ctypes.c_void_p
    lib.nxt_cache_open_local.argtypes = [ctypes.c_char_p]
    lib.nxt_read_file_raw.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                      ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                                      ctypes.POINTER(ctypes.c_size_t)]
    lib.nxt_get_mapsquare_clip.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                           ctypes.POINTER(ctypes.POINTER(ctypes.c_uint32)),
                                           ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_uint8)]
    lib.nxt_free.argtypes = [ctypes.c_void_p]
    lib.nxt_last_error.restype = ctypes.c_char_p
    cache = lib.nxt_cache_open_local(cache_dir.encode())
    if not cache:
        raise SystemExit(f'cannot open cache {cache_dir}: {lib.nxt_last_error()}')
    return lib, cache


def read_smart(data, i):
    if data[i] < 128:
        return data[i], i + 1
    return ((data[i] << 8) | data[i + 1]) - 32768, i + 2


def decode_terrain(data):
    """The overlay and underlay ids per [plane][x][y], decoded as MapSquare.cpp decodeMapTerrain does."""
    overlays = [[[0] * SQUARE for _ in range(SQUARE)] for _ in range(PLANES)]
    underlays = [[[0] * SQUARE for _ in range(SQUARE)] for _ in range(PLANES)]
    i = 5
    for plane in range(PLANES):
        for x in range(SQUARE):
            for y in range(SQUARE):
                if i >= len(data):
                    return overlays, underlays
                flags = data[i]
                i += 1
                if flags & 0x1:
                    i += 1
                    overlays[plane][x][y], i = read_smart(data, i)
                if flags & 0x2:
                    i += 1
                if flags & 0x4:
                    underlays[plane][x][y], i = read_smart(data, i)
                if flags & 0x8:
                    underlays[plane][x][y] = (data[i] << 8) | data[i + 1]
                    i += 2
    return overlays, underlays


class Squares:
    """Terrain and clip words per map square, read once each."""

    def __init__(self, lib, cache):
        self.lib = lib
        self.cache = cache
        self.terrain = {}
        self.clip = {}

    def load(self, sx, sy):
        key = (sx, sy)
        if key in self.terrain:
            return
        data = ctypes.POINTER(ctypes.c_uint8)()
        size = ctypes.c_size_t()
        archive = (sx & 0x7F) | (sy << 7)
        if self.lib.nxt_read_file_raw(self.cache, MAP_INDEX, archive, TERRAIN_FILE,
                                      ctypes.byref(data), ctypes.byref(size)) == 0:
            self.terrain[key] = decode_terrain(bytes(data[:size.value]))
            self.lib.nxt_free(data)
        else:
            self.terrain[key] = None
        words = ctypes.POINTER(ctypes.c_uint32)()
        count = ctypes.c_size_t()
        mask = ctypes.c_uint8()
        if self.lib.nxt_get_mapsquare_clip(self.cache, sx, sy, ctypes.byref(words), ctypes.byref(count),
                                           ctypes.byref(mask)) == 0:
            self.clip[key] = list(words[:count.value])
            self.lib.nxt_free(words)
        else:
            self.clip[key] = None

    def is_lake(self, x, y, plane, underlay, surface_overlay):
        sx, sy = x // SQUARE, y // SQUARE
        self.load(sx, sy)
        terrain = self.terrain[(sx, sy)]
        clip = self.clip[(sx, sy)]
        if terrain is None or clip is None:
            return False
        lx, ly = x % SQUARE, y % SQUARE
        overlays, underlays = terrain
        word = clip[(plane * SQUARE + lx) * SQUARE + ly]
        return (underlays[plane][lx][ly] == underlay and overlays[plane][lx][ly] == 0
                and overlays[0][lx][ly] == surface_overlay and (word & CLIP_OBJECT) == 0)


def lake_tiles(squares, lake):
    """Every lake-terrain tile in the box."""
    x0, y0, x1, y1 = lake['box']
    return {(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)
            if squares.is_lake(x, y, lake['plane'], lake['underlay'], lake['surface_overlay'])}


def open_water(tiles):
    """The opening of the tile set by a square of radius OPEN_RADIUS: eroded, then grown back inside the set."""
    r = OPEN_RADIUS
    core = {(x, y) for x, y in tiles
            if all((x + dx, y + dy) in tiles for dx in range(-r, r + 1) for dy in range(-r, r + 1))}
    return {(x + dx, y + dy) for x, y in core for dx in range(-r, r + 1) for dy in range(-r, r + 1)
            if (x + dx, y + dy) in tiles}


def flood(tiles, seed, name):
    """The 4-connected part of the tile set holding the seed."""
    if seed not in tiles:
        raise SystemExit(f'{name}: seed {seed} is not open lake water')
    seen = {seed}
    stack = [seed]
    while stack:
        x, y = stack.pop()
        for n in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if n in tiles and n not in seen:
                seen.add(n)
                stack.append(n)
    return seen


def runs_of(tiles, plane):
    rows = {}
    for x, y in tiles:
        rows.setdefault(y, []).append(x)
    out = []
    for y in sorted(rows):
        xs = sorted(rows[y])
        start = prev = xs[0]
        for x in xs[1:]:
            if x != prev + 1:
                out.append((plane, y, start, prev))
                start = x
            prev = x
        out.append((plane, y, start, prev))
    return out


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    lib, cache = open_cache(sys.argv[1], sys.argv[2])
    squares = Squares(lib, cache)
    runs = []
    for lake in LAKES:
        tiles = flood(open_water(lake_tiles(squares, lake)), lake['seed'], lake['name'])
        lake_runs = runs_of(tiles, lake['plane'])
        print(f"{lake['name']}: {len(tiles)} tiles in {len(lake_runs)} runs")
        runs.extend(lake_runs)
    runs.sort()
    out = pathlib.Path(__file__).resolve().parents[2] / 'src' / 'runtime' / 'BoatWaterRuns.inc'
    lines = ['// GENERATED by tools/boat_water/gen_boat_water.py -- do not edit by hand.',
             '// { plane, y, firstX, lastX } for every row of rowable lake water,',
             '// sorted by plane, y and firstX. See BoatWater.cpp.']
    lines += [f'{{ {p}, {y}, {a}, {b} }},' for p, y, a, b in runs]
    out.write_text('\n'.join(lines) + '\n', encoding='ascii', newline='\n')
    print(f'{len(runs)} runs -> {out}')


if __name__ == '__main__':
    main()
