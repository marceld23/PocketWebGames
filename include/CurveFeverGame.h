#pragma once

#include <Arduino.h>

#include "Config.h"
#include "IGame.h"

// One snake/curve. Position is server-authoritative (the server integrates
// movement from the player's steer input and rasterises the trail into the
// occupancy grid), so collisions are identical for everyone. Clients only
// send steer input and render the head positions they receive.
struct CfPlayer {
    bool     active = false;
    uint8_t  id     = 0;
    char     name[24] = {0};
    AsyncWebSocketClient *client = nullptr;

    float    x = 0, y = 0, heading = 0;
    int8_t   turn = 0;        // -1 left, 0 straight, +1 right
    bool     alive = false;
    int      score = 0;
    uint32_t lastSeen = 0;

    uint32_t gapUntil   = 0;  // currently in a trail gap until this time
    uint32_t nextGap    = 0;  // when the next gap starts
    uint32_t graceUntil = 0;  // no self-collision until this time
    int16_t  cellX = -1;      // last grid cell the head occupied
    int16_t  cellY = -1;
};

// Curve Fever / "Achtung die Kurve" — last-survivor mini-rounds inside a
// timed match. Implements IGame so it can be launched from the Core2 launcher.
class CurveFeverGame : public IGame {
public:
    const char *name() const override { return "Trails"; }
    const char *slug() const override { return "trails"; }

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
    static constexpr int CELLS = Config::CF_GW * Config::CF_GH;
    static int gidx(int x, int y) { return y * Config::CF_GW + x; }
    bool occupied(int x, int y) const {
        return x >= 0 && y >= 0 && x < Config::CF_GW && y < Config::CF_GH
               && grid_[gidx(x, y)] != 0;
    }

    uint8_t  grid_[CELLS];
    CfPlayer players_[Config::MAX_PLAYERS];

    GameState state_          = GameState::Lobby;
    uint32_t  roundStartedAt_ = 0;
    uint32_t  pausedAt_       = 0;
    uint32_t  pausedAccumMs_  = 0;
    uint32_t  lastUpdate_     = 0;
    bool      miniActive_     = false;
    uint32_t  miniRestartAt_  = 0;

    uint32_t  lastStateBcast_ = 0;
    uint32_t  lastScoreBcast_ = 0;
    uint32_t  lastRoundBcast_ = 0;

    GameState lastDrawnState_       = (GameState)0xFF;
    uint8_t   lastDrawnPlayerCount_ = 255;
    uint32_t  lastDrawnSec_         = 0xFFFFFFFF;
    int       lastDrawnScores_[Config::MAX_PLAYERS] = {0};
    bool      scoresChanged();

    int8_t addPlayer(AsyncWebSocketClient *client, const char *name);
    void   removeByClient(AsyncWebSocketClient *client);
    CfPlayer *findByClient(AsyncWebSocketClient *client);
    CfPlayer *findById(uint8_t id);

    void onInput(uint8_t id, int8_t dir);

    void spawnFor(uint8_t id, float &x, float &y, float &heading) const;
    void startMiniRound(uint32_t now);
    void integrate(CfPlayer &p, uint32_t now, float dt);
    void killPlayer(CfPlayer &p);
    uint8_t aliveCount() const;
    void resetAllStats();

    void sendInitTo(const CfPlayer &p);
    void broadcastClear();
    void broadcastStates();
    void broadcastDead(uint8_t id);
    void broadcastScores();
    void broadcastRound();
    void broadcastLeave(uint8_t id);

    void drawLobby();
    void drawInGame();
    void drawEnd();
};
