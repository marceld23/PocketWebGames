# PocketWebGames — Architecture

This document describes the **current architecture** of PocketWebGames: how the
firmware, web clients and PC mock fit together, the conventions every game
follows, and the wire protocol. It is a snapshot of the system as it is, not a
build history. For how to add a game, see [Agents.md](Agents.md); for the user
view, see [README.md](README.md).

---

## What it is

An **M5Stack Core2** is a self-contained local-multiplayer game console. It
opens its own Wi-Fi access point, serves a small website, and runs a match
server. Players join with their phones, open `http://192.168.4.1`, and play
together in the same room — no internet, no app install.

The Core2 hosts **several games but runs exactly one at a time**, chosen from a
launcher on its touchscreen. Each game renders in the players' browsers; the
Core2 is the **host console** (launcher, lobby, scoreboard, round timer).

```text
Phone 1        Phone 2        Phone 3        Phone 4
Browser        Browser        Browser        Browser
   |              |              |              |
   +----------- WebSocket over Wi-Fi ----------+
                        |
                  M5Stack Core2
    Wi-Fi AP + HTTP + WebSocket match server + touch launcher/host display
```

---

## Roles

- **Core2 (host):** Wi-Fi SoftAP, HTTP server (static assets + captive portal),
  WebSocket server, match logic for the active game, and the host display
  (launcher carousel, lobby with join QR, live scoreboard/timer).
- **Phone (client):** renders the active game (Canvas2D or Three.js/WebGL),
  reads touch/keyboard input, and exchanges small JSON messages over one
  WebSocket. No phone-to-phone traffic — everything is mediated by the Core2.

---

## Authority model

The split is per concern, and consistent across games:

- **Client-authoritative:** a player's own avatar *pose* (ship position/rotation,
  car position/heading, paddle position). The client predicts it locally and
  reports it; this hides Wi-Fi latency.
- **Server-authoritative:** everything that must be fair and identical for all —
  hits, score, round state, join/leave/respawn, and any shared world state
  (asteroids, bombs/flames, snake bodies, trail grid, ball, laps).
- **Deterministic / replicated world:** the arena is not synced entity-by-entity.
  The dogfight sends a *seed* and both sides generate the same asteroid field;
  the grid games send the level/grid **verbatim** in `init` (no PRNG-parity
  pitfalls). Snake is the exception — it is fully server-authoritative and
  reconstructed from incremental updates over the reliable WebSocket.

A few games are *more* server-authoritative because their core mechanic is
shared state (Trails, Snake) or a single shared object (Bounce ball). See the
per-game table below.

---

## Firmware structure (`src/`, `include/`)

```text
main.cpp          setup(): splash → host.begin() → Net::begin() → UI::begin();
                  loop(): Net::loop(); host.update(now); UI::update(now)
IGame.h           the interface every game implements (+ shared GameState enum)
Host.{h,cpp}      registry of games; "launcher" vs "in-game" mode; routes net
                  events to the active game; launch()/stop()
Net.cpp           Wi-Fi SoftAP, HTTP static serving + captive portal + DNS
                  hijack, WebSocket server, flow-controlled broadcast
UI.cpp            host display: swipe launcher (logos, battery, power-off),
                  delegates in-game screens to the active game, shared draw
                  helpers, touch + hardware-button input
Config.h          all tunables (SSID, ports, per-game constants, timings)
DogfightGame.*    Space Dogfight     (slug: dogfight)
BombermanGame.*   Blaster            (slug: blaster)
CurveFeverGame.*  Trails             (slug: trails)
RacingGame.*      3D Racing          (slug: racing)
SnakeGame.*       Snake Arena        (slug: snake)
PongGame.*        Bounce             (slug: bounce)
```

(Internal class/file names keep their original genre names; the user-facing
name and URL slug are what changed during rebranding.)

### `IGame` interface

```cpp
class IGame {
public:
  // Metadata (launcher + asset routing)
  virtual const char *name() const = 0;   // e.g. "Space Dogfight"
  virtual const char *slug() const = 0;    // e.g. "dogfight" -> /dogfight/

  // Lifecycle
  virtual void begin() = 0;                // on launch
  virtual void end()   = 0;                // on return to launcher
  virtual void update(uint32_t now) = 0;   // ticked only while active

  // Net (forwarded by the Net layer; each game owns its own wire protocol)
  virtual void onClientConnect(AsyncWebSocketClient *c) {}
  virtual void onClientText(AsyncWebSocketClient *c, const String &msg) = 0;
  virtual void onClientDisconnect(AsyncWebSocketClient *c) = 0;

  // Host control (from UI) + display
  virtual void startRound() = 0;
  virtual void resetRound() = 0;
  virtual void togglePause() = 0;
  virtual GameState state() const = 0;     // Lobby | Running | Paused | Ended
  virtual void drawHostScreen() = 0;       // draws this game's host display
  virtual void drawLogo(int cx, int cy, int r) {}  // launcher carousel icon
};
```

### Host (launcher + active game)

```text
Host
├── mode LAUNCHER  -> swipe carousel of registered games on the Core2 display
└── mode IN_GAME   -> exactly one active game (Lobby/Running/Paused/Ended)
```

Games are **statically allocated** (each struct is a few KB) and registered in
`Host::begin()`. `Host::launch(i)` runs the chosen game's `begin()` and switches
to in-game mode; `Host::stop()` runs `end()` and returns to the launcher. Only
the active game is ticked. On every switch the Host calls
`Net::closeAllClients()` so browsers reconnect and fetch the new game's assets.

### Net layer

- **SoftAP** `PocketWebGames` (WPA2, channel 6, ≤6 stations), gateway
  `192.168.4.1`.
- **HTTP:** `serveStatic("/")` from LittleFS. `GET /` is dynamic: in-game it
  **302-redirects to `/<active-slug>/`**, in the launcher it serves a short
  "host is choosing a game" page (meta-refresh). So the player URL is always the
  same `http://192.168.4.1/`.
- **Captive portal:** a DNS server hijacks all names to the AP IP, and unknown
  URLs return a portal page. (Auto-open is reliable on iOS but **not on Android**
  with arduino-esp32 core 3.x — a known upstream regression; players open
  `192.168.4.1` manually there. See "Known constraints".)
- **WebSocket** at `/ws`. `onClientText` is forwarded to `host.active()`. A
  **flow-controlled broadcast** (`broadcastText`) skips any client whose send
  queue is full, so one congested phone can't stall the match.

### UI layer

- **Launcher:** a swipe carousel — one card per game with its `drawLogo()` icon
  and name, page dots, side arrows, hardware-button fallback (A/B/C =
  prev/play/next). A **battery icon** (from the AXP192) and a **power-off**
  button (with confirm dialog → `M5.Axp.PowerOff()`) live in the header.
- **In-game:** the UI delegates the whole screen to `active->drawHostScreen()`.
  The three on-screen buttons are tappable **and** mirror hardware A/B/C:
  **Start / Menu / Pause**, where **Menu returns to the launcher** (`Host::stop()`).
  Games request a redraw via `UI::invalidate()`; shared helpers
  (`drawHeader`, `drawButtons`, `drawJoinPanel`) keep the look consistent.
- **Splash:** a 4 s "PocketWebGames" splash on boot while init runs.

---

## The games

| Game (slug) | Render | Client-authoritative | Server-authoritative | Win (3-min round) |
|---|---|---|---|---|
| **Space Dogfight** (`dogfight`) | 3D / Three.js | ship pose | hits, asteroids, power-ups, score | most kills |
| **Blaster** (`blaster`) | 2D canvas | own position | grid, bombs, flames, bricks, power-ups, deaths | most kills |
| **Trails** (`trails`) | 2D canvas | — (sends steer only) | movement + trail grid, collisions, mini-rounds | most mini-rounds survived |
| **3D Racing** (`racing`) | 3D / Three.js | car pose | track, checkpoints, laps | first to 3 laps (or most by time) |
| **Snake Arena** (`snake`) | 2D canvas | — (sends direction only) | full grid sim (snakes, food) | most food eaten |
| **Bounce** (`bounce`) | 2D canvas | own paddle | ball physics, scoring | highest score (≤4 players, one per edge) |

All six reuse the `GameState` flow, the timed-round host screen, the scoreboard,
and the PC mock. Score broadcasts and round broadcasts share a format (below).

---

## Web clients (`data/`)

```text
data/
  shared/three.min.js.gz     one copy of Three.js, shared by the 3D games
  dogfight/  blaster/  trails/  racing/  snake/  bounce/
      index.html  game.js  style.css
```

`index.html` references its own `game.js`/`style.css` relatively and (for 3D
games) `<script src="/shared/three.min.js">`. Each `game.js` is self-contained:
it opens one WebSocket to `location.host + "/ws"`, shows a name/login overlay,
and on **join** waits for `init` before showing the game. The host pressing
**Start** drives the round; clients render from broadcasts and send input.

Controls are touch-first (virtual joystick / d-pad / drag + action buttons) with
keyboard equivalents for PC testing.

---

## Network protocol

JSON with short keys. A small **common envelope** is shared by all games; each
game adds its own message types.

### Common (all games)

```json
// client -> server
{"t":"join","name":"Marcel"}
{"t":"ping"}                              // keep-alive; server replies {"t":"pong"}

// server -> client
{"t":"score","p":[[1,5,"Marcel"],[2,3,"BOT-6"]]}    // [id, score, name]
{"t":"round","state":"running","remaining":108,"winner":0,"nC":3,"fw":"..."}
{"t":"leave","id":2}
{"t":"err","m":"full"}
```

`state` is one of `lobby | running | paused | ended`. `init` is per-game (it
carries the world the client must build).

### Space Dogfight (representative of the pose-based games)

```json
// init: arena seed + live state for late joiners
{"t":"init","id":1,"seed":12345,"arena":500,"hp":100,"acount":22,
 "dead":[3,7],"adhp":[[5,40]],"pu":[{"id":1,"type":2,"x":..,"y":..,"z":..}]}

// client pose ~15 Hz
{"t":"s","x":..,"y":..,"z":..,"qx":..,"qy":..,"qz":..,"qw":..}
// pose broadcast ~10 Hz (r = respawning flag)
{"t":"sb","p":[{"id":1,"x":..,..,"hp":75,"r":0}, ...]}

{"t":"f","x":..,"y":..,"z":..,"dx":..,"dy":..,"dz":..}   // fire
{"t":"fb",...} {"t":"hit",...} {"t":"kill",...} {"t":"respawn",...}
{"t":"adamage",...} {"t":"adestroy",...} {"t":"pspawn",...} {"t":"ppickup",...}
```

### Distinctive messages of the other games

- **Blaster:** `init` carries the grid as a `cells` string; `s` is `{x,y}`;
  `b` places a bomb; server emits `bomb`, `boom`, `brick`, `pspawn`, `ppickup`.
- **Trails:** client sends only `i` `{d:-1|0|1}` (steer); server emits `clear`
  (new mini-round), `sb` (head positions + gap flag) and `dead`.
- **3D Racing:** `init` carries the track waypoints; `s` is `{x,z,h}`; server
  emits `sb` (car poses + lap) and `lap`.
- **Snake Arena:** `init` is a full snapshot (snake bodies + food); client sends
  `d` `{x,y}` (direction); server emits incremental `step` (new heads + grew
  flag), plus `food`/`eat`/`dead`/`respawn`. State stays in sync because the
  WebSocket is reliable/ordered.
- **Bounce:** `init` gives the player's `edge`; client sends `p` `{v}` (paddle
  position); server emits `sb` (ball + all paddles) and `point`.

---

## Timing & parameters (see `Config.h` for exact values)

- Round length: **3 minutes**; respawn delay ~2 s.
- Player timeout: **8 s** without a recognised message → dropped. (The check
  uses a *signed* `millis()` comparison: the WebSocket task can update a
  player's `lastSeen` slightly after the main loop sampled `now`, and an
  unsigned subtraction would underflow and falsely time everyone out.)
- Broadcast cadence: state ~10 Hz (Trails/Bounce ~30 Hz for smoothness),
  scoreboard ~2 Hz, round ~1 Hz.
- Max players: **6** (Bounce is capped at 4 — one per edge).
- Main loop yields `delay(2)` so AsyncTCP background tasks can run.

---

## Storage & memory (16 MB flash Core2)

- Partition: app **6.25 MB**, OTA slot 6.25 MB, **LittleFS ~3.375 MB**.
- Web assets use **~0.2 MB** of LittleFS (Three.js `.gz` ~160 KB is the bulk and
  is shared) — room for many more games.
- Firmware with all six games: **~1.37 MB (~21 %)** of the app partition;
  RAM ~1.6 %. Each added game cost only ~10–15 KB flash.

---

## PC mock (`mock_server.py`)

A standard-library-only Python server that stands in for the Core2 so the
clients can be developed/tested without hardware. It serves `data/` (with a
`/` → `/<slug>/` redirect and gzip fallback) and re-implements **just enough**
of each game's server protocol, with bot opponents. One game per process,
selected by argument:

```text
uv run mock_server.py [dogfight|blaster|trails|racing|snake|bounce]
```

Each game has a matching `*Sim` class behind a small `slug / on_text /
on_disconnect / tick` shape. It is a test aid, **not** a second source of
truth — the firmware is authoritative. Keep the two in lockstep when changing a
protocol, and keep any PRNG (`mulberry32`) byte-identical to the client.

`tools/screenshot_games.py` drives the mock with headless Chromium to produce
the README screenshots.

---

## Known constraints & non-goals

- **One game at a time** by design (the Core2 is a single host; switching is a
  clean teardown/relaunch).
- **Captive-portal auto-open is unreliable on Android** (arduino-esp32 core 3.x
  regression; HTTPS connectivity probes; the captive mini-browser can't run
  WebGL/WebSockets anyway). The robust path is: scan the Wi-Fi QR to join, then
  open `192.168.4.1`. iOS generally auto-opens.
- **No server-authoritative high-precision physics** — coarse, latency-tolerant
  hit/collision resolution suited to the ESP32 and Wi-Fi.
- **3D on the Core2 itself** is out of scope; phones render, the Core2 hosts.
- Audio/haptics on the Core2 are not implemented (possible future polish).

---

## Adding a game

`class YourGame : public IGame` → register in `Host::begin()` → add
`data/<slug>/` (reuse `data/shared/`) → add a `*Sim` to `mock_server.py`.
`pio run` and `pio run -t buildfs` must both succeed. Full checklist in
[Agents.md](Agents.md).
