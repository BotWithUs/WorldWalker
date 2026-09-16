/* WorldWalker viewer — overlays a wwcli `path` JSON on the mejrs RS3 tile set.
 *
 * Tile source: https://github.com/mejrs/layers_rs3 — PNGs served from raw.
 * githubusercontent. Leaflet CRS.Simple, mapId=0 (overworld), planes 0–3.
 *
 * Coord transform — at native zoom 3 the tiles align 1:1 with game tiles and
 * no y-flip is needed; CRS.Simple stores (lat, lng) ↔ (y, x). If a future
 * tile set is reissued upside-down, flip the sign of `y` in gameToLatLng().
 */

(function ()
{
    const TILE_BASE =
        "https://raw.githubusercontent.com/mejrs/layers_rs3/refs/heads/master";
    // mapId 28 is the RS3 overworld; mapId 0 is a small sub-region. The
    // mejrs viewer (js/main/main_rs3.js) defaults to `initialMapId: 28`.
    const MAP_ID = 28;
    const PLANES = [0, 1, 2, 3];

    // Mejrs viewer bounds (js/layers.js + js/main/main_rs3.js):
    //   maxBounds: [[-1000, -1000], [12800 + 1000, 6400 + 1000]]
    // Under L.CRS.Simple with latLng = (game y, game x), the lat axis carries
    // game y (north-south, the longer dimension up to ~12800) and lng carries
    // game x (east-west, up to ~6400). The earlier draft had these flipped.
    const WORLD_BOUNDS = [[-1000, -1000], [13800, 7400]];
    // Lumbridge spawn — initial recenter target, also a sanity check that the
    // tile alignment is right when the page first loads.
    const INITIAL_CENTER = [3218, 3222];   // (y, x) = (lat, lng)
    const INITIAL_ZOOM = 2;

    const map = L.map("map",
    {
        crs: L.CRS.Simple,
        minZoom: -4,
        maxZoom: 4,
        zoomSnap: 1,
        zoomDelta: 1,
        maxBounds: WORLD_BOUNDS,
        maxBoundsViscosity: 0.5,
        attributionControl: false,
    });

    // mejrs flips Leaflet's tile y in the URL (`y: -(1 + coords.y)`) — the
    // stock L.tileLayer URL-template substitution can't express that, so
    // subclass and override getTileUrl. Source: mejrs.github.io js/layers.js,
    // L.TileLayer.Main.getTileUrl.
    const GameTileLayer = L.TileLayer.extend(
    {
        getTileUrl: function (coords)
        {
            return L.Util.template(this._url,
            {
                mapId: MAP_ID,
                plane: this.options.plane,
                z: coords.z,
                x: coords.x,
                y: -(1 + coords.y),
            });
        },
    });

    function mapLayer(kind, plane)
    {
        const url = `${TILE_BASE}/${kind}/{mapId}/{z}/{plane}_{x}_{y}.png`;
        return new GameTileLayer(url,
        {
            plane,
            tileSize: 256,
            maxNativeZoom: 3,
            maxZoom: 4,
            minNativeZoom: -3,
            noWrap: true,
            bounds: WORLD_BOUNDS,
            updateWhenIdle: false,
        });
    }

    // One pair of layers per plane (base map + icons); only the active plane
    // pair is added to the map at any one time.
    const layers = {};
    for (const p of PLANES)
    {
        layers[p] = { base: mapLayer("map_squares", p), icons: mapLayer("icon_squares", p) };
    }

    let activePlane = 0;
    let iconsOn = false;
    layers[activePlane].base.addTo(map);

    function setPlane(p)
    {
        if (p === activePlane) { return; }
        map.removeLayer(layers[activePlane].base);
        if (iconsOn) { map.removeLayer(layers[activePlane].icons); }
        activePlane = p;
        layers[activePlane].base.addTo(map);
        if (iconsOn) { layers[activePlane].icons.addTo(map); }
        renderPath();
    }

    function setIcons(on)
    {
        if (on === iconsOn) { return; }
        iconsOn = on;
        if (iconsOn) { layers[activePlane].icons.addTo(map); }
        else         { map.removeLayer(layers[activePlane].icons); }
    }

    document.getElementById("plane-row").addEventListener("change", (e) =>
    {
        if (e.target.name === "plane") { setPlane(Number(e.target.value)); }
    });
    document.getElementById("icons-toggle").addEventListener("change", (e) =>
    {
        setIcons(e.target.checked);
    });

    // +0.5 shifts each plotted point from the SW tile corner to the tile
    // centre — matches the mejrs viewer's marker convention
    // (`L.marker([item.y + 0.5, item.x + 0.5], ...)` in js/layers.js). Without
    // this, polylines and markers ride half a tile NW of the actual tile
    // and visually clip into walls/buildings to the north and west.
    function gameToLatLng(x, y)
    {
        return L.latLng(y + 0.5, x + 0.5);
    }

    map.setView(gameToLatLng(INITIAL_CENTER[1], INITIAL_CENTER[0]), INITIAL_ZOOM);

    // ---- Path rendering --------------------------------------------------

    let currentPath = null;
    const pathGroup = L.layerGroup().addTo(map);

    function transitionColor(kind)
    {
        switch (kind)
        {
            case "transport":      return "#fbbf24";   // amber — most common
            case "fairy_ring":     return "#a78bfa";   // violet
            case "teleport_chain": return "#34d399";   // green
            case "spell":          return "#60a5fa";   // blue
            case "lodestone":      return "#f472b6";   // pink
            default:               return "#e5e5e5";
        }
    }

    function clearPath() { pathGroup.clearLayers(); }

    function stepTooltip(step, idx)
    {
        if (step.kind === "walk")
        {
            return `#${idx} walk → (${step.x}, ${step.y}, p${step.plane})`;
        }
        const dest = `(${step.destX}, ${step.destY}, p${step.destPlane})`;
        const kind = step.transitionKind || "transition";
        return `#${idx} ${kind} → ${dest}`;
    }

    function renderPath()
    {
        clearPath();
        const data = currentPath;
        if (!data) { return; }

        // Polyline of (start + every step.target) intersected with the active
        // plane — segments that cross planes get drawn as dashed connectors.
        const segments = [];
        let runOnPlane = [];
        const pushRun = () =>
        {
            if (runOnPlane.length >= 2) { segments.push({ pts: runOnPlane, plane: true }); }
            runOnPlane = [];
        };

        let cursor = { x: data.start.x, y: data.start.y, plane: data.start.plane };
        if (cursor.plane === activePlane) { runOnPlane.push(gameToLatLng(cursor.x, cursor.y)); }

        data.steps.forEach((step, idx) =>
        {
            const onPlane = step.plane === activePlane;
            if (step.kind === "walk")
            {
                if (onPlane && cursor.plane === activePlane)
                {
                    // Prefer the tile-level route (full A* refinement) over a
                    // straight line — without it the polyline cuts through
                    // walls/buildings the planner correctly routed around.
                    if (Array.isArray(step.route) && step.route.length > 0)
                    {
                        for (const t of step.route)
                        {
                            runOnPlane.push(gameToLatLng(t[0], t[1]));
                        }
                    }
                    else
                    {
                        runOnPlane.push(gameToLatLng(step.x, step.y));
                    }
                }
                else if (onPlane && cursor.plane !== activePlane)
                {
                    if (Array.isArray(step.route) && step.route.length > 0)
                    {
                        for (const t of step.route)
                        {
                            runOnPlane.push(gameToLatLng(t[0], t[1]));
                        }
                    }
                    else
                    {
                        runOnPlane.push(gameToLatLng(step.x, step.y));
                    }
                }
                else
                {
                    pushRun();
                }
                cursor = { x: step.x, y: step.y, plane: step.plane };
                return;
            }
            // Transition step. The step.x/y/plane is the interact tile; the
            // dest fields are where the player lands.
            const interactOnPlane = step.plane === activePlane;
            const destOnPlane = step.destPlane === activePlane;

            if (interactOnPlane && cursor.plane === activePlane)
            {
                runOnPlane.push(gameToLatLng(step.x, step.y));
            }

            // Draw the transition's own jump (interact -> dest) as a dashed
            // connector regardless of plane visibility — it's a teleport.
            segments.push(
            {
                pts: [gameToLatLng(step.x, step.y), gameToLatLng(step.destX, step.destY)],
                plane: false,
                dashed: true,
                color: transitionColor(step.transitionKind),
            });

            // After a transition, the cursor jumps to the dest tile.
            cursor = { x: step.destX, y: step.destY, plane: step.destPlane };

            if (!destOnPlane) { pushRun(); }
            else if (destOnPlane && !interactOnPlane)
            {
                // Resume on this plane starting at the dest tile.
                runOnPlane = [gameToLatLng(step.destX, step.destY)];
            }
            else
            {
                runOnPlane.push(gameToLatLng(step.destX, step.destY));
            }
            void idx;
        });
        pushRun();

        // Walk polylines (this-plane runs).
        for (const seg of segments)
        {
            if (seg.plane)
            {
                const line = L.polyline(seg.pts,
                    { color: "#38bdf8", weight: 3, opacity: 0.9 });
                line.addTo(pathGroup);
                L.polylineDecorator(line,
                {
                    patterns: [{
                        offset: 16, repeat: 48,
                        symbol: L.Symbol.arrowHead(
                        { pixelSize: 8, polygon: false, pathOptions: { color: "#38bdf8", weight: 2 } }),
                    }],
                }).addTo(pathGroup);
            }
            else
            {
                L.polyline(seg.pts,
                {
                    color: seg.color || "#fbbf24",
                    weight: 2,
                    opacity: 0.85,
                    dashArray: "6 6",
                }).addTo(pathGroup);
            }
        }

        // Numbered step markers (one circle per step, dimmed when off-plane).
        data.steps.forEach((step, idx) =>
        {
            const onPlane = step.plane === activePlane;
            const color = step.kind === "transition"
                ? transitionColor(step.transitionKind)
                : "#38bdf8";
            const marker = L.circleMarker(gameToLatLng(step.x, step.y),
            {
                radius: step.kind === "transition" ? 7 : 5,
                color,
                fillColor: "#0b1220",
                fillOpacity: onPlane ? 0.95 : 0.25,
                opacity: onPlane ? 1.0 : 0.4,
                weight: step.kind === "transition" ? 3 : 2,
            });
            marker.bindTooltip(stepTooltip(step, idx + 1),
                { direction: "right", offset: [6, 0] });
            marker.addTo(pathGroup);

            // Transition dest indicator on the destination plane.
            if (step.kind === "transition" && step.destPlane === activePlane)
            {
                L.circleMarker(gameToLatLng(step.destX, step.destY),
                {
                    radius: 5,
                    color,
                    fillColor: color,
                    fillOpacity: 0.4,
                    weight: 1,
                    dashArray: "2 2",
                }).bindTooltip(`#${idx + 1} land → (${step.destX}, ${step.destY}, p${step.destPlane})`,
                    { direction: "right", offset: [6, 0] }).addTo(pathGroup);
            }
        });

        // Start / goal pins.
        if (data.start.plane === activePlane)
        {
            L.circleMarker(gameToLatLng(data.start.x, data.start.y),
            {
                radius: 7, color: "#16a34a", fillColor: "#4ade80",
                fillOpacity: 1.0, weight: 2,
            }).bindTooltip(`start (${data.start.x}, ${data.start.y}, p${data.start.plane})`,
                { direction: "right", offset: [8, 0] }).addTo(pathGroup);
        }
        if (data.goal.plane === activePlane)
        {
            L.circleMarker(gameToLatLng(data.goal.x, data.goal.y),
            {
                radius: 7, color: "#b91c1c", fillColor: "#f87171",
                fillOpacity: 1.0, weight: 2,
            }).bindTooltip(`goal (${data.goal.x}, ${data.goal.y}, p${data.goal.plane})`,
                { direction: "right", offset: [8, 0] }).addTo(pathGroup);
        }
    }

    function updateStats(data)
    {
        const stats = document.getElementById("stats");
        if (!data) { stats.innerHTML = "<span class='dim'>no path loaded</span>"; return; }
        const walks = data.steps.filter((s) => s.kind === "walk").length;
        const hops  = data.steps.filter((s) => s.kind === "transition").length;
        const cost  = (typeof data.cost === "number") ? data.cost.toFixed(1) : "?";
        stats.textContent =
            `artifact   ${data.artifact || "?"}\n` +
            `start      (${data.start.x}, ${data.start.y}, p${data.start.plane})\n` +
            `goal       (${data.goal.x}, ${data.goal.y}, p${data.goal.plane})\n` +
            `cost       ${cost}\n` +
            `steps      ${data.steps.length}  (${walks} walk + ${hops} transition)`;
    }

    function loadJsonText(text)
    {
        let data;
        try { data = JSON.parse(text); }
        catch (e) { alert("invalid JSON: " + e.message); return; }
        if (!data.steps || !data.start || !data.goal)
        {
            alert("not a wwcli path JSON (missing steps/start/goal).");
            return;
        }
        currentPath = data;
        updateStats(data);
        // Auto-flip to the start's plane on load so the path is visible.
        const startPlane = data.start.plane;
        if (PLANES.includes(startPlane))
        {
            document.querySelector(`input[name='plane'][value='${startPlane}']`).checked = true;
            setPlane(startPlane);
        }
        // Recenter on the bbox of all steps + endpoints. paddingTopLeft keeps
        // the start clear of the 240px side panel; maxZoom 2 leaves enough
        // surrounding map visible to recognise landmarks (zoom 3 fits so
        // tightly that a multi-tile path fills the viewport with no context).
        const lls = [gameToLatLng(data.start.x, data.start.y),
                     gameToLatLng(data.goal.x,  data.goal.y)];
        for (const s of data.steps) { lls.push(gameToLatLng(s.x, s.y)); }
        map.fitBounds(L.latLngBounds(lls).pad(0.15),
        {
            maxZoom: 2,
            paddingTopLeft: [260, 20],
            paddingBottomRight: [20, 20],
        });
        renderPath();
    }

    document.getElementById("file").addEventListener("change", (e) =>
    {
        const f = e.target.files && e.target.files[0];
        if (!f) { return; }
        f.text().then(loadJsonText);
    });

    // Drag-and-drop anywhere on the map.
    const mapEl = document.getElementById("map");
    mapEl.addEventListener("dragover", (e) => { e.preventDefault(); });
    mapEl.addEventListener("drop", (e) =>
    {
        e.preventDefault();
        const f = e.dataTransfer.files && e.dataTransfer.files[0];
        if (!f) { return; }
        f.text().then(loadJsonText);
    });

    // Live coordinate readout in game-tile space.
    const readout = document.getElementById("coord");
    map.on("mousemove", (e) =>
    {
        const x = Math.floor(e.latlng.lng);
        const y = Math.floor(e.latlng.lat);
        readout.textContent = `(${x}, ${y}, p${activePlane})`;
    });
})();
