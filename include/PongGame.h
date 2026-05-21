#pragma once

#include <Arduino.h>

#include "Config.h"
#include "IGame.h"

// One paddle. The paddle position `v` (0..1 along the owned edge) is
// client-authoritative; the server owns the ball and scoring.
struct PongPlayer {
    bool     active = false;
    uint8_t  id     = 0;
    char     name[24] = {0};
    AsyncWebSocketClient *client = nullptr;

    int8_t   edge = -1;     // 0 left, 1 right, 2 top, 3 bottom
    float    v = 0.5f;      // paddle centre along its edge
    int      score = 0;
    uint32_t lastSeen = 0;
};

// 4-edge multiplayer Pong / air-hockey. Implements IGame. Timed match; the
// highest score wins. 1 player can rally solo (3 walls), up to 4 play at once.
class PongGame : public IGame {
public:
    const char *name() const override { return "Bounce"; }
    const char *slug() const override { return "bounce"; }

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
    PongPlayer players_[Config::MAX_PLAYERS];

    float bx_ = 0.5f, by_ = 0.5f, bvx_ = 0, bvy_ = 0;
    uint8_t lastHit_ = 0;

    GameState state_          = GameState::Lobby;
    uint32_t  roundStartedAt_ = 0;
    uint32_t  pausedAt_       = 0;
    uint32_t  pausedAccumMs_  = 0;
    uint32_t  lastUpdate_     = 0;
    uint32_t  lastSbBcast_    = 0;
    uint32_t  lastScoreBcast_ = 0;
    uint32_t  lastRoundBcast_ = 0;

    GameState lastDrawnState_       = (GameState)0xFF;
    uint8_t   lastDrawnPlayerCount_ = 255;
    uint32_t  lastDrawnSec_         = 0xFFFFFFFF;
    int       lastDrawnScores_[Config::MAX_PLAYERS] = {0};
    bool      scoresChanged();

    int8_t addPlayer(AsyncWebSocketClient *client, const char *name);
    void   removeByClient(AsyncWebSocketClient *client);
    PongPlayer *findByClient(AsyncWebSocketClient *client);
    PongPlayer *findById(uint8_t id);
    PongPlayer *playerOnEdge(int8_t edge);

    void resetBall();
    void stepBall(float dt);
    void scoreMiss(PongPlayer *missEdgePlayer);

    void sendInitTo(const PongPlayer &p);
    void broadcastStates();
    void broadcastPoint(uint8_t scorer, uint8_t on);
    void broadcastScores();
    void broadcastRound();
    void broadcastLeave(uint8_t id);

    void drawLobby();
    void drawInGame();
    void drawEnd();
};
