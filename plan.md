# PocketWebGames - Implementation Plan

A phased plan for building a local-multiplayer browser-game console hosted by
an M5Stack Core2. The Core2 acts as Wi-Fi AP, web server, match server, and
launcher; phones/tablets render each game in their browser. It began as a
single 3D dogfight (phases 1–7), then grew a multi-game host (phase 8) and
several more games (phases 9–10).

---

## Goals and non-goals

### Goals

- Local multiplayer dogfight for **2-4 players** (target), up to **6 players**
  (stretch).
- No internet required. Core2 is its own Wi-Fi access point.
- 3D rendering happens entirely in the browser on each tablet/phone.
- Core2 hosts the website, runs the match server, and shows a scoreboard.
- Arcade-style flight model (easy to fly on touch).

### Non-goals (for now)

- High-precision shooter netcode.
- Full 6DoF flight model with realistic inertia.
- Server-authoritative physics for every entity.
- Rendering the 3D game on the Core2 display.
- Hosting very large textures or models directly from the Core2.

---

## Architecture

```text
M5Stack Core2
- SoftAP    "SpaceDogfight"
- HTTP      serves /index.html, /game.js, /three.min.js, /assets/*
- WebSocket /ws  game protocol
- Game loop tick at ~10-15 Hz
- Display   lobby, score, round timer, controls (Start / Reset / Pause)

Browser (per player)
- Three.js scene, starfield, ship meshes, lasers, asteroids
- Touch input (left joystick: steer, right buttons: fire / boost)
- Local prediction of own ship
- Sends own state ~15 Hz
- Receives broadcast of other ships at ~10-15 Hz
- Plays sounds locally
```

### Authority split

- **Client authoritative**: own ship position, rotation, velocity.
- **Server authoritative**: HP, score, hit validation (coarse), round state,
  player join/leave, respawn.
- **Deterministic**: arena is generated from a seed sent by the server, so
  asteroids and stars are not synced as entities.

---

## Phased roadmap

### Phase 1 - Wi-Fi AP and static web page

**Goal:** Core2 opens its own Wi-Fi, serves a static page over HTTP.

Tasks:

- Configure SoftAP (`SSID = SpaceDogfight`, `password = dogfight123`,
  channel 6, max 6 clients).
- Mount `LittleFS`, serve `/index.html`, `/style.css`, a stub `/game.js`.
- Show on the Core2 display:
  ```text
  SPACE DOGFIGHT
  WLAN: SpaceDogfight
  IP:   192.168.4.1
  ```
- Verify on a phone that the page loads.

Acceptance: phone connects, browser shows "Space Dogfight Host online".

### Phase 2 - WebSocket connection and lobby

**Goal:** Browsers connect via WebSocket, Core2 tracks players.

Tasks:

- Add `/ws` endpoint with `ESPAsyncWebServer`.
- Define minimal protocol: `join`, `init`, `leave`, `ping`.
- Maintain a player list on the Core2 (id, name, connected, score).
- Display player count and names on Core2 screen.
- Add Start / Reset touch buttons on the Core2 display.

Acceptance: opening the page registers the player on the Core2 display,
closing the tab removes them.

### Phase 3 - 3D test scene in the browser

**Goal:** Single-player local rendering only, no networked state.

Tasks:

- Load `Three.js` from LittleFS (gzipped).
- Build a starfield + a single ship mesh (cone or simple model).
- Touch input: left half = virtual joystick for yaw/pitch, right half =
  fire and boost buttons.
- Arcade flight: forward velocity is constant, joystick rotates the ship.
- FPS overlay for debugging.

Acceptance: one tablet, smooth flight, no multiplayer yet.

### Phase 4 - Multiple ships visible

**Goal:** Each client sees the other clients' ships move.

Tasks:

- Client sends own state at ~15 Hz:
  ```json
  {"t":"s","id":1,"x":1.2,"y":0.4,"z":-9.1,
   "qx":0,"qy":0.2,"qz":0,"qw":0.98}
  ```
- Server broadcasts the latest known state of all players at ~10 Hz.
- Clients render remote ships from the broadcast, with simple interpolation.
- Server sends `init` with arena seed on join; all clients build the same
  asteroid field deterministically.

Acceptance: two tablets connected, each sees the other ship moving.

### Phase 5 - Shooting

**Goal:** Players can fire lasers; other players see the shots.

Tasks:

- Right button on the touch UI fires.
- Client sends:
  ```json
  {"t":"f","id":1,"x":1.2,"y":0.4,"z":-9.1,"dx":0,"dy":0,"dz":1}
  ```
- Server rebroadcasts to all other clients.
- Each client renders the laser locally (line or fast projectile).
- Cooldown enforced client-side (server may validate later).

Acceptance: pressing fire on one tablet draws a laser visible on every
other connected tablet.

### Phase 6 - Hits, HP, and score

**Goal:** Server resolves hits and tracks the scoreboard.

Tasks:

- Coarse hit check on the server: when a `fire` arrives, sample the ray
  against the last known sphere positions of other players.
- On hit: reduce target HP, send:
  ```json
  {"t":"hit","a":1,"target":2,"hp":70}
  ```
- On kill: increase shooter's score, respawn target at random position
  after 2 seconds.
- Broadcast scoreboard:
  ```json
  {"t":"score","p":[[1,5],[2,3],[3,1]]}
  ```
- Core2 display shows live scoreboard.

Acceptance: hitting another player decreases their HP; kills bump the
score on every screen and on the Core2 display.

### Phase 7 - Round flow, polish, audio

**Goal:** A round feels like a full game.

Tasks:

- 3-minute round timer driven by the server.
- Pre-game lobby on Core2: shows joined players, host presses **Start**.
- End-of-round screen on the Core2 with winner.
- Sound effects in the browser: shoot, hit, explosion.
- Core2 plays a short sound and vibrates on each hit notification.
- Captive portal (later): auto-open the page when joining the Wi-Fi.

Acceptance: full game loop - lobby -> round -> end screen -> new round.

### Phase 8 - Multi-game host (launcher)

**Goal:** The Core2 becomes a host for **several games**, of which exactly
**one runs at a time**. On boot it shows a launcher menu; the host picks a
game on the touchscreen, that game launches and shows its lobby + join QR.
A "Menu" affordance returns to the launcher.

This phase introduces no new gameplay. It is a structural refactor so that
the existing dogfight becomes the *first* game behind a small abstraction,
and new games can be added later as self-contained modules.

#### Top-level model

```text
Host
├── mode LAUNCHER  -> touch list of registered games on the Core2 display
└── mode IN_GAME   -> exactly one active game (its existing
                      Lobby / Running / Paused / Ended flow)
```

- Boot -> LAUNCHER. Tap a game -> `Host::launch(i)` -> mode IN_GAME, the
  active game's `begin()` runs, its lobby (with QR) is shown.
- A "Menu" touch zone in the game's lobby calls `Host::stop()` -> active
  game's `end()` runs -> back to LAUNCHER.
- Only the active game is ticked. Game objects are statically allocated
  (each is a few KB); no heap churn on switching.

#### `IGame` interface (new `include/IGame.h`)

Every game implements one interface so Host / Net / UI stay generic:

```cpp
class IGame {
public:
  // Metadata for launcher + asset routing
  virtual const char *name() const = 0;   // "Space Dogfight"
  virtual const char *slug() const = 0;    // "dogfight" -> /dogfight/, assets

  // Lifecycle
  virtual void begin() = 0;                // on launch
  virtual void end()   = 0;                // on return to menu
  virtual void update(uint32_t now) = 0;

  // Net (forwarded by the Net layer; each game owns its own protocol)
  virtual void onClientText(AsyncWebSocketClient *c, const String &msg) = 0;
  virtual void onClientDisconnect(AsyncWebSocketClient *c) = 0;

  // Host control + display
  virtual void startRound() = 0;
  virtual void resetRound() = 0;
  virtual void togglePause() = 0;
  virtual GameState state() const = 0;
  virtual void drawHostScreen() = 0;       // each game draws its in-game host UI
};
```

Today's `Game` becomes `DogfightGame : public IGame`; the gameplay code
moves nearly unchanged behind these entry points.

#### New / changed files

- **New** `include/IGame.h`, `src/Host.{h,cpp}` (registry of `IGame*`,
  active pointer, host mode, touch-list navigation), `src/DogfightGame.{h,cpp}`
  (current `Game` logic behind the interface).
- `src/Net.cpp`: forward WS connect/text/disconnect to `host.active()`
  (guard null in LAUNCHER mode); redirect `/` to the active game's folder;
  serve `/shared/`.
- `src/UI.cpp`: add `drawLauncher()` (touch list); route redraw by host
  mode; delegate in-game screens to `active->drawHostScreen()`.
- `src/main.cpp`: drive `host.update(now)` instead of `game.update(now)`.

#### Asset layout (shared Three.js)

```text
data/
  shared/
    three.min.js.gz        # one copy, used by all games (~160 KB)
  dogfight/
    index.html  game.js  style.css
  <future-game>/
    index.html  game.js  ...
```

`index.html` references `<script src="/shared/three.min.js">`. Storage is a
non-issue: ~210 KB used of 3.375 MB LittleFS, Three.js shared across games.

#### URL & QR flow

- Player URL stays constant: `http://192.168.4.1/`.
- The server **redirects `/` to the active game's folder** (set by the
  launcher). In LAUNCHER mode, `/` and the captive portal show a short
  "Host is choosing a game..." page.
- Players always scan the same Wi-Fi-join QR; whatever game the host
  launched is what loads. The launched game's lobby shows that QR.

#### Game switching

When the host stops a game and launches another, connected browsers must
reload. On switch, close all WebSockets (or broadcast `{"t":"reload"}`);
clients reconnect and fetch the new root.

#### Navigation (decided)

Touch list on the Core2 screen: each registered game is a tappable row /
tile; tap launches it. (Hardware buttons remain for in-game Start / Reset /
Pause as today.)

#### Scope (decided)

This phase ships the **framework + dogfight as the only game**. A second
demo game is deliberately deferred - the value here is the clean seam, not
a second title.

#### Mock server

Update `mock_server.py` to mirror the layout: serve `data/shared/` and
`data/dogfight/`, redirect `/` to `/dogfight/`. The PC mock only ever needs
to host one game, so a simple constant redirect is enough.

Acceptance: power on -> launcher list appears; tap "Space Dogfight" -> its
lobby with join QR appears and players can join and play exactly as before;
a Menu tap returns to the launcher; only one game is ever active.

**Status: implemented.** `IGame` + `Host` + the touch launcher are in place,
the dogfight runs behind `DogfightGame : IGame`, assets are split into
`data/shared/` + `data/dogfight/`, and `mock_server.py` mirrors the layout.
Firmware and LittleFS image both build (`pio run`, `pio run -t buildfs`).
Not yet verified on real hardware - the touch list / MENU chip need a
hands-on check on the Core2.

### Phase 9 - Second game: Blaster (grid bomber)

**Goal:** Prove the multi-game seam with a second, very different game.

A tile-grid **timed deathmatch** with bombs: walls + destructible
bricks, bombs that explode in a cross after a fuse (with chain reactions),
power-ups hidden under bricks (extra bomb / longer flame / speed), respawn on
death, +1 score per kill, 3-minute round, highest score wins (reusing the
same Lobby/Running/Paused/Ended flow and host screen layout).

- Server-authoritative: grid, bombs, flames, bricks, power-ups, deaths, score.
  Client-authoritative: own movement (predicted against the synced grid).
- The grid is sent verbatim in `init` (`cells` string), so no PRNG parity is
  required - simpler and less bug-prone than the dogfight's seed approach.
- Rendering is a **2D canvas** (no Three.js); controls are a touch joystick +
  a BOMB button, or WASD/arrows + Space on a PC.
- Files: `include/BombermanGame.h`, `src/BombermanGame.cpp` (internal class
  names kept), `data/blaster/*`, slug `blaster`; registered in `Host::begin()`;
  `BombermanSim` added to `mock_server.py` (`uv run mock_server.py blaster`).

**Status: implemented.** Firmware + LittleFS build; the PC mock plays with
bots. The second game added only ~12 KB flash / ~2 KB RAM on top of the
dogfight, confirming there is room for many more. Hardware play-test pending.

Acceptance: launcher lists Dogfight + Blaster; tapping Blaster shows its
lobby + join QR; players move, drop bombs, blow up bricks/each other, collect
power-ups, and the scoreboard/timer run on the Core2 host screen.

### Phase 10 - More games

**Goal:** Fill out the console with games that exercise different parts of the
architecture. All reuse the `IGame` seam, the timed-round host screen, and the
PC mock (`uv run mock_server.py <slug>`).

- **Trails** (`trails`) — steer-only light-trails on a coarse
  occupancy grid; last-survivor mini-rounds inside the timed match. Server
  integrates movement and resolves collisions; client sends only steer input.
- **3D Racing** (`racing`) — lap racing on a fixed track sent in `init`;
  client-authoritative arcade car (Three.js), server counts checkpoints/laps,
  first to 3 laps wins.
- **Snake Arena** (`snake`) — fully server-authoritative grid snake stepping
  at a fixed rate; reconstructed client-side from incremental `step` messages
  over the reliable WebSocket. Score = food eaten.
- **Bounce** (`bounce`) — server-authoritative ball on a unit square; up to four
  players each own one edge (unused edges are walls); last hitter scores on a
  miss. Client owns its paddle position.

**Status: implemented.** All six games build (firmware + LittleFS) and play
against bots in the PC mock. Total firmware ~1.37 MB / 21% of the app
partition. Hardware play-test still pending.

Also rebranded the project to **PocketWebGames** (AP SSID, captive portal,
titles, comments). The dogfight keeps its own game name "Space Dogfight".

---

## Network protocol (MVP)

Short field names to keep JSON small.

### Join (client -> server)

```json
{"t":"join","name":"Marcel"}
```

### Init (server -> client)

```json
{"t":"init","id":1,"seed":12345,"arena":500}
```

### Ship state (client -> server, ~15 Hz)

```json
{"t":"s","id":1,"x":1.2,"y":0.4,"z":-9.1,
 "qx":0,"qy":0.2,"qz":0,"qw":0.98}
```

### Ship state broadcast (server -> clients, ~10 Hz)

```json
{"t":"sb","p":[
  {"id":1,"x":1.2,"y":0.4,"z":-9.1,"qx":0,"qy":0.2,"qz":0,"qw":0.98},
  {"id":2,"x":-3.0,"y":1.0,"z":4.5,"qx":0,"qy":0,"qz":0,"qw":1}
]}
```

### Fire (client -> server, rebroadcast to all)

```json
{"t":"f","id":1,"x":1.2,"y":0.4,"z":-9.1,"dx":0,"dy":0,"dz":1}
```

### Hit (server -> clients)

```json
{"t":"hit","a":1,"target":2,"hp":70}
```

### Score (server -> clients)

```json
{"t":"score","p":[[1,5],[2,3],[3,1]]}
```

### Round (server -> clients)

```json
{"t":"round","state":"running","remaining":108}
```

Later optimization: switch to binary WebSocket frames
(`[type:u8][id:u8][x,y,z,qx,qy,qz,qw : f32]`). Not in the MVP.

---

## Network parameters

MVP defaults:

```text
max_players          = 4
server_tick          = 10 Hz
client_send_rate     = 15 Hz
state_broadcast_rate = 10 Hz
```

After stability is confirmed:

```text
max_players          = 6
server_tick          = 15 Hz
state_broadcast_rate = 15 Hz
```

---

## 3D models

The Core2 has 16 MB Flash and a relatively slow Wi-Fi link, so any 3D
asset has to be cheap to **store, transfer, and render**. The browser
does the rendering, but the Core2 hosts the bytes.

### Constraints

- Total web payload (HTML + JS + Three.js + models + textures) should
  comfortably fit in LittleFS alongside firmware - aim for **< 4 MB**
  total, ideally **< 1 MB**. Bigger payloads should move to microSD.
- First page load happens over the Core2's SoftAP; keep the cold-start
  download small so up to 6 tablets can fetch in parallel.
- Each ship is rendered on a phone GPU - prefer **a few hundred
  triangles**, not thousands. No PBR, no normal maps.
- Lasers, asteroids, and stars are generated procedurally; they are not
  3D model assets.

### MVP renderer (phase 3-6)

- Wireframe / vector-style ships drawn from a tiny embedded vertex/edge
  list inside `game.js`. No external model files needed.
- Looks intentional (retro Asteroids / Elite aesthetic), avoids any
  asset pipeline question for the MVP.

### Upgrade path (phase 7+)

Once the MVP is playable, we'll upgrade to small triangle-mesh ships.
**Research will happen at that point** - pick exact tools/sources then,
not now. Direction to aim for:

- **Format**: gzipped `.glb` (binary glTF). One file per ship, ideally
  **<= 30 KB each** after gzip.
- **Geometry budget**: ~200-500 triangles per ship, single material,
  flat or vertex-color shading.
- **Loader**: Three.js `GLTFLoader` (only the modules actually used,
  tree-shaken to keep `three.min.js.gz` small).
- **Sources to research when we get there**:
  - Kenney "Space Kit" (CC0) - lots of low-poly ships, trivial license.
  - Quaternius "Ultimate Space Kit" (CC0).
  - Poly Pizza / Sketchfab CC0 filter - case-by-case.
  - Hand-modeled in Blender if we want a unique look (export glTF 2.0,
    apply Draco compression).
- **Pipeline to evaluate when we get there**:
  - `gltf-pipeline` for Draco compression.
  - `gltfpack` (meshoptimizer) for vertex/index quantization.
  - Strip unused channels (UVs, second tangent set) before shipping.

This is intentionally not implemented yet - the MVP ships with
embedded wireframes so the game is playable end-to-end before we spend
time on art assets.

## Asset hosting strategy

- **MVP**: everything on LittleFS, served gzip-precompressed
  (`index.html.gz`, `game.js.gz`, `three.min.js.gz`).
- **Later**: move large assets to microSD; keep small UI files in LittleFS.
- Avoid syncing background/decoration entities over the wire - generate
  them deterministically from the seed sent in `init`.

---

## Core2 display states

### Launcher (Phase 8)

```text
+--------------------+
| SELECT A GAME      |
|                    |
| > Space Dogfight   |  <- tap a row to launch
|   (more games...)  |
|                    |
|                    |
+--------------------+
```

### Lobby

```text
+--------------------+
| SPACE DOGFIGHT     |
| WLAN active        |
| SSID: Dogfight     |
| IP:   192.168.4.1  |
|                    |
| Players: 0/4       |
|                    |
| [Start] [Reset]    |
+--------------------+
```

### In-game

```text
+--------------------+
| Round: 01:48       |
|                    |
| Justus       7     |
| Marcel       4     |
| Guest        2     |
|                    |
| [Pause] [Reset]    |
+--------------------+
```

### End of round

```text
+--------------------+
| Winner: Justus     |
| Score:  12         |
|                    |
| [New Round]        |
+--------------------+
```

---

## Risks and feasibility

| Topic                                         | Assessment             |
| --------------------------------------------- | ---------------------- |
| Wi-Fi SoftAP                                  | Very good              |
| HTTP server                                   | Good                   |
| WebSocket multiplayer                         | Good                   |
| 2-4 players                                   | Good                   |
| 6 players                                     | Possible, must test    |
| Hosting Three.js from Core2                   | OK with gzip / SD card |
| 3D rendering on tablets                       | Very good              |
| 3D rendering on Core2 itself                  | No                     |
| Core2 as host display                         | Very good              |
| Core2 sound + vibration on game events        | Very good              |
| Server-authoritative high-precision shooting  | Not realistic on Core2 |

---

## Definition of done (MVP)

- Power on the Core2, it opens `SpaceDogfight` Wi-Fi.
- Two to four tablets join, open `http://192.168.4.1`, see the lobby.
- Host presses **Start** on the Core2.
- All tablets show the same 3D arena, each renders its own ship and
  the other players' ships.
- Shooting, hits, HP, score work end-to-end.
- Core2 displays the live scoreboard and round timer.
- A 3-minute round runs to completion and a winner is shown.
