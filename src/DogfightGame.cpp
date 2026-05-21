#include "DogfightGame.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <M5Core2.h>
#include <math.h>

#include "Config.h"
#include "Net.h"
#include "UI.h"

// ---------- helpers ----------

namespace {

uint32_t newSeed() {
    return (uint32_t)esp_random();
}

float randFloat(float lo, float hi) {
    return lo + (float)esp_random() / (float)UINT32_MAX * (hi - lo);
}

// mulberry32 PRNG. Must produce the EXACT same sequence as the JS counterpart
// in data/dogfight/game.js, otherwise client and server arenas would diverge.
// The trick is uint32 wrap-around arithmetic (== JS `| 0` / `Math.imul`).
inline uint32_t mb32(uint32_t &s) {
    s = s + 0x6D2B79F5u;
    uint32_t t = s;
    t = (t ^ (t >> 15)) * (t | 1u);
    t = t ^ (t + ((t ^ (t >> 7)) * (t | 61u)));
    return t ^ (t >> 14);
}
inline float mb32f(uint32_t &s) {
    return (float)mb32(s) / 4294967296.0f;
}

// Closest distance from point p to ray (origin o, unit direction d), within tMin..tMax.
// Returns the distance squared and the t value at the closest approach.
struct RayHit {
    float distSq;
    float t;
};
RayHit closestOnRay(float ox, float oy, float oz,
                    float dx, float dy, float dz,
                    float px, float py, float pz) {
    float vx = px - ox, vy = py - oy, vz = pz - oz;
    float t = vx * dx + vy * dy + vz * dz;
    if (t < 0) t = 0;
    float cx = ox + dx * t - px;
    float cy = oy + dy * t - py;
    float cz = oz + dz * t - pz;
    return { cx * cx + cy * cy + cz * cz, t };
}

constexpr int SCREEN_W = 320;

}  // namespace

// ---------- lifecycle ----------

void DogfightGame::begin() {
    seed_ = newSeed();
    state_ = GameState::Lobby;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        players_[i] = Player{};
        players_[i].id = i + 1;
    }
    regenAsteroids();
    // Reset host-display dirty trackers so the first draw after launch happens.
    lastDrawnState_       = (GameState)0xFF;
    lastDrawnPlayerCount_ = 255;
    lastDrawnSec_         = 0xFFFFFFFF;
    for (auto &s : lastDrawnScores_) s = 0;
    Serial.printf("[game] begin, seed=%u, asteroids=%u\n", seed_, asteroidCount_);
}

void DogfightGame::end() {
    // Returning to the launcher: drop everyone and go quiet. The Net layer
    // closes the sockets; we just clear our view of them.
    for (auto &p : players_) {
        p.active = false;
        p.client = nullptr;
    }
    state_ = GameState::Lobby;
}

// MUST match the JS regenArena() asteroid loop in data/dogfight/game.js exactly.
// In particular: the PRNG calls happen in the order
//   for each asteroid:  r, x, y, z   (4 floats, no extra "seed" call)
// and asteroids come BEFORE stars (stars are client-only).
void DogfightGame::regenAsteroids() {
    uint32_t s = seed_;
    asteroidCount_ = 22;
    if (asteroidCount_ > Config::MAX_ASTEROIDS) asteroidCount_ = Config::MAX_ASTEROIDS;
    for (uint8_t i = 0; i < asteroidCount_; ++i) {
        Asteroid &a = asteroids_[i];
        a.alive = true;
        a.r = 6.0f + mb32f(s) * 10.0f;
        a.x = (mb32f(s) * 2.0f - 1.0f) * Config::ARENA_SIZE * 0.45f;
        a.y = (mb32f(s) * 2.0f - 1.0f) * Config::ARENA_SIZE * 0.15f;
        a.z = (mb32f(s) * 2.0f - 1.0f) * Config::ARENA_SIZE * 0.45f;
        a.maxHp = (int)(a.r * Config::ASTEROID_HP_PER_R);
        a.hp    = a.maxHp;
    }
    for (uint8_t i = asteroidCount_; i < Config::MAX_ASTEROIDS; ++i) {
        asteroids_[i] = Asteroid{};
    }
}

void DogfightGame::update(uint32_t now) {
    // Drop stale clients.
    for (auto &p : players_) {
        if (!p.active) continue;
        if ((int32_t)(now - p.lastSeen) > (int32_t)Config::PLAYER_TIMEOUT_MS) {
            Serial.printf("[game] timeout id=%u\n", p.id);
            p.active = false;
            p.client = nullptr;
            // Notify the rest.
            StaticJsonDocument<64> doc;
            doc["t"] = "leave";
            doc["id"] = p.id;
            String s; serializeJson(doc, s);
            Net::broadcastText(s);
        }
    }

    // Gameplay (respawns, collisions, power-ups) ticks in every state EXCEPT
    // Paused. Score-add and round-timer remain gated on Running elsewhere, so
    // Lobby/Ended just behave like a warm-up arena.
    if (state_ != GameState::Paused) {
        for (auto &p : players_) {
            if (p.active && p.respawnAt != 0 && now >= p.respawnAt) {
                respawn(p);
                broadcastRespawn(p);
            }
        }
        checkAsteroidCollisions(now);
        checkPowerupPickups();
        trySpawnPowerup(now);
    }

    // Round end?
    if (state_ == GameState::Running && remainingMs() == 0) {
        state_ = GameState::Ended;
        roundEndedAt_ = now;
        broadcastRound();
        Serial.println("[game] round ended");
    }

    // Periodic broadcasts.
    if (now - lastStateBcast_ >= Config::STATE_BROADCAST_MS) {
        lastStateBcast_ = now;
        broadcastStates();
    }
    if (now - lastScoreBcast_ >= Config::SCORE_BROADCAST_MS) {
        lastScoreBcast_ = now;
        broadcastScores();
    }
    if (now - lastRoundBcast_ >= Config::ROUND_BROADCAST_MS) {
        lastRoundBcast_ = now;
        broadcastRound();
    }

    // Host-display dirty tracking (moved from the UI layer so the UI stays
    // game-agnostic). Mark the host screen for redraw when something visible
    // changes: the match state, the lobby player count, or the timer/scores.
    bool dirty = false;
    if (state_ != lastDrawnState_) {
        lastDrawnState_ = state_;
        dirty = true;
    }
    if (state_ == GameState::Lobby) {
        uint8_t pc = playerCount();
        if (pc != lastDrawnPlayerCount_) {
            lastDrawnPlayerCount_ = pc;
            dirty = true;
        }
    }
    if (state_ == GameState::Running || state_ == GameState::Paused) {
        uint32_t s = remainingMs() / 1000;
        if (s != lastDrawnSec_ || scoresChanged()) {
            lastDrawnSec_ = s;
            dirty = true;
        }
    }
    if (dirty) UI::invalidate();
}

// ---------- net entry points ----------

void DogfightGame::onClientText(AsyncWebSocketClient *client, const String &text) {
    StaticJsonDocument<384> doc;
    DeserializationError err = deserializeJson(doc, text);
    if (err) {
        Serial.printf("[ws] bad json from #%u: %s\n", client->id(), err.c_str());
        return;
    }
    const char *type = doc["t"] | "";

    if (strcmp(type, "join") == 0) {
        const char *name = doc["name"] | "Pilot";
        int8_t id = addPlayer(client, name);
        if (id < 0) {
            client->text("{\"t\":\"err\",\"m\":\"full\"}");
            client->close();
            return;
        }
        Player *p = findById((uint8_t)id);
        if (p) sendInitTo(*p);
        return;
    }

    // For all other message types we need a known player.
    Player *p = findByClient(client);
    if (!p) return;

    if (strcmp(type, "s") == 0) {
        onState(p->id,
                doc["x"]  | 0.0f, doc["y"]  | 0.0f, doc["z"]  | 0.0f,
                doc["qx"] | 0.0f, doc["qy"] | 0.0f, doc["qz"] | 0.0f,
                doc["qw"] | 1.0f);
    } else if (strcmp(type, "f") == 0) {
        onFire(p->id,
               doc["x"]  | 0.0f, doc["y"]  | 0.0f, doc["z"]  | 0.0f,
               doc["dx"] | 0.0f, doc["dy"] | 0.0f, doc["dz"] | 1.0f);
    } else if (strcmp(type, "ping") == 0) {
        client->text("{\"t\":\"pong\"}");
    }
}

void DogfightGame::onClientDisconnect(AsyncWebSocketClient *client) {
    removeByClient(client);
}

// ---------- player lifecycle ----------

int8_t DogfightGame::addPlayer(AsyncWebSocketClient *client, const char *name) {
    for (auto &p : players_) {
        if (!p.active) {
            p.active = true;
            p.client = client;
            strncpy(p.name, name, sizeof(p.name) - 1);
            p.name[sizeof(p.name) - 1] = 0;
            p.hp = Config::START_HP;
            p.score = 0;
            p.x = randFloat(-20, 20);
            p.y = randFloat(-5, 5);
            p.z = randFloat(-20, 20);
            p.qx = p.qy = p.qz = 0; p.qw = 1;
            p.lastSeen = millis();
            p.respawnAt = 0;
            // IMPORTANT: reset slot-recycled buff/cooldown state. Otherwise a
            // new joiner inherits the previous occupant's timestamps and the
            // collision cooldown blocks damage for a long time.
            p.shieldHits      = 0;
            p.lastCollisionAt = 0;
            Serial.printf("[game] join id=%u name=%s\n", p.id, p.name);
            return (int8_t)p.id;
        }
    }
    return -1;
}

void DogfightGame::removeByClient(AsyncWebSocketClient *client) {
    for (auto &p : players_) {
        if (p.active && p.client == client) {
            Serial.printf("[game] leave id=%u name=%s\n", p.id, p.name);
            p.active = false;
            p.client = nullptr;
            StaticJsonDocument<64> doc;
            doc["t"] = "leave";
            doc["id"] = p.id;
            String s; serializeJson(doc, s);
            Net::broadcastText(s);
            return;
        }
    }
}

Player *DogfightGame::findByClient(AsyncWebSocketClient *client) {
    for (auto &p : players_) {
        if (p.active && p.client == client) return &p;
    }
    return nullptr;
}

Player *DogfightGame::findById(uint8_t id) {
    if (id == 0 || id > Config::MAX_PLAYERS) return nullptr;
    Player &p = players_[id - 1];
    return p.active ? &p : nullptr;
}

// ---------- inbound messages ----------

void DogfightGame::onState(uint8_t id, float x, float y, float z,
                           float qx, float qy, float qz, float qw) {
    Player *p = findById(id);
    if (!p) return;
    p->x = x; p->y = y; p->z = z;
    p->qx = qx; p->qy = qy; p->qz = qz; p->qw = qw;
    p->lastSeen = millis();
}

void DogfightGame::onFire(uint8_t shooterId,
                          float ox, float oy, float oz,
                          float dx, float dy, float dz) {
    // Shooting always works except while paused.
    if (state_ == GameState::Paused) return;

    Player *shooter = findById(shooterId);
    if (!shooter || shooter->respawnAt != 0) return;

    // Normalize direction.
    float len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 1e-6f) return;
    dx /= len; dy /= len; dz /= len;

    // Always rebroadcast the shot so others can render it.
    broadcastFire(shooterId, ox, oy, oz, dx, dy, dz);

    // Find the closest hit along the ray: a player or an asteroid. Whichever
    // is closer "wins" — asteroids block the laser, giving them tactical use.
    enum { K_NONE, K_PLAYER, K_ASTEROID } kind = K_NONE;
    float    bestT = Config::LASER_RANGE;
    Player  *bestPlayer = nullptr;
    int      bestAst   = -1;

    for (auto &p : players_) {
        if (!p.active || p.id == shooterId || p.respawnAt != 0) continue;
        RayHit h = closestOnRay(ox, oy, oz, dx, dy, dz, p.x, p.y, p.z);
        if (h.t > Config::LASER_RANGE) continue;
        if (h.distSq <= Config::HIT_RADIUS * Config::HIT_RADIUS && h.t < bestT) {
            kind = K_PLAYER; bestPlayer = &p; bestT = h.t;
        }
    }
    for (uint8_t i = 0; i < asteroidCount_; ++i) {
        Asteroid &a = asteroids_[i];
        if (!a.alive) continue;
        RayHit h = closestOnRay(ox, oy, oz, dx, dy, dz, a.x, a.y, a.z);
        if (h.t > Config::LASER_RANGE) continue;
        if (h.distSq <= a.r * a.r && h.t < bestT) {
            kind = K_ASTEROID; bestAst = i; bestT = h.t;
        }
    }

    if (kind == K_PLAYER && bestPlayer) {
        applyDamage(*bestPlayer, Config::DAMAGE_PER_HIT, shooterId);
    } else if (kind == K_ASTEROID && bestAst >= 0) {
        Asteroid &a = asteroids_[bestAst];
        a.hp -= Config::DAMAGE_PER_HIT;
        if (a.hp <= 0) {
            a.alive = false;
            broadcastAsteroidDestroy((uint8_t)bestAst);
        } else {
            broadcastAsteroidDamage((uint8_t)bestAst, a.hp);
        }
    }
}

// Central damage handler — every HP-reducing path goes through here so that
// SHIELD absorption and the kill/score broadcast happen in exactly one place.
void DogfightGame::applyDamage(Player &target, int dmg, uint8_t shooterId) {
    if (target.shieldHits > 0) {
        // Shield eats the hit entirely.
        target.shieldHits--;
        broadcastShield(target.id);
        return;
    }
    target.hp -= dmg;
    if (target.hp < 0) target.hp = 0;
    broadcastHit(shooterId, target.id, target.hp);
    if (target.hp == 0) {
        // Score only in a real round - lobby/ended are warm-up.
        if (shooterId != 0 && state_ == GameState::Running) {
            Player *shooter = findById(shooterId);
            if (shooter) shooter->score += 1;
        }
        target.respawnAt = millis() + Config::RESPAWN_MS;
        broadcastKill(shooterId, target.id);
        if (state_ == GameState::Running) broadcastScores();
    }
}

void DogfightGame::checkAsteroidCollisions(uint32_t now) {
    for (auto &p : players_) {
        if (!p.active || p.respawnAt != 0) continue;
        if (now - p.lastCollisionAt < Config::COLLISION_COOLDOWN_MS) continue;
        for (uint8_t i = 0; i < asteroidCount_; ++i) {
            Asteroid &a = asteroids_[i];
            if (!a.alive) continue;
            float dx = p.x - a.x, dy = p.y - a.y, dz = p.z - a.z;
            float d2 = dx*dx + dy*dy + dz*dz;
            float reach = Config::HIT_RADIUS + a.r;
            if (d2 < reach * reach) {
                p.lastCollisionAt = now;
                applyDamage(p, Config::COLLISION_PLAYER_DMG, 0);

                a.hp -= Config::COLLISION_ASTEROID_DMG;
                if (a.hp <= 0) {
                    a.alive = false;
                    broadcastAsteroidDestroy(i);
                } else {
                    broadcastAsteroidDamage(i, a.hp);
                }
                break;  // one collision event per player per tick
            }
        }
    }
}

void DogfightGame::trySpawnPowerup(uint32_t now) {
    // Outer caller in update() already gates by state. Spawn pacing only.
    if (now - lastPowerupSpawn_ < Config::POWERUP_SPAWN_MS) return;
    lastPowerupSpawn_ = now;

    uint8_t alive = 0;
    int8_t  slot  = -1;
    for (uint8_t i = 0; i < Config::MAX_POWERUPS; ++i) {
        if (powerups_[i].active) alive++;
        else if (slot < 0) slot = (int8_t)i;
    }
    if (alive >= Config::POWERUP_TARGET_COUNT) return;
    if (slot < 0) return;

    PowerUp &pu = powerups_[slot];
    pu.active = true;
    pu.id     = nextPowerupId_++;
    if (nextPowerupId_ == 0) nextPowerupId_ = 1;
    pu.type   = (uint8_t)(esp_random() % Config::PU_TYPE_COUNT);
    pu.x = randFloat(-Config::ARENA_SIZE * 0.40f, Config::ARENA_SIZE * 0.40f);
    pu.y = randFloat(-Config::ARENA_SIZE * 0.10f, Config::ARENA_SIZE * 0.10f);
    pu.z = randFloat(-Config::ARENA_SIZE * 0.40f, Config::ARENA_SIZE * 0.40f);
    broadcastPowerupSpawn(pu);
    Serial.printf("[game] spawn pu id=%u type=%u\n", pu.id, pu.type);
}

void DogfightGame::checkPowerupPickups() {
    const float pr2 = Config::POWERUP_PICKUP_RADIUS * Config::POWERUP_PICKUP_RADIUS;
    for (auto &p : players_) {
        if (!p.active || p.respawnAt != 0) continue;
        for (auto &pu : powerups_) {
            if (!pu.active) continue;
            float dx = p.x - pu.x, dy = p.y - pu.y, dz = p.z - pu.z;
            if (dx*dx + dy*dy + dz*dz < pr2) {
                applyPowerupEffect(p, pu.type);
                broadcastPowerupPickup(pu.id, p.id, pu.type);
                pu.active = false;
            }
        }
    }
}

void DogfightGame::applyPowerupEffect(Player &p, uint8_t type) {
    switch (type) {
        case Config::PU_REPAIR: {
            p.hp += Config::POWERUP_REPAIR_HP;
            if (p.hp > Config::START_HP) p.hp = Config::START_HP;
            break;
        }
        case Config::PU_SHIELD:
            p.shieldHits = 1;
            break;
        case Config::PU_RAPID:
        case Config::PU_TRIPLE:
            // Pure client-side buff: server only relays the pickup.
            break;
    }
}

// ---------- match control ----------

void DogfightGame::startRound() {
    seed_ = newSeed();
    resetAllStats();
    regenAsteroids();
    for (auto &pu : powerups_) pu.active = false;
    lastPowerupSpawn_ = millis();
    roundStartedAt_ = millis();
    roundEndedAt_   = 0;
    pausedAt_       = 0;
    pausedAccumMs_  = 0;
    state_          = GameState::Running;

    // Re-init clients with new seed (so all browsers regenerate the arena).
    for (auto &p : players_) {
        if (!p.active || !p.client) continue;
        sendInitTo(p);
    }
    broadcastRound();
    Serial.printf("[game] round start seed=%u\n", seed_);
}

void DogfightGame::sendInitTo(const Player &p) {
    // For late joiners we replay enough state to render the live arena:
    //   dead[]  -> indices of destroyed asteroids
    //   adhp[]  -> [idx, hp] pairs for asteroids damaged but still alive
    //   pu[]    -> active power-up spawns
    StaticJsonDocument<1024> doc;
    doc["t"]      = "init";
    doc["id"]     = p.id;
    doc["seed"]   = seed_;
    doc["arena"]  = Config::ARENA_SIZE;
    doc["hp"]     = Config::START_HP;
    doc["acount"] = asteroidCount_;

    JsonArray dead = doc.createNestedArray("dead");
    JsonArray adhp = doc.createNestedArray("adhp");
    for (uint8_t i = 0; i < asteroidCount_; ++i) {
        const Asteroid &a = asteroids_[i];
        if (!a.alive) {
            dead.add(i);
        } else if (a.hp < a.maxHp) {
            JsonArray row = adhp.createNestedArray();
            row.add(i);
            row.add(a.hp);
        }
    }

    JsonArray pu = doc.createNestedArray("pu");
    for (auto &x : powerups_) {
        if (!x.active) continue;
        JsonObject o = pu.createNestedObject();
        o["id"]   = x.id;
        o["type"] = x.type;
        o["x"]    = x.x;
        o["y"]    = x.y;
        o["z"]    = x.z;
    }

    String s; serializeJson(doc, s);
    Net::sendText(p.client, s);
}

void DogfightGame::resetRound() {
    state_ = GameState::Lobby;
    roundStartedAt_ = 0;
    roundEndedAt_   = 0;
    pausedAt_       = 0;
    pausedAccumMs_  = 0;
    resetAllStats();
    broadcastRound();
    broadcastScores();
    Serial.println("[game] reset to lobby");
}

void DogfightGame::togglePause() {
    uint32_t now = millis();
    if (state_ == GameState::Running) {
        state_ = GameState::Paused;
        pausedAt_ = now;
    } else if (state_ == GameState::Paused) {
        pausedAccumMs_ += now - pausedAt_;
        pausedAt_ = 0;
        state_ = GameState::Running;
    }
    broadcastRound();
}

// ---------- queries ----------

uint32_t DogfightGame::remainingMs() const {
    if (state_ != GameState::Running && state_ != GameState::Paused) return Config::ROUND_MS;
    uint32_t now      = millis();
    uint32_t pauseExtra = (state_ == GameState::Paused) ? (now - pausedAt_) : 0;
    uint32_t elapsed  = now - roundStartedAt_ - pausedAccumMs_ - pauseExtra;
    if (elapsed >= Config::ROUND_MS) return 0;
    return Config::ROUND_MS - elapsed;
}

uint8_t DogfightGame::playerCount() const {
    uint8_t n = 0;
    for (const auto &p : players_) if (p.active) ++n;
    return n;
}

uint8_t DogfightGame::winnerId() const {
    int best = -1;
    uint8_t id = 0;
    for (const auto &p : players_) {
        if (!p.active) continue;
        if (p.score > best) {
            best = p.score;
            id = p.id;
        }
    }
    return id;
}

// ---------- internals ----------

void DogfightGame::respawn(Player &p) {
    p.hp = Config::START_HP;
    p.respawnAt = 0;
    p.shieldHits = 0;
    p.lastCollisionAt = 0;
    p.x = randFloat(-30, 30);
    p.y = randFloat(-8, 8);
    p.z = randFloat(-30, 30);
}

void DogfightGame::resetAllStats() {
    for (auto &p : players_) {
        if (!p.active) continue;
        p.hp = Config::START_HP;
        p.score = 0;
        p.respawnAt = 0;
        p.shieldHits = 0;
        p.lastCollisionAt = 0;
    }
}

bool DogfightGame::scoresChanged() {
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        int s = players_[i].active ? players_[i].score : -1;
        if (s != lastDrawnScores_[i]) {
            lastDrawnScores_[i] = s;
            return true;
        }
    }
    return false;
}

// ---------- host screen ----------

void DogfightGame::drawHostScreen() {
    switch (state_) {
        case GameState::Lobby:   drawLobby();  break;
        case GameState::Running:
        case GameState::Paused:  drawInGame(); break;
        case GameState::Ended:   drawEnd();    break;
    }
}

void DogfightGame::drawLogo(int cx, int cy, int r) {
    // swept-back wings
    M5.Lcd.fillTriangle(cx - r/5, cy, cx - r, cy + r*3/5, cx - r/5, cy + r*3/5, TFT_NAVY);
    M5.Lcd.fillTriangle(cx + r/5, cy, cx + r, cy + r*3/5, cx + r/5, cy + r*3/5, TFT_NAVY);
    // fuselage, nose up
    M5.Lcd.fillTriangle(cx, cy - r, cx - r*9/20, cy + r/2, cx + r*9/20, cy + r/2, TFT_CYAN);
    // cockpit + engine flame
    M5.Lcd.fillCircle(cx, cy - r/6, r/6, TFT_WHITE);
    M5.Lcd.fillTriangle(cx - r/6, cy + r/2, cx + r/6, cy + r/2, cx, cy + r*4/5, TFT_ORANGE);
}

void DogfightGame::drawLobby() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("SPACE DOGFIGHT");
    UI::drawJoinPanel();

    // Players list under the connection info.
    M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(8, 144);
    M5.Lcd.printf("Players %u/%u",
                  (unsigned)playerCount(), Config::MAX_PLAYERS);

    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setTextSize(1);
    int row = 168;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        const Player &p = players_[i];
        if (!p.active) continue;
        M5.Lcd.setCursor(20, row);
        M5.Lcd.printf("%u. %s", p.id, p.name);
        row += 10;
        if (row > 195) break;
    }

    UI::drawButtons("Start", "Menu", "Pause");
}

void DogfightGame::drawInGame() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("ROUND IN PLAY");

    uint32_t s = remainingMs() / 1000;
    M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.setCursor(8, 32);
    M5.Lcd.printf("%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));

    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    int row = 76;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        const Player &p = players_[i];
        if (!p.active) continue;
        M5.Lcd.setCursor(8,  row);
        M5.Lcd.printf("%u", p.id);
        M5.Lcd.setCursor(28, row);
        M5.Lcd.print(p.name);
        char score[8];
        snprintf(score, sizeof(score), "%d", p.score);
        int w = (int)strlen(score) * 12;
        M5.Lcd.setCursor(SCREEN_W - w - 12, row);
        M5.Lcd.print(score);
        row += 22;
        if (row > 190) break;
    }

    UI::drawButtons("Start", "Menu",
                    state_ == GameState::Paused ? "Resume" : "Pause");
}

void DogfightGame::drawEnd() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("ROUND OVER");

    uint8_t wid = winnerId();
    const Player *w = nullptr;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        if (players_[i].id == wid && players_[i].active) { w = &players_[i]; break; }
    }

    M5.Lcd.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.setCursor(8, 48);
    if (w) M5.Lcd.printf("Winner: %s", w->name);
    else   M5.Lcd.print("No winner");

    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(8, 96);
    if (w) M5.Lcd.printf("Score: %d", w->score);

    UI::drawButtons("NewRnd", "Menu", "");
}

// ---------- outbound broadcasts ----------

void DogfightGame::broadcastStates() {
    uint8_t n = playerCount();
    if (n == 0) return;
    // {"t":"sb","p":[{id,x,y,z,qx,qy,qz,qw,hp,r},...]}
    // Allocate generously; up to 6 players ~ 80 bytes each.
    StaticJsonDocument<1024> doc;
    doc["t"] = "sb";
    JsonArray arr = doc.createNestedArray("p");
    for (auto &p : players_) {
        if (!p.active) continue;
        JsonObject o = arr.createNestedObject();
        o["id"] = p.id;
        o["x"]  = p.x;
        o["y"]  = p.y;
        o["z"]  = p.z;
        o["qx"] = p.qx;
        o["qy"] = p.qy;
        o["qz"] = p.qz;
        o["qw"] = p.qw;
        o["hp"] = p.hp;
        o["r"]  = p.respawnAt != 0 ? 1 : 0;
    }
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void DogfightGame::broadcastScores() {
    StaticJsonDocument<512> doc;
    doc["t"] = "score";
    JsonArray arr = doc.createNestedArray("p");
    for (auto &p : players_) {
        if (!p.active) continue;
        JsonArray row = arr.createNestedArray();
        row.add(p.id);
        row.add(p.score);
        row.add(p.name);
    }
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void DogfightGame::broadcastRound() {
    StaticJsonDocument<256> doc;
    doc["t"] = "round";
    switch (state_) {
        case GameState::Lobby:   doc["state"] = "lobby";   break;
        case GameState::Running: doc["state"] = "running"; break;
        case GameState::Paused:  doc["state"] = "paused";  break;
        case GameState::Ended:   doc["state"] = "ended";   break;
    }
    doc["remaining"] = remainingMs() / 1000;
    doc["winner"]    = state_ == GameState::Ended ? winnerId() : 0;
    // Diagnostic counts so the client can show what the server is actually
    // tracking. Cheap to send (a few bytes) and lifesaving when debugging.
    uint8_t aliveAst = 0, alivePu = 0;
    for (auto &a : asteroids_) if (a.alive) ++aliveAst;
    for (auto &x : powerups_)  if (x.active) ++alivePu;
    doc["nA"] = aliveAst;
    doc["nP"] = alivePu;
    doc["nC"] = playerCount();
    // Build stamp lets the client verify it is talking to the firmware we
    // just flashed (compared against its own static BUILD_VERSION).
    doc["fw"] = __DATE__ " " __TIME__;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void DogfightGame::broadcastHit(uint8_t shooter, uint8_t target, int hp) {
    StaticJsonDocument<96> doc;
    doc["t"]      = "hit";
    doc["a"]      = shooter;
    doc["target"] = target;
    doc["hp"]     = hp;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void DogfightGame::broadcastKill(uint8_t shooter, uint8_t target) {
    StaticJsonDocument<96> doc;
    doc["t"]      = "kill";
    doc["a"]      = shooter;
    doc["target"] = target;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void DogfightGame::broadcastRespawn(const Player &p) {
    StaticJsonDocument<128> doc;
    doc["t"]  = "respawn";
    doc["id"] = p.id;
    doc["x"]  = p.x;
    doc["y"]  = p.y;
    doc["z"]  = p.z;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void DogfightGame::broadcastFire(uint8_t shooterId,
                                 float ox, float oy, float oz,
                                 float dx, float dy, float dz) {
    StaticJsonDocument<192> doc;
    doc["t"]  = "fb";
    doc["id"] = shooterId;
    doc["x"]  = ox;
    doc["y"]  = oy;
    doc["z"]  = oz;
    doc["dx"] = dx;
    doc["dy"] = dy;
    doc["dz"] = dz;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void DogfightGame::broadcastAsteroidDestroy(uint8_t idx) {
    StaticJsonDocument<64> doc;
    doc["t"]  = "adestroy";
    doc["id"] = idx;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void DogfightGame::broadcastAsteroidDamage(uint8_t idx, int hp) {
    StaticJsonDocument<96> doc;
    doc["t"]  = "adamage";
    doc["id"] = idx;
    doc["hp"] = hp;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void DogfightGame::broadcastShield(uint8_t targetId) {
    StaticJsonDocument<64> doc;
    doc["t"]      = "shield";
    doc["target"] = targetId;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void DogfightGame::broadcastPowerupSpawn(const PowerUp &pu) {
    StaticJsonDocument<128> doc;
    doc["t"]    = "pspawn";
    doc["id"]   = pu.id;
    doc["type"] = pu.type;
    doc["x"]    = pu.x;
    doc["y"]    = pu.y;
    doc["z"]    = pu.z;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void DogfightGame::broadcastPowerupPickup(uint8_t id, uint8_t targetId, uint8_t type) {
    StaticJsonDocument<96> doc;
    doc["t"]      = "ppickup";
    doc["id"]     = id;
    doc["target"] = targetId;
    doc["type"]   = type;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}
