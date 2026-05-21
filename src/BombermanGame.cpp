#include "BombermanGame.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <M5Core2.h>
#include <math.h>

#include "Config.h"
#include "Net.h"
#include "UI.h"

namespace {

float randFloat(float lo, float hi) {
    return lo + (float)esp_random() / (float)UINT32_MAX * (hi - lo);
}

constexpr int SCREEN_W = 320;

}  // namespace

// ---------- lifecycle ----------

void BombermanGame::begin() {
    state_ = GameState::Lobby;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        players_[i] = BmPlayer{};
        players_[i].id = i + 1;
    }
    for (auto &b : bombs_)    b = Bomb{};
    for (auto &f : flames_)   f = Flame{};
    for (auto &pu : powerups_) pu = BmPowerUp{};
    nextBombId_ = 1;
    nextPuId_   = 1;
    regenLevel();
    lastDrawnState_       = (GameState)0xFF;
    lastDrawnPlayerCount_ = 255;
    lastDrawnSec_         = 0xFFFFFFFF;
    for (auto &s : lastDrawnScores_) s = 0;
    Serial.println("[bm] begin");
}

void BombermanGame::end() {
    for (auto &p : players_) {
        p.active = false;
        p.client = nullptr;
    }
    state_ = GameState::Lobby;
}

void BombermanGame::regenLevel() {
    for (int y = 0; y < Config::BM_H; ++y) {
        for (int x = 0; x < Config::BM_W; ++x) {
            uint8_t t;
            bool border = (x == 0 || y == 0 ||
                           x == Config::BM_W - 1 || y == Config::BM_H - 1);
            if (border || (x % 2 == 0 && y % 2 == 0)) {
                t = Config::BM_TILE_WALL;
            } else {
                t = (randFloat(0, 1) < Config::BM_BRICK_PROB)
                        ? Config::BM_TILE_BRICK : Config::BM_TILE_EMPTY;
            }
            cells_[idx(x, y)] = t;
        }
    }
    // Keep every spawn area open so nobody starts boxed in.
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        int sx, sy;
        spawnPointFor(i + 1, sx, sy);
        clearSpawnArea(sx, sy);
    }
}

void BombermanGame::clearSpawnArea(int sx, int sy) {
    const int off[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (auto &o : off) {
        int x = sx + o[0], y = sy + o[1];
        if (x <= 0 || y <= 0 || x >= Config::BM_W - 1 || y >= Config::BM_H - 1)
            continue;
        if (cells_[idx(x, y)] == Config::BM_TILE_BRICK)
            cells_[idx(x, y)] = Config::BM_TILE_EMPTY;
    }
}

void BombermanGame::spawnPointFor(uint8_t id, int &sx, int &sy) const {
    const int W = Config::BM_W, H = Config::BM_H;
    const int pts[6][2] = {
        {1, 1}, {W - 2, 1}, {1, H - 2}, {W - 2, H - 2},
        {W / 2, 1}, {W / 2, H - 2},
    };
    const int *p = pts[(id - 1) % 6];
    sx = p[0];
    sy = p[1];
}

// ---------- tick ----------

void BombermanGame::update(uint32_t now) {
    // Drop stale clients.
    for (auto &p : players_) {
        if (!p.active) continue;
        if ((int32_t)(now - p.lastSeen) > (int32_t)Config::PLAYER_TIMEOUT_MS) {
            Serial.printf("[bm] timeout id=%u\n", p.id);
            p.active = false;
            p.client = nullptr;
            broadcastLeave(p.id);
        }
    }

    if (state_ != GameState::Paused) {
        processBombs(now);
        expireFlames(now);
        killPlayersOnFlames(now);
        checkPickups();
        for (auto &p : players_) {
            if (p.active && p.dead && p.respawnAt != 0 && now >= p.respawnAt) {
                respawn(p);
                broadcastRespawn(p);
            }
        }
    }

    if (state_ == GameState::Running && remainingMs() == 0) {
        state_ = GameState::Ended;
        broadcastRound();
        Serial.println("[bm] round ended");
    }

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

    // Host-display dirty tracking.
    bool dirty = false;
    if (state_ != lastDrawnState_) { lastDrawnState_ = state_; dirty = true; }
    if (state_ == GameState::Lobby) {
        uint8_t pc = playerCount();
        if (pc != lastDrawnPlayerCount_) { lastDrawnPlayerCount_ = pc; dirty = true; }
    }
    if (state_ == GameState::Running || state_ == GameState::Paused) {
        uint32_t s = remainingMs() / 1000;
        if (s != lastDrawnSec_ || scoresChanged()) { lastDrawnSec_ = s; dirty = true; }
    }
    if (dirty) UI::invalidate();
}

// ---------- net entry points ----------

void BombermanGame::onClientText(AsyncWebSocketClient *client, const String &text) {
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, text);
    if (err) {
        Serial.printf("[bm] bad json from #%u: %s\n", client->id(), err.c_str());
        return;
    }
    const char *type = doc["t"] | "";

    if (strcmp(type, "join") == 0) {
        const char *name = doc["name"] | "Player";
        int8_t id = addPlayer(client, name);
        if (id < 0) {
            client->text("{\"t\":\"err\",\"m\":\"full\"}");
            client->close();
            return;
        }
        BmPlayer *p = findById((uint8_t)id);
        if (p) sendInitTo(*p);
        return;
    }

    BmPlayer *p = findByClient(client);
    if (!p) return;

    if (strcmp(type, "s") == 0) {
        onState(p->id, doc["x"] | 1.0f, doc["y"] | 1.0f);
    } else if (strcmp(type, "b") == 0) {
        onPlaceBomb(p->id);
    } else if (strcmp(type, "ping") == 0) {
        client->text("{\"t\":\"pong\"}");
    }
}

void BombermanGame::onClientDisconnect(AsyncWebSocketClient *client) {
    removeByClient(client);
}

// ---------- players ----------

int8_t BombermanGame::addPlayer(AsyncWebSocketClient *client, const char *name) {
    for (auto &p : players_) {
        if (!p.active) {
            int sx, sy;
            spawnPointFor(p.id, sx, sy);
            p.active = true;
            p.client = client;
            strncpy(p.name, name, sizeof(p.name) - 1);
            p.name[sizeof(p.name) - 1] = 0;
            p.x = sx; p.y = sy;
            p.score = 0;
            p.dead = false;
            p.respawnAt = 0;
            p.maxBombs = Config::BM_BASE_BOMBS;
            p.activeBombs = 0;
            p.flame = Config::BM_BASE_FLAME;
            p.lastSeen = millis();
            Serial.printf("[bm] join id=%u name=%s\n", p.id, p.name);
            return (int8_t)p.id;
        }
    }
    return -1;
}

void BombermanGame::removeByClient(AsyncWebSocketClient *client) {
    for (auto &p : players_) {
        if (p.active && p.client == client) {
            Serial.printf("[bm] leave id=%u\n", p.id);
            p.active = false;
            p.client = nullptr;
            broadcastLeave(p.id);
            return;
        }
    }
}

BmPlayer *BombermanGame::findByClient(AsyncWebSocketClient *client) {
    for (auto &p : players_) if (p.active && p.client == client) return &p;
    return nullptr;
}

BmPlayer *BombermanGame::findById(uint8_t id) {
    if (id == 0 || id > Config::MAX_PLAYERS) return nullptr;
    BmPlayer &p = players_[id - 1];
    return p.active ? &p : nullptr;
}

// ---------- inbound ----------

void BombermanGame::onState(uint8_t id, float x, float y) {
    BmPlayer *p = findById(id);
    if (!p) return;
    p->x = x;
    p->y = y;
    p->lastSeen = millis();
}

void BombermanGame::onPlaceBomb(uint8_t id) {
    if (state_ == GameState::Paused) return;
    BmPlayer *p = findById(id);
    if (!p || p->dead) return;
    if (p->activeBombs >= p->maxBombs) return;

    int tx = (int)lroundf(p->x);
    int ty = (int)lroundf(p->y);
    if (tx < 0 || ty < 0 || tx >= Config::BM_W || ty >= Config::BM_H) return;
    if (cells_[idx(tx, ty)] != Config::BM_TILE_EMPTY) return;
    for (auto &b : bombs_)
        if (b.active && b.x == tx && b.y == ty) return;  // one bomb per tile

    for (auto &b : bombs_) {
        if (!b.active) {
            b.active = true;
            b.id = nextBombId_++;
            if (nextBombId_ == 0) nextBombId_ = 1;
            b.x = tx; b.y = ty;
            b.owner = id;
            b.range = p->flame;
            b.explodeAt = millis() + Config::BM_FUSE_MS;
            p->activeBombs++;
            broadcastBomb(b);
            return;
        }
    }
}

// ---------- simulation ----------

void BombermanGame::processBombs(uint32_t now) {
    // Loop so chain reactions triggered this tick resolve immediately.
    bool again = true;
    while (again) {
        again = false;
        for (auto &b : bombs_) {
            if (b.active && now >= b.explodeAt) {
                explodeBomb(b, now);
                again = true;
            }
        }
    }
}

void BombermanGame::explodeBomb(Bomb &b, uint32_t now) {
    b.active = false;
    BmPlayer *owner = findById(b.owner);
    if (owner && owner->activeBombs > 0) owner->activeBombs--;

    uint8_t cx[1 + 4 * Config::BM_MAX_FLAME];
    uint8_t cy[1 + 4 * Config::BM_MAX_FLAME];
    uint8_t n = 0;

    cx[n] = b.x; cy[n] = b.y; ++n;     // center

    const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (auto &d : dirs) {
        for (int r = 1; r <= b.range; ++r) {
            int nx = b.x + d[0] * r;
            int ny = b.y + d[1] * r;
            if (nx < 0 || ny < 0 || nx >= Config::BM_W || ny >= Config::BM_H) break;
            uint8_t t = cells_[idx(nx, ny)];
            if (t == Config::BM_TILE_WALL) break;

            if (n < sizeof(cx)) { cx[n] = nx; cy[n] = ny; ++n; }

            if (t == Config::BM_TILE_BRICK) {
                cells_[idx(nx, ny)] = Config::BM_TILE_EMPTY;
                broadcastBrick(nx, ny);
                maybeDropPowerup(nx, ny);
                break;  // bricks block the blast
            }
            // Chain any bomb caught in the blast.
            for (auto &ob : bombs_) {
                if (ob.active && ob.x == nx && ob.y == ny) ob.explodeAt = now;
            }
        }
    }

    for (uint8_t i = 0; i < n; ++i) addFlame(cx[i], cy[i], b.owner, now);
    broadcastBoom(cx, cy, n);
}

void BombermanGame::addFlame(int x, int y, uint8_t owner, uint32_t now) {
    for (auto &f : flames_) {
        if (!f.active) {
            f.active = true;
            f.x = x; f.y = y;
            f.owner = owner;
            f.until = now + Config::BM_FLAME_MS;
            return;
        }
    }
}

void BombermanGame::expireFlames(uint32_t now) {
    for (auto &f : flames_)
        if (f.active && now >= f.until) f.active = false;
}

void BombermanGame::killPlayersOnFlames(uint32_t now) {
    (void)now;
    for (auto &p : players_) {
        if (!p.active || p.dead) continue;
        int tx = (int)lroundf(p.x);
        int ty = (int)lroundf(p.y);
        for (auto &f : flames_) {
            if (f.active && f.x == tx && f.y == ty) {
                killPlayer(p, f.owner);
                break;
            }
        }
    }
}

void BombermanGame::checkPickups() {
    for (auto &p : players_) {
        if (!p.active || p.dead) continue;
        int tx = (int)lroundf(p.x);
        int ty = (int)lroundf(p.y);
        for (auto &pu : powerups_) {
            if (!pu.active || pu.x != tx || pu.y != ty) continue;
            applyPickup(p, pu.type);
            broadcastPowerupPickup(pu.id, p.id, pu.type);
            pu.active = false;
        }
    }
}

void BombermanGame::applyPickup(BmPlayer &p, uint8_t type) {
    switch (type) {
        case Config::BM_PU_BOMB:
            if (p.maxBombs < Config::BM_MAX_BOMBS) p.maxBombs++;
            break;
        case Config::BM_PU_FLAME:
            if (p.flame < Config::BM_MAX_FLAME) p.flame++;
            break;
        case Config::BM_PU_SPEED:
            // Pure client-side movement buff: server only relays the pickup.
            break;
    }
}

void BombermanGame::maybeDropPowerup(int x, int y) {
    if (randFloat(0, 1) >= Config::BM_PU_PROB) return;
    for (auto &pu : powerups_) {
        if (!pu.active) {
            pu.active = true;
            pu.id = nextPuId_++;
            if (nextPuId_ == 0) nextPuId_ = 1;
            pu.type = (uint8_t)(esp_random() % Config::BM_PU_TYPE_COUNT);
            pu.x = x; pu.y = y;
            broadcastPowerupSpawn(pu);
            return;
        }
    }
}

void BombermanGame::killPlayer(BmPlayer &target, uint8_t owner) {
    if (target.dead) return;
    target.dead = true;
    target.respawnAt = millis() + Config::RESPAWN_MS;
    target.activeBombs = 0;
    if (state_ == GameState::Running && owner != 0 && owner != target.id) {
        BmPlayer *o = findById(owner);
        if (o) o->score++;
    }
    broadcastKill(owner, target.id);
    if (state_ == GameState::Running) broadcastScores();
}

void BombermanGame::respawn(BmPlayer &p) {
    int sx, sy;
    spawnPointFor(p.id, sx, sy);
    p.x = sx; p.y = sy;
    p.dead = false;
    p.respawnAt = 0;
    p.maxBombs = Config::BM_BASE_BOMBS;
    p.flame = Config::BM_BASE_FLAME;
    p.activeBombs = 0;
}

void BombermanGame::resetAllStats() {
    for (auto &p : players_) {
        if (!p.active) continue;
        int sx, sy;
        spawnPointFor(p.id, sx, sy);
        p.x = sx; p.y = sy;
        p.score = 0;
        p.dead = false;
        p.respawnAt = 0;
        p.maxBombs = Config::BM_BASE_BOMBS;
        p.flame = Config::BM_BASE_FLAME;
        p.activeBombs = 0;
    }
}

// ---------- match control ----------

void BombermanGame::startRound() {
    regenLevel();
    resetAllStats();
    for (auto &b : bombs_)     b.active = false;
    for (auto &f : flames_)    f.active = false;
    for (auto &pu : powerups_) pu.active = false;
    roundStartedAt_ = millis();
    pausedAt_       = 0;
    pausedAccumMs_  = 0;
    state_          = GameState::Running;
    for (auto &p : players_) {
        if (p.active && p.client) sendInitTo(p);
    }
    broadcastRound();
    Serial.println("[bm] round start");
}

void BombermanGame::resetRound() {
    state_ = GameState::Lobby;
    roundStartedAt_ = 0;
    pausedAt_       = 0;
    pausedAccumMs_  = 0;
    resetAllStats();
    broadcastRound();
    broadcastScores();
    Serial.println("[bm] reset to lobby");
}

void BombermanGame::togglePause() {
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

uint32_t BombermanGame::remainingMs() const {
    if (state_ != GameState::Running && state_ != GameState::Paused) return Config::ROUND_MS;
    uint32_t now = millis();
    uint32_t pauseExtra = (state_ == GameState::Paused) ? (now - pausedAt_) : 0;
    uint32_t elapsed = now - roundStartedAt_ - pausedAccumMs_ - pauseExtra;
    if (elapsed >= Config::ROUND_MS) return 0;
    return Config::ROUND_MS - elapsed;
}

uint8_t BombermanGame::playerCount() const {
    uint8_t n = 0;
    for (const auto &p : players_) if (p.active) ++n;
    return n;
}

uint8_t BombermanGame::winnerId() const {
    int best = -1;
    uint8_t id = 0;
    for (const auto &p : players_) {
        if (!p.active) continue;
        if (p.score > best) { best = p.score; id = p.id; }
    }
    return id;
}

bool BombermanGame::scoresChanged() {
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        int s = players_[i].active ? players_[i].score : -1;
        if (s != lastDrawnScores_[i]) { lastDrawnScores_[i] = s; return true; }
    }
    return false;
}

// ---------- outbound ----------

void BombermanGame::sendInitTo(const BmPlayer &p) {
    StaticJsonDocument<1024> doc;
    doc["t"]  = "init";
    doc["id"] = p.id;
    doc["w"]  = Config::BM_W;
    doc["h"]  = Config::BM_H;
    doc["sx"] = p.x;
    doc["sy"] = p.y;
    doc["mb"] = p.maxBombs;
    doc["fl"] = p.flame;

    char buf[CELLS + 1];
    for (int i = 0; i < CELLS; ++i) {
        switch (cells_[i]) {
            case Config::BM_TILE_WALL:  buf[i] = '#'; break;
            case Config::BM_TILE_BRICK: buf[i] = '+'; break;
            default:                    buf[i] = '.'; break;
        }
    }
    buf[CELLS] = 0;
    doc["cells"] = (const char *)buf;

    JsonArray bombs = doc.createNestedArray("bombs");
    for (auto &b : bombs_) {
        if (!b.active) continue;
        JsonObject o = bombs.createNestedObject();
        o["id"] = b.id; o["x"] = b.x; o["y"] = b.y; o["range"] = b.range;
    }
    JsonArray pu = doc.createNestedArray("pu");
    for (auto &x : powerups_) {
        if (!x.active) continue;
        JsonObject o = pu.createNestedObject();
        o["id"] = x.id; o["type"] = x.type; o["x"] = x.x; o["y"] = x.y;
    }

    String s; serializeJson(doc, s);
    Net::sendText(p.client, s);
}

void BombermanGame::broadcastStates() {
    if (playerCount() == 0) return;
    StaticJsonDocument<768> doc;
    doc["t"] = "sb";
    JsonArray arr = doc.createNestedArray("p");
    for (auto &p : players_) {
        if (!p.active) continue;
        JsonObject o = arr.createNestedObject();
        o["id"] = p.id;
        o["x"]  = p.x;
        o["y"]  = p.y;
        o["d"]  = p.dead ? 1 : 0;
        o["mb"] = p.maxBombs;
        o["fl"] = p.flame;
    }
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void BombermanGame::broadcastScores() {
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

void BombermanGame::broadcastRound() {
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
    doc["nC"]        = playerCount();
    doc["fw"]        = __DATE__ " " __TIME__;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void BombermanGame::broadcastBomb(const Bomb &b) {
    StaticJsonDocument<128> doc;
    doc["t"]     = "bomb";
    doc["id"]    = b.id;
    doc["x"]     = b.x;
    doc["y"]     = b.y;
    doc["range"] = b.range;
    doc["owner"] = b.owner;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void BombermanGame::broadcastBoom(const uint8_t *cx, const uint8_t *cy, uint8_t n) {
    StaticJsonDocument<768> doc;
    doc["t"] = "boom";
    JsonArray c = doc.createNestedArray("c");
    for (uint8_t i = 0; i < n; ++i) {
        JsonArray cell = c.createNestedArray();
        cell.add(cx[i]);
        cell.add(cy[i]);
    }
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void BombermanGame::broadcastBrick(int x, int y) {
    StaticJsonDocument<64> doc;
    doc["t"] = "brick";
    doc["x"] = x;
    doc["y"] = y;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void BombermanGame::broadcastKill(uint8_t owner, uint8_t target) {
    StaticJsonDocument<96> doc;
    doc["t"]      = "kill";
    doc["a"]      = owner;
    doc["target"] = target;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void BombermanGame::broadcastRespawn(const BmPlayer &p) {
    StaticJsonDocument<96> doc;
    doc["t"]  = "respawn";
    doc["id"] = p.id;
    doc["x"]  = p.x;
    doc["y"]  = p.y;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void BombermanGame::broadcastPowerupSpawn(const BmPowerUp &pu) {
    StaticJsonDocument<96> doc;
    doc["t"]    = "pspawn";
    doc["id"]   = pu.id;
    doc["type"] = pu.type;
    doc["x"]    = pu.x;
    doc["y"]    = pu.y;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void BombermanGame::broadcastPowerupPickup(uint8_t id, uint8_t target, uint8_t type) {
    StaticJsonDocument<96> doc;
    doc["t"]      = "ppickup";
    doc["id"]     = id;
    doc["target"] = target;
    doc["type"]   = type;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void BombermanGame::broadcastLeave(uint8_t id) {
    StaticJsonDocument<64> doc;
    doc["t"]  = "leave";
    doc["id"] = id;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

// ---------- host screen ----------

void BombermanGame::drawHostScreen() {
    switch (state_) {
        case GameState::Lobby:   drawLobby();  break;
        case GameState::Running:
        case GameState::Paused:  drawInGame(); break;
        case GameState::Ended:   drawEnd();    break;
    }
}

void BombermanGame::drawLogo(int cx, int cy, int r) {
    int by = cy + r/6;
    M5.Lcd.fillCircle(cx, by, r*3/5, TFT_DARKGREY);                       // bomb body
    M5.Lcd.fillCircle(cx - r/5, by - r/5, r/8, TFT_LIGHTGREY);            // highlight
    M5.Lcd.fillRect(cx - 3, by - r*3/5 - 6, 6, 7, 0x7BEF);                // cap
    M5.Lcd.drawLine(cx, by - r*3/5 - 6, cx + r*2/5, by - r*3/5 - 15, TFT_ORANGE);
    M5.Lcd.fillCircle(cx + r*2/5, by - r*3/5 - 15, 3, TFT_YELLOW);        // spark
}

void BombermanGame::drawLobby() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("BLASTER");
    UI::drawJoinPanel();

    M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(8, 144);
    M5.Lcd.printf("Players %u/%u", (unsigned)playerCount(), Config::MAX_PLAYERS);

    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setTextSize(1);
    int row = 168;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        const BmPlayer &p = players_[i];
        if (!p.active) continue;
        M5.Lcd.setCursor(20, row);
        M5.Lcd.printf("%u. %s", p.id, p.name);
        row += 10;
        if (row > 195) break;
    }

    UI::drawButtons("Start", "Menu", "Pause");
}

void BombermanGame::drawInGame() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("BLASTER");

    uint32_t s = remainingMs() / 1000;
    M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.setCursor(8, 32);
    M5.Lcd.printf("%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));

    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    int row = 76;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        const BmPlayer &p = players_[i];
        if (!p.active) continue;
        M5.Lcd.setCursor(8, row);
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

void BombermanGame::drawEnd() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("ROUND OVER");

    uint8_t wid = winnerId();
    const BmPlayer *w = nullptr;
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
