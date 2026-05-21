#pragma once

#include <Arduino.h>

#include "Config.h"
#include "IGame.h"

// One car. Pose (x,z,heading on the ground plane) is client-authoritative;
// the server counts checkpoints/laps from the reported position.
struct RcPlayer {
    bool     active = false;
    uint8_t  id     = 0;
    char     name[24] = {0};
    AsyncWebSocketClient *client = nullptr;

    float    x = 0, z = 0, h = 0;
    uint8_t  nextCp = 1;     // next checkpoint index to reach
    int      lap = 0;        // completed laps (also the score)
    uint32_t lastSeen = 0;
};

// Lap-racing on a fixed closed track. Implements IGame. Score = laps; the
// round ends when a car reaches RACE_LAPS or the timer runs out.
class RacingGame : public IGame {
public:
    const char *name() const override { return "3D Racing"; }
    const char *slug() const override { return "racing"; }

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
    float trackX_[Config::RACE_N];
    float trackZ_[Config::RACE_N];
    RcPlayer players_[Config::MAX_PLAYERS];

    GameState state_          = GameState::Lobby;
    uint32_t  roundStartedAt_ = 0;
    uint32_t  pausedAt_       = 0;
    uint32_t  pausedAccumMs_  = 0;
    uint32_t  lastStateBcast_ = 0;
    uint32_t  lastScoreBcast_ = 0;
    uint32_t  lastRoundBcast_ = 0;
    uint8_t   winner_         = 0;

    GameState lastDrawnState_       = (GameState)0xFF;
    uint8_t   lastDrawnPlayerCount_ = 255;
    uint32_t  lastDrawnSec_         = 0xFFFFFFFF;
    int       lastDrawnScores_[Config::MAX_PLAYERS] = {0};
    bool      scoresChanged();

    void buildTrack();
    void startPose(uint8_t id, float &x, float &z, float &h) const;

    int8_t addPlayer(AsyncWebSocketClient *client, const char *name);
    void   removeByClient(AsyncWebSocketClient *client);
    RcPlayer *findByClient(AsyncWebSocketClient *client);
    RcPlayer *findById(uint8_t id);

    void onState(uint8_t id, float x, float z, float h);
    void resetAllStats();

    void sendInitTo(const RcPlayer &p);
    void broadcastStates();
    void broadcastLap(uint8_t id, int lap);
    void broadcastScores();
    void broadcastRound();
    void broadcastLeave(uint8_t id);

    void drawLobby();
    void drawInGame();
    void drawEnd();
};
