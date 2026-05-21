#pragma once

#include <Arduino.h>

#include "Config.h"
#include "IGame.h"

// One human/bot fighter on the grid. Position is client-authoritative (the
// browser predicts movement against the synced grid and sends x,y); the
// server is authoritative for bombs, explosions, bricks, power-ups, deaths
// and score.
struct BmPlayer {
    bool     active = false;
    uint8_t  id     = 0;
    char     name[24] = {0};
    AsyncWebSocketClient *client = nullptr;

    float    x = 1, y = 1;        // tile coordinates (center of a tile)
    int      score = 0;
    bool     dead = false;
    uint32_t respawnAt = 0;
    uint32_t lastSeen = 0;

    uint8_t  maxBombs    = Config::BM_BASE_BOMBS;
    uint8_t  activeBombs = 0;
    uint8_t  flame       = Config::BM_BASE_FLAME;
};

struct Bomb {
    bool     active = false;
    uint8_t  id     = 0;
    uint8_t  x = 0, y = 0;
    uint8_t  owner  = 0;
    uint8_t  range  = Config::BM_BASE_FLAME;
    uint32_t explodeAt = 0;
};

// A burning tile. Lingers for BM_FLAME_MS and kills any player standing on it.
struct Flame {
    bool     active = false;
    uint8_t  x = 0, y = 0;
    uint8_t  owner = 0;
    uint32_t until = 0;
};

struct BmPowerUp {
    bool    active = false;
    uint8_t id     = 0;
    uint8_t type   = 0;
    uint8_t x = 0, y = 0;
};

// Bomberman-style timed deathmatch, hosted by the Core2. Implements IGame so
// it can be registered with the Host and launched from the launcher.
class BombermanGame : public IGame {
public:
    const char *name() const override { return "Blaster"; }
    const char *slug() const override { return "blaster"; }

    void begin() override;
    void end() override;
    void update(uint32_t nowMs) override;

    void onClientText(AsyncWebSocketClient *client, const String &msg) override;
    void onClientDisconnect(AsyncWebSocketClient *client) override;

    void startRound() override;
    void resetRound() override;
    void togglePause() override;
    GameState state() const override { return state_; }
    void drawHostScreen() override;
    void drawLogo(int cx, int cy, int r) override;

    uint32_t remainingMs() const;
    uint8_t  playerCount() const;
    uint8_t  winnerId() const;

private:
    static constexpr int CELLS = Config::BM_W * Config::BM_H;
    static int idx(int x, int y) { return y * Config::BM_W + x; }

    uint8_t cells_[CELLS];
    BmPlayer  players_[Config::MAX_PLAYERS];
    Bomb      bombs_[Config::BM_MAX_BOMBS_TOTAL];
    Flame     flames_[Config::BM_MAX_FLAMES];
    BmPowerUp powerups_[Config::BM_MAX_POWERUPS];
    uint8_t   nextBombId_  = 1;
    uint8_t   nextPuId_    = 1;

    GameState state_          = GameState::Lobby;
    uint32_t  roundStartedAt_ = 0;
    uint32_t  pausedAt_       = 0;
    uint32_t  pausedAccumMs_  = 0;
    uint32_t  lastStateBcast_ = 0;
    uint32_t  lastScoreBcast_ = 0;
    uint32_t  lastRoundBcast_ = 0;

    // Host-display dirty tracking.
    GameState lastDrawnState_       = (GameState)0xFF;
    uint8_t   lastDrawnPlayerCount_ = 255;
    uint32_t  lastDrawnSec_         = 0xFFFFFFFF;
    int       lastDrawnScores_[Config::MAX_PLAYERS] = {0};
    bool      scoresChanged();

    // Level / players
    void regenLevel();
    void clearSpawnArea(int sx, int sy);
    void spawnPointFor(uint8_t id, int &sx, int &sy) const;
    int8_t addPlayer(AsyncWebSocketClient *client, const char *name);
    void   removeByClient(AsyncWebSocketClient *client);
    BmPlayer *findByClient(AsyncWebSocketClient *client);
    BmPlayer *findById(uint8_t id);

    // Inbound
    void onState(uint8_t id, float x, float y);
    void onPlaceBomb(uint8_t id);

    // Simulation
    void processBombs(uint32_t now);
    void explodeBomb(Bomb &b, uint32_t now);
    void addFlame(int x, int y, uint8_t owner, uint32_t now);
    void expireFlames(uint32_t now);
    void killPlayersOnFlames(uint32_t now);
    void checkPickups();
    void applyPickup(BmPlayer &p, uint8_t type);
    void maybeDropPowerup(int x, int y);
    void killPlayer(BmPlayer &target, uint8_t owner);
    void respawn(BmPlayer &p);
    void resetAllStats();

    // Outbound
    void sendInitTo(const BmPlayer &p);
    void broadcastStates();
    void broadcastScores();
    void broadcastRound();
    void broadcastBomb(const Bomb &b);
    void broadcastBoom(const uint8_t *cx, const uint8_t *cy, uint8_t n);
    void broadcastBrick(int x, int y);
    void broadcastKill(uint8_t owner, uint8_t target);
    void broadcastRespawn(const BmPlayer &p);
    void broadcastPowerupSpawn(const BmPowerUp &pu);
    void broadcastPowerupPickup(uint8_t id, uint8_t target, uint8_t type);
    void broadcastLeave(uint8_t id);

    // Host screen
    void drawLobby();
    void drawInGame();
    void drawEnd();
};
