# WorldWalker path viewer

Standalone Leaflet page that overlays a `wwcli path` JSON on the public RS3
map tiles from
[`mejrs/layers_rs3`](https://github.com/mejrs/layers_rs3) (the same tile set
behind <https://mejrs.github.io/rs3>). No build step — open `index.html` in a
browser, drop a `path.json` on the map, or pick one with the file input.

## Generating a path

```
wwbuild ... build\test.wwa                                       (one-time)
build\Debug\wwcli.exe path build\test.wwa ^
    3222 3218 0   3210 3424 0   --out lumby.json
```

Coords are game tiles: `fromX fromY fromPlane toX toY toPlane`. The
subcommand uses a maximally permissive capability snapshot (every
requirement-gated transition admitted), so the plan matches what the executor
would run for a fully-equipped player.

## Controls

- **Plane radios** — switch the rendered floor. The path is drawn only on the
  active plane; transitions to other planes appear as dashed jumps.
- **Icons overlay** — toggles the in-game `icon_squares` layer.
- **Drop / browse** — load a different path JSON without reloading the page.

The bottom-right readout shows the game tile under the cursor (`(x, y, p)`),
handy for picking new `from`/`to` coords to feed back into `wwcli path`.

## Tile source caveat

PNGs are fetched live from `raw.githubusercontent.com` — internet required.
The tile set rebuilds on a cache snapshot newer than ours; some scenery may
not match the baked artifact exactly. Verify path correctness against the
artifact's own collision (via `wwcli crosscheck`), not the visual.
