#pragma once

#include <Arduino.h>

namespace Config {

// Wi-Fi SoftAP
constexpr const char *AP_SSID     = "PocketWebGames";
constexpr const char *AP_PASSWORD = "pocketgames";
constexpr uint8_t     AP_CHANNEL  = 6;
constexpr uint8_t     AP_MAX_CONN = 6;

// HTTP / WebSocket
constexpr uint16_t HTTP_PORT  = 80;
constexpr const char *WS_PATH = "/ws";

// Game rules
constexpr uint8_t  MAX_PLAYERS         = 6;
constexpr int      START_HP            = 100;
constexpr int      DAMAGE_PER_HIT      = 25;
constexpr float    ARENA_SIZE          = 500.0f;
constexpr float    HIT_RADIUS          = 6.0f;     // ship hit sphere (was 4.0, too tight)
constexpr float    LASER_RANGE         = 220.0f;
constexpr uint32_t ROUND_MS            = 3UL * 60UL * 1000UL;
constexpr uint32_t RESPAWN_MS          = 2000;
constexpr uint32_t PLAYER_TIMEOUT_MS   = 8000;     // drop if no state for this long
constexpr uint32_t STATE_BROADCAST_MS  = 100;      // 10 Hz
constexpr uint32_t SCORE_BROADCAST_MS  = 500;      // 2 Hz
constexpr uint32_t ROUND_BROADCAST_MS  = 1000;     // 1 Hz

// Asteroids (server-authoritative). Same generator as client (mulberry32).
constexpr uint8_t  MAX_ASTEROIDS       = 24;
constexpr int      ASTEROID_HP_PER_R   = 5;   // hp = (int)(r * 5)

// Ship-vs-asteroid ramming damage.
constexpr int      COLLISION_PLAYER_DMG  = 15;
constexpr int      COLLISION_ASTEROID_DMG = 15;
constexpr uint32_t COLLISION_COOLDOWN_MS = 600;

// Power-ups.
constexpr uint8_t  MAX_POWERUPS          = 6;
constexpr uint8_t  POWERUP_TARGET_COUNT  = 4;
constexpr uint32_t POWERUP_SPAWN_MS      = 8000;
constexpr float    POWERUP_PICKUP_RADIUS = 6.0f;
constexpr int      POWERUP_REPAIR_HP     = 50;
constexpr uint8_t  PU_REPAIR  = 0;
constexpr uint8_t  PU_SHIELD  = 1;
constexpr uint8_t  PU_RAPID   = 2;
constexpr uint8_t  PU_TRIPLE  = 3;
constexpr uint8_t  PU_TYPE_COUNT = 4;

// ---- Bomberman game (timed deathmatch on a tile grid) ----
// The grid layout is sent verbatim in `init` (no PRNG parity needed). Walls
// are the border plus pillars where both coordinates are even; the rest is
// randomly filled with destructible bricks, with spawn corners cleared.
constexpr uint8_t  BM_W            = 13;   // grid width  (odd)
constexpr uint8_t  BM_H            = 11;   // grid height (odd)
constexpr uint32_t BM_FUSE_MS      = 2500; // time from placement to explosion
constexpr uint32_t BM_FLAME_MS     = 600;  // flame lifetime (kills on contact)
constexpr uint8_t  BM_BASE_BOMBS   = 1;    // bombs a fresh player may place
constexpr uint8_t  BM_MAX_BOMBS    = 6;
constexpr uint8_t  BM_BASE_FLAME   = 2;    // blast reach in tiles
constexpr uint8_t  BM_MAX_FLAME    = 6;
constexpr float    BM_BRICK_PROB   = 0.72f;
constexpr float    BM_PU_PROB      = 0.35f; // chance a destroyed brick drops a PU

constexpr uint8_t  BM_TILE_EMPTY   = 0;
constexpr uint8_t  BM_TILE_WALL    = 1;
constexpr uint8_t  BM_TILE_BRICK   = 2;

constexpr uint8_t  BM_PU_BOMB      = 0;    // +1 max bombs
constexpr uint8_t  BM_PU_FLAME     = 1;    // +1 blast reach
constexpr uint8_t  BM_PU_SPEED     = 2;    // +move speed (client-side)
constexpr uint8_t  BM_PU_TYPE_COUNT = 3;

constexpr uint8_t  BM_MAX_BOMBS_TOTAL = 36;
constexpr uint8_t  BM_MAX_FLAMES      = 80;
constexpr uint8_t  BM_MAX_POWERUPS    = 12;

// ---- Curve Fever ("Achtung die Kurve") ----
// Players move forward at constant speed and only steer left/right, leaving a
// solid trail. Touching any trail or the wall is fatal. A match is a series of
// mini-rounds inside the round timer; the last survivor of each mini-round
// scores a point. Collision uses a coarse occupancy grid (cheap on the ESP32).
constexpr uint8_t  CF_GW = 120;            // collision grid width  (cells)
constexpr uint8_t  CF_GH = 80;             // collision grid height (cells)
constexpr float    CF_SPEED = 20.0f;       // cells per second
constexpr float    CF_TURN  = 3.4f;        // radians per second
constexpr uint32_t CF_STATE_MS         = 33;    // ~30 Hz head broadcast
constexpr uint32_t CF_GAP_INTERVAL_MS  = 2600;  // time between trail gaps
constexpr uint32_t CF_GAP_LEN_MS       = 180;   // length of each trail gap
constexpr uint32_t CF_SPAWN_GRACE_MS   = 500;   // no self-collision after spawn
constexpr uint32_t CF_MINIROUND_GAP_MS = 1600;  // pause between mini-rounds

// ---- 3D Racing ----
// Client-authoritative car pose (arcade physics in the browser); the server
// owns the track, counts checkpoints/laps and decides the winner. The track
// is a fixed closed loop sent verbatim in `init` (no PRNG parity needed).
constexpr uint8_t  RACE_N         = 24;     // track centreline waypoints
constexpr float    RACE_RX        = 160.0f; // loop radius X
constexpr float    RACE_RZ        = 110.0f; // loop radius Z
constexpr float    RACE_WIDTH     = 18.0f;  // road width (full)
constexpr float    RACE_CP_RADIUS = 26.0f;  // checkpoint capture radius
constexpr int      RACE_LAPS      = 3;      // laps to win (ends the round)

// ---- Snake Arena ----
// Fully server-authoritative: snakes step on a grid at a fixed rate, grow by
// eating food, and die on hitting any wall or snake body. Timed deathmatch:
// score = food eaten, respawn after death, highest score wins. State is sent
// incrementally (one head per step) over the reliable WebSocket, so clients
// stay in sync without full snapshots.
constexpr uint8_t  SNK_GW         = 32;
constexpr uint8_t  SNK_GH         = 24;
constexpr uint32_t SNK_STEP_MS    = 120;   // grid step interval
constexpr uint16_t SNK_MAXLEN     = 96;    // max body length per snake
constexpr uint8_t  SNK_GROW       = 3;     // cells gained per food
constexpr uint8_t  SNK_FOOD       = 5;     // food pellets kept in the arena
constexpr uint32_t SNK_RESPAWN_MS = 1500;

// ---- Pong (4-edge multiplayer) ----
// Server-authoritative ball on a unit-square arena; each player owns one of
// the four edges (unused edges are walls). Client-authoritative paddle slides
// along its edge. The last player to touch the ball scores when someone misses.
constexpr float    PONG_PADDLE_LEN = 0.20f;  // paddle length (fraction of edge)
constexpr float    PONG_PADDLE_T   = 0.03f;  // distance of the goal line from edge
constexpr float    PONG_BALL_R     = 0.018f;
constexpr float    PONG_BALL_SPEED = 0.55f;  // initial speed (arena units / s)
constexpr float    PONG_BALL_MAX   = 1.5f;
constexpr float    PONG_SPEEDUP    = 1.04f;  // per paddle hit
constexpr uint32_t PONG_SB_MS      = 33;     // ~30 Hz ball/paddle broadcast
constexpr uint8_t  PONG_EDGES      = 4;      // max simultaneous players

}  // namespace Config
