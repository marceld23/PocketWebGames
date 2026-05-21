#pragma once

#include <Arduino.h>

#include "Config.h"
#include "IGame.h"

// One snake. The body is a ring buffer of grid cell indices (head first).
// Fully server-authoritative; the client only sends the desired direction.
struct Snake {
    bool     active = false;
    uint8_t  id     = 0;
    char     name[24] = {0};
    AsyncWebSocketClient *client = nullptr;

    uint16_t body[Config::SNK_MAXLEN];
    uint16_t headIdx = 0;     // ring index of the head
    uint16_t len = 0;

    int8_t   dx = 1, dy = 0;  // current direction
    int8_t   pdx = 1, pdy = 0;// pending (requested) direction
    bool     alive = false;
    int      score = 0;
    uint8_t  growBy = 0;
    uint32_t respawnAt = 0;
    uint32_t lastSeen = 0;

    uint16_t headCell() const { return body[headIdx]; }
    uint16_t tailCell() const { return body[(headIdx + len - 1) % Config::SNK_MAXLEN]; }
};

// Multiplayer Snake on a shared grid. Implements IGame. Timed deathmatch:
// score = food eaten; respawn after death; highest score wins.
class SnakeGame : public IGame {
public:
    const char *name() const override { return "Snake Arena"; }
    const char *slug() const override { return "snake"; }

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
    static constexpr int CELLS = Config::SNK_GW * Config::SNK_GH;
    static int cellOf(int x, int y) { return y * Config::SNK_GW + x; }
    static int cx(uint16_t c) { return c % Config::SNK_GW; }
    static int cy(uint16_t c) { return c / Config::SNK_GW; }

    uint8_t  occ_[CELLS];                 // 0 empty, else snake id
    Snake    snakes_[Config::MAX_PLAYERS];
    uint16_t food_[Config::SNK_FOOD];
    bool     foodActive_[Config::SNK_FOOD];

    GameState state_          = GameState::Lobby;
    uint32_t  roundStartedAt_ = 0;
    uint32_t  pausedAt_       = 0;
    uint32_t  pausedAccumMs_  = 0;
    uint32_t  stepAccum_      = 0;
    uint32_t  lastUpdate_     = 0;
    uint32_t  lastScoreBcast_ = 0;
    uint32_t  lastRoundBcast_ = 0;

    GameState lastDrawnState_       = (GameState)0xFF;
    uint8_t   lastDrawnPlayerCount_ = 255;
    uint32_t  lastDrawnSec_         = 0xFFFFFFFF;
    int       lastDrawnScores_[Config::MAX_PLAYERS] = {0};
    bool      scoresChanged();

    int8_t addPlayer(AsyncWebSocketClient *client, const char *name);
    void   removeByClient(AsyncWebSocketClient *client);
    Snake *findByClient(AsyncWebSocketClient *client);
    Snake *findById(uint8_t id);

    void onDir(uint8_t id, int8_t dx, int8_t dy);

    void resetSnake(Snake &s);
    void clearBody(Snake &s);
    void spawnSnake(Snake &s);
    int  freeCell() const;
    void spawnFood(uint8_t slot);
    void doStep();
    void killSnake(Snake &s);
    void resetAll();

    void sendInitTo(const Snake &p);
    void broadcastStep(const uint8_t *ids, const uint8_t *hx, const uint8_t *hy,
                       const uint8_t *grew, uint8_t n);
    void broadcastDead(uint8_t id);
    void broadcastRespawn(const Snake &s);
    void broadcastFoodSpawn(int x, int y);
    void broadcastFoodEat(int x, int y);
    void broadcastScores();
    void broadcastRound();
    void broadcastLeave(uint8_t id);

    void drawLobby();
    void drawInGame();
    void drawEnd();
};
