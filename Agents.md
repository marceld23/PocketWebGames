# Agents.md

Guidance for AI coding agents (Claude Code, Cursor, etc.) working on the
**PocketWebGames** repo. Read this before making changes.

(The on-disk folder is still named `SpaceFight3d` — only the project identity
was renamed. Renaming the folder is optional and left to the human.)

## What this project is

A local-multiplayer **game console**: an **M5Stack Core2** is the Wi-Fi access
point, web server, WebSocket match server, and host display, running several
browser games (one at a time, chosen from an on-device launcher). Players use
phones/tablets; each game renders in their browser. Current games: Space
Dogfight, Blaster, Trails, 3D Racing, Snake Arena, Bounce.

See [README.md](README.md) for the overview and [plan.md](plan.md) for the
phased roadmap and protocol.

## Hardware target

- M5Stack Core2 (ESP32-D0WDQ6-V3, 16 MB Flash, 8 MB PSRAM, touch display,
  speaker, vibration motor, IMU, microSD slot).
- Framework: PlatformIO + Arduino. `platformio.ini` is already configured
  for `board = m5stack-core2`. Do **not** switch frameworks (no ESP-IDF
  refactor) without a discussion.

## Golden rules

1. **The Core2 does not render the 3D game.** Rendering happens in the
   browser. Do not propose moving Three.js logic onto the device.
2. **Authority split:** clients are authoritative for their own ship
   position/rotation; the server is authoritative for HP, score, hit
   resolution, round state, join/leave.
3. **No internet required.** All assets are served from the Core2
   (LittleFS or microSD). Do not pull JS from a CDN at runtime.
4. **Wire format stays small.** Use short JSON keys (`t`, `id`, `x`...),
   not verbose ones (`type`, `playerId`, `positionX`). Binary frames are a
   future optimization, not for the MVP.
5. **Don't sync decoration.** Asteroids, stars, particles, sounds, HUD
   are local. The arena is deterministic from a seed sent in `init`.
6. **Arcade flight, not full 6DoF.** Forward velocity is constant,
   joystick rotates the ship. Easy on a touchscreen.

## Coding conventions

### Firmware (C++ / Arduino)

- Stay in `src/` and `include/`. Place reusable modules in `lib/`.
- Use the M5Core2 library for display/touch/speaker/vibration.
- Use `ESPAsyncWebServer` + `AsyncTCP` for HTTP and WebSocket.
- Use `ArduinoJson` for message parsing; allocate documents with explicit
  capacity, do not rely on dynamic strings in tight loops.
- Avoid blocking calls inside the game loop. No `delay()` in the loop -
  use millis-based scheduling.
- Keep the game tick at ~10-15 Hz; do not push higher without measuring.
- Pre-compress web assets (`gzip`) when feasible; serve `.gz` directly
  via the `AsyncWebServer` `setDefaultFile`/`ContentEncoding` path.
- Don't enable Wi-Fi power save while running matches.

### Browser side (HTML / JS)

- Vanilla JS or Three.js. No build step in the MVP - keep it loadable
  directly from LittleFS.
- Load Three.js locally, not from a CDN.
- One WebSocket per client to `ws://192.168.4.1/ws`.
- Implement client-side prediction for the local ship; interpolate
  remote ships between received states.
- Touch UI: left half = joystick for yaw/pitch, right half = fire and
  boost buttons.

### File layout

```text
src/                C++ firmware (Core2)
  Host.{h,cpp}      Game registry + launcher mode (one active game)
  IGame.h           Interface every game implements
  DogfightGame.*    The dogfight, behind IGame
  DogfightGame.* BombermanGame.* CurveFeverGame.*
  RacingGame.* SnakeGame.* PongGame.*   one IGame implementation each
  Net.cpp UI.cpp    HTTP/WS server, host display
include/            Firmware headers
lib/                Local C++ libraries
data/               Web assets uploaded to LittleFS
  shared/           Shared libs used by every game (three.min.js.gz)
  dogfight/ blaster/ trails/ racing/ snake/ bounce/   one folder per game
test/               Unit tests
```

If you add web assets, place them in the relevant `data/<game>/` folder (or
`data/shared/` if reused) so `pio run -t uploadfs` ships them.

## Multi-game host (see plan.md Phase 8)

The Core2 hosts several games but runs exactly **one at a time**. Boot ->
launcher (touch list); pick a game -> it becomes the active game and its
lobby + join QR show; a MENU tap returns to the launcher.

- Each game implements `IGame` (`include/IGame.h`); `Host` (`src/Host.{h,cpp}`)
  owns the registry and the active pointer. Register new games in
  `Host::begin()`.
- A game owns its **own wire protocol**: the Net layer hands it raw text via
  `onClientText`. It also draws its **own host screen** via `drawHostScreen()`
  (using the shared `UI::drawHeader/drawButtons/drawJoinPanel` helpers) and
  requests redraws with `UI::invalidate()`.
- Player URL stays constant `http://192.168.4.1/`; the server redirects `/`
  to the active game's `slug()` folder. Web assets live in `data/<slug>/`,
  shared libs in `data/shared/`.
- On game switch the Host closes all WebSockets (`Net::closeAllClients()`) so
  browsers reconnect to the new game.
- Keep the PRNG in any game's server logic byte-identical to its client JS
  (see `mb32`/`mulberry32`) or arenas diverge. Blaster sidesteps this by
  sending the whole grid in `init` instead of a seed. Update `mock_server.py`
  to match any layout/protocol change.

### Adding a new game (checklist)

1. `class YourGame : public IGame` in `include/` + `src/` (own wire protocol
   in `onClientText`, own host screen via `drawHostScreen()`).
2. Register it in `Host::begin()` (`games_[count_++] = &yourGame_;`).
3. Add `data/<slug>/` (index.html, game.js, style.css); reuse `data/shared/`.
4. Add a `YourGameSim` to `mock_server.py` and register it in `SIMS` so it can
   be tested on PC (`uv run mock_server.py <slug>`).
5. `pio run` and `pio run -t buildfs` must both succeed.

## Local testing on PC (no Core2 hardware)

`mock_server.py` (repo root) stands in for the Core2 so the browser client
can be tested on a PC without hardware. It serves `data/` over HTTP and
speaks the same WebSocket protocol as the firmware (`src/Net.cpp`,
`src/Game.cpp`), including a couple of wandering bot enemies that fire and
can be shot down.

```text
uv run mock_server.py        # start, then open http://localhost:8080
```

PC controls (already in `game.js`): arrow keys steer, Space fires, left
Shift boosts — no touchscreen needed.

It is a **test mock**, not a second source of truth: it deliberately
implements only enough of the rules (hit resolution, asteroid HP, scoring,
respawn, round timer) to exercise the client. The authoritative rules live
in the C++ firmware. If you change the protocol or the wire format, update
`mock_server.py` **and** the firmware **and** [plan.md](plan.md) together so
they stay in lockstep — the PRNG port in `mulberry32`/`gen_asteroids` must
keep matching `data/game.js` or arenas diverge.

## Python tooling

Use **`uv`** for anything Python in this repo (running scripts, venvs,
dependencies): `uv run <script>.py`, `uv add <pkg>`, etc. Do not fall back
to bare `python`/`pip`/`venv`. `mock_server.py` uses only the standard
library, so `uv run mock_server.py` needs no extra install.

## Things to ask before doing

- Changing the protocol shape (adding/renaming top-level message types).
- Replacing `ESPAsyncWebServer` with another HTTP/WS stack.
- Introducing a JS bundler/build step (Vite, webpack, esbuild).
- Switching from LittleFS to SPIFFS or vice versa.
- Increasing `server_tick`, `client_send_rate`, or `max_players` past the
  values in [plan.md](plan.md).
- Adding any dependency on internet access at runtime.

## Things you can do without asking

- Implement the next unimplemented step from [plan.md](plan.md).
- Add small C++ helpers, refactor for readability, fix bugs.
- Add or improve serial logging behind a compile-time flag.
- Tune Three.js rendering (geometry simplification, frustum culling).
- Add unit tests under `test/` for protocol parsing or game logic.

## Definition of "done" for a task

- Builds cleanly with `pio run` for the `m5stack-core2` env.
- Web assets, if changed, are placed in `data/` and uploaded via
  `pio run -t uploadfs`.
- New protocol fields/messages are documented in [plan.md](plan.md).
- Manual smoke test described, even if the agent cannot run it on real
  hardware: which screen on the Core2 should show what, what the browser
  should look like, expected console output.

## When in doubt

Prefer the smallest change that moves the current plan phase forward.
Do not introduce abstractions for hypothetical future phases. Three
similar lines is fine; a premature framework is not.
