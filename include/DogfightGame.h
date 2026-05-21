#pragma once

#include <Arduino.h>

#include "Config.h"
#include "IGame.h"

struct Player {
    bool     active = false;
    uint8_t  id     = 0;
    char     name[24] = {0};
    AsyncWebSocketClient *client = nullptr;

    // Pose (client-authoritative)
    float x = 0, y = 0, z = 0;
    float qx = 0, qy = 0, qz = 0, qw = 1;

    // Match state
    int      hp        = Config::START_HP;
    int      score     = 0;
    uint32_t lastSeen  = 0;
    uint32_t respawnAt = 0;   // 0 == alive

    // Buffs / cooldowns
    uint8_t  shieldHits     = 0;   // SHIELD power-up: absorbs next N hits
    uint32_t lastCollisionAt = 0;  // for ship-vs-asteroid cooldown
};

struct Asteroid {
    bool  alive = false;
    float x = 0, y = 0, z = 0;
    float r = 8;
    int   hp = 0;
    int   maxHp = 0;
};

struct PowerUp {
    bool    active = false;
    uint8_t id     = 0;     // sequence id for messages
    uint8_t type   = 0;     // PU_REPAIR | PU_SHIELD | PU_RAPID | PU_TRIPLE
    float   x = 0, y = 0, z = 0;
};

// The 3D space dogfight, hosted by the Core2. Implements IGame so it can be
// registered with the Host and launched from the on-device launcher.
class DogfightGame : public IGame {
public:
    // --- IGame ---
    const char *name() const override { return "Space Dogfight"; }
    const char *slug() const override { return "dogfight"; }

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

    // Read-only views (used internally and by the host screen)
    uint32_t  remainingMs()   const;
    uint32_t  seed()          const { return seed_; }
    uint8_t   playerCount()   const;
    uint8_t   winnerId()      const;
    const Player &player(uint8_t i) const { return players_[i]; }

private:
    // Player lifecycle
    int8_t addPlayer(AsyncWebSocketClient *client, const char *name);
    void   removeByClient(AsyncWebSocketClient *client);
    Player *findByClient(AsyncWebSocketClient *client);
    Player *findById(uint8_t id);

    // Messages from clients
    void onState(uint8_t id, float x, float y, float z,
                 float qx, float qy, float qz, float qw);
    void onFire(uint8_t shooterId,
                float ox, float oy, float oz,
                float dx, float dy, float dz);

    // Used by Net when a fresh client joins or a round restarts.
    void sendInitTo(const Player &p);

    Player    players_[Config::MAX_PLAYERS];
    Asteroid  asteroids_[Config::MAX_ASTEROIDS];
    PowerUp   powerups_[Config::MAX_POWERUPS];
    uint8_t   asteroidCount_   = 0;
    uint8_t   nextPowerupId_   = 1;
    uint32_t  lastPowerupSpawn_ = 0;
    GameState state_           = GameState::Lobby;
    uint32_t  seed_            = 0;
    uint32_t  roundStartedAt_  = 0;
    uint32_t  roundEndedAt_    = 0;
    uint32_t  pausedAt_        = 0;
    uint32_t  pausedAccumMs_   = 0;
    uint32_t  lastStateBcast_  = 0;
    uint32_t  lastScoreBcast_  = 0;
    uint32_t  lastRoundBcast_  = 0;

    // Host-display dirty tracking (drives UI redraws while this game is active).
    GameState lastDrawnState_       = (GameState)0xFF;
    uint8_t   lastDrawnPlayerCount_ = 255;
    uint32_t  lastDrawnSec_         = 0xFFFFFFFF;
    int       lastDrawnScores_[Config::MAX_PLAYERS] = {0};
    bool      scoresChanged();

    void regenAsteroids();
    void checkAsteroidCollisions(uint32_t now);
    void trySpawnPowerup(uint32_t now);
    void checkPowerupPickups();
    void applyPowerupEffect(Player &p, uint8_t type);

    // Damage helper: handles SHIELD absorption and kill/score logic in one place.
    // shooterId == 0 means environmental damage (collisions); no score bonus.
    void applyDamage(Player &target, int dmg, uint8_t shooterId);

    void broadcastStates();
    void broadcastScores();
    void broadcastRound();
    void broadcastHit(uint8_t shooter, uint8_t target, int hp);
    void broadcastKill(uint8_t shooter, uint8_t target);
    void broadcastRespawn(const Player &p);
    void broadcastFire(uint8_t shooterId,
                       float ox, float oy, float oz,
                       float dx, float dy, float dz);
    void broadcastAsteroidDamage(uint8_t idx, int hp);
    void broadcastAsteroidDestroy(uint8_t idx);
    void broadcastShield(uint8_t targetId);
    void broadcastPowerupSpawn(const PowerUp &pu);
    void broadcastPowerupPickup(uint8_t id, uint8_t targetId, uint8_t type);

    void respawn(Player &p);
    void resetAllStats();

    // Host-screen rendering (one per match state).
    void drawLobby();
    void drawInGame();
    void drawEnd();
};
