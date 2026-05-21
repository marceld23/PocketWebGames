#include "PongGame.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <M5Core2.h>
#include <math.h>

#include "Config.h"
#include "Net.h"
#include "UI.h"

namespace {
constexpr int SCREEN_W = 320;

float randf(float lo, float hi) {
    return lo + (float)esp_random() / (float)UINT32_MAX * (hi - lo);
}
void setSpeed(float &vx, float &vy, float sp) {
    float m = sqrtf(vx * vx + vy * vy);
    if (m < 1e-5f) { vx = sp; vy = 0; return; }
    vx *= sp / m; vy *= sp / m;
}
}  // namespace

// ---------- lifecycle ----------

void PongGame::begin() {
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        players_[i] = PongPlayer{};
        players_[i].id = i + 1;
    }
    state_ = GameState::Lobby;
    lastUpdate_ = millis();
    resetBall();
    lastDrawnState_       = (GameState)0xFF;
    lastDrawnPlayerCount_ = 255;
    lastDrawnSec_         = 0xFFFFFFFF;
    for (auto &s : lastDrawnScores_) s = 0;
    Serial.println("[pong] begin");
}

void PongGame::end() {
    for (auto &p : players_) { p.active = false; p.client = nullptr; p.edge = -1; }
    state_ = GameState::Lobby;
}

void PongGame::resetBall() {
    bx_ = 0.5f; by_ = 0.5f;
    float t = randf(0.45f, 1.12f);                 // ~26..64 degrees
    float sx = (esp_random() & 1) ? 1.0f : -1.0f;
    float sy = (esp_random() & 1) ? 1.0f : -1.0f;
    bvx_ = cosf(t) * Config::PONG_BALL_SPEED * sx;
    bvy_ = sinf(t) * Config::PONG_BALL_SPEED * sy;
    lastHit_ = 0;
}

// ---------- tick ----------

void PongGame::update(uint32_t now) {
    float dt = (now - lastUpdate_) / 1000.0f;
    lastUpdate_ = now;
    if (dt > 0.05f) dt = 0.05f;

    for (auto &p : players_) {
        if (!p.active) continue;
        if ((int32_t)(now - p.lastSeen) > (int32_t)Config::PLAYER_TIMEOUT_MS) {
            p.active = false;
            p.client = nullptr;
            p.edge = -1;
            broadcastLeave(p.id);
        }
    }

    if (state_ == GameState::Running) {
        stepBall(dt);
        if (remainingMs() == 0) {
            state_ = GameState::Ended;
            broadcastRound();
        }
    }

    if (now - lastSbBcast_ >= Config::PONG_SB_MS) {
        lastSbBcast_ = now;
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

void PongGame::stepBall(float dt) {
    const float T = Config::PONG_PADDLE_T;
    const float R = Config::PONG_BALL_R;
    const float half = Config::PONG_PADDLE_LEN * 0.5f + R;

    float sp = sqrtf(bvx_ * bvx_ + bvy_ * bvy_);
    int n = (int)ceilf(sp * dt / 0.015f);
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    float h = dt / n;

    for (int k = 0; k < n; ++k) {
        bx_ += bvx_ * h;
        by_ += bvy_ * h;

        // ----- left / right (x) -----
        if (bvx_ < 0 && bx_ <= T) {
            PongPlayer *pl = playerOnEdge(0);
            if (pl) {
                if (fabsf(by_ - pl->v) <= half) {
                    bx_ = T;
                    float ns = fminf(Config::PONG_BALL_MAX, sp * Config::PONG_SPEEDUP);
                    bvx_ = fabsf(bvx_); bvy_ += (by_ - pl->v) * 2.0f;
                    setSpeed(bvx_, bvy_, ns); lastHit_ = pl->id;
                } else if (bx_ <= R) { scoreMiss(pl); return; }
            } else if (bx_ <= R) { bvx_ = fabsf(bvx_); bx_ = R; }
        } else if (bvx_ > 0 && bx_ >= 1 - T) {
            PongPlayer *pl = playerOnEdge(1);
            if (pl) {
                if (fabsf(by_ - pl->v) <= half) {
                    bx_ = 1 - T;
                    float ns = fminf(Config::PONG_BALL_MAX, sp * Config::PONG_SPEEDUP);
                    bvx_ = -fabsf(bvx_); bvy_ += (by_ - pl->v) * 2.0f;
                    setSpeed(bvx_, bvy_, ns); lastHit_ = pl->id;
                } else if (bx_ >= 1 - R) { scoreMiss(pl); return; }
            } else if (bx_ >= 1 - R) { bvx_ = -fabsf(bvx_); bx_ = 1 - R; }
        }

        // ----- top / bottom (y) -----
        if (bvy_ < 0 && by_ <= T) {
            PongPlayer *pl = playerOnEdge(2);
            if (pl) {
                if (fabsf(bx_ - pl->v) <= half) {
                    by_ = T;
                    float ns = fminf(Config::PONG_BALL_MAX, sp * Config::PONG_SPEEDUP);
                    bvy_ = fabsf(bvy_); bvx_ += (bx_ - pl->v) * 2.0f;
                    setSpeed(bvx_, bvy_, ns); lastHit_ = pl->id;
                } else if (by_ <= R) { scoreMiss(pl); return; }
            } else if (by_ <= R) { bvy_ = fabsf(bvy_); by_ = R; }
        } else if (bvy_ > 0 && by_ >= 1 - T) {
            PongPlayer *pl = playerOnEdge(3);
            if (pl) {
                if (fabsf(bx_ - pl->v) <= half) {
                    by_ = 1 - T;
                    float ns = fminf(Config::PONG_BALL_MAX, sp * Config::PONG_SPEEDUP);
                    bvy_ = -fabsf(bvy_); bvx_ += (bx_ - pl->v) * 2.0f;
                    setSpeed(bvx_, bvy_, ns); lastHit_ = pl->id;
                } else if (by_ >= 1 - R) { scoreMiss(pl); return; }
            } else if (by_ >= 1 - R) { bvy_ = -fabsf(bvy_); by_ = 1 - R; }
        }
    }
}

void PongGame::scoreMiss(PongPlayer *missEdgePlayer) {
    uint8_t scorer = 0;
    if (lastHit_ != 0 && missEdgePlayer && lastHit_ != missEdgePlayer->id) {
        PongPlayer *s = findById(lastHit_);
        if (s) { s->score++; scorer = s->id; }
    }
    broadcastPoint(scorer, missEdgePlayer ? missEdgePlayer->id : 0);
    broadcastScores();
    resetBall();
}

// ---------- net ----------

void PongGame::onClientText(AsyncWebSocketClient *client, const String &text) {
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, text)) return;
    const char *type = doc["t"] | "";

    if (strcmp(type, "join") == 0) {
        const char *name = doc["name"] | "Player";
        int8_t id = addPlayer(client, name);
        if (id < 0) {
            client->text("{\"t\":\"err\",\"m\":\"full\"}");
            client->close();
            return;
        }
        PongPlayer *p = findById((uint8_t)id);
        if (p) sendInitTo(*p);
        return;
    }

    PongPlayer *p = findByClient(client);
    if (!p) return;
    p->lastSeen = millis();
    if (strcmp(type, "p") == 0) {
        float v = doc["v"] | 0.5f;
        p->v = v < 0 ? 0 : (v > 1 ? 1 : v);
    } else if (strcmp(type, "ping") == 0) {
        client->text("{\"t\":\"pong\"}");
    }
}

void PongGame::onClientDisconnect(AsyncWebSocketClient *client) {
    removeByClient(client);
}

int8_t PongGame::addPlayer(AsyncWebSocketClient *client, const char *name) {
    // assign the lowest free edge (0..3)
    int8_t edge = -1;
    for (int8_t e = 0; e < Config::PONG_EDGES; ++e) {
        if (!playerOnEdge(e)) { edge = e; break; }
    }
    if (edge < 0) return -1;   // all four edges taken
    for (auto &p : players_) {
        if (!p.active) {
            p.active = true;
            p.client = client;
            strncpy(p.name, name, sizeof(p.name) - 1);
            p.name[sizeof(p.name) - 1] = 0;
            p.edge = edge;
            p.v = 0.5f;
            p.score = 0;
            p.lastSeen = millis();
            Serial.printf("[pong] join id=%u edge=%d name=%s\n", p.id, edge, p.name);
            return (int8_t)p.id;
        }
    }
    return -1;
}

void PongGame::removeByClient(AsyncWebSocketClient *client) {
    for (auto &p : players_) {
        if (p.active && p.client == client) {
            p.active = false;
            p.client = nullptr;
            p.edge = -1;
            broadcastLeave(p.id);
            return;
        }
    }
}

PongPlayer *PongGame::findByClient(AsyncWebSocketClient *client) {
    for (auto &p : players_) if (p.active && p.client == client) return &p;
    return nullptr;
}

PongPlayer *PongGame::findById(uint8_t id) {
    if (id == 0 || id > Config::MAX_PLAYERS) return nullptr;
    PongPlayer &p = players_[id - 1];
    return p.active ? &p : nullptr;
}

PongPlayer *PongGame::playerOnEdge(int8_t edge) {
    for (auto &p : players_) if (p.active && p.edge == edge) return &p;
    return nullptr;
}

// ---------- match control ----------

void PongGame::startRound() {
    for (auto &p : players_) { if (p.active) { p.score = 0; p.v = 0.5f; } }
    roundStartedAt_ = millis();
    pausedAt_ = 0;
    pausedAccumMs_ = 0;
    resetBall();
    state_ = GameState::Running;
    broadcastRound();
    broadcastScores();
    Serial.println("[pong] round start");
}

void PongGame::resetRound() {
    state_ = GameState::Lobby;
    roundStartedAt_ = 0;
    pausedAt_ = 0;
    pausedAccumMs_ = 0;
    for (auto &p : players_) if (p.active) p.score = 0;
    resetBall();
    broadcastRound();
    broadcastScores();
}

void PongGame::togglePause() {
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

uint32_t PongGame::remainingMs() const {
    if (state_ != GameState::Running && state_ != GameState::Paused) return Config::ROUND_MS;
    uint32_t now = millis();
    uint32_t pauseExtra = (state_ == GameState::Paused) ? (now - pausedAt_) : 0;
    uint32_t elapsed = now - roundStartedAt_ - pausedAccumMs_ - pauseExtra;
    if (elapsed >= Config::ROUND_MS) return 0;
    return Config::ROUND_MS - elapsed;
}

uint8_t PongGame::playerCount() const {
    uint8_t n = 0;
    for (const auto &p : players_) if (p.active) ++n;
    return n;
}

uint8_t PongGame::winnerId() const {
    int best = -1; uint8_t id = 0;
    for (const auto &p : players_) {
        if (!p.active) continue;
        if (p.score > best) { best = p.score; id = p.id; }
    }
    return id;
}

bool PongGame::scoresChanged() {
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        int s = players_[i].active ? players_[i].score : -1;
        if (s != lastDrawnScores_[i]) { lastDrawnScores_[i] = s; return true; }
    }
    return false;
}

// ---------- outbound ----------

void PongGame::sendInitTo(const PongPlayer &p) {
    StaticJsonDocument<96> doc;
    doc["t"]    = "init";
    doc["id"]   = p.id;
    doc["edge"] = p.edge;
    doc["plen"] = Config::PONG_PADDLE_LEN;
    doc["pt"]   = Config::PONG_PADDLE_T;
    String s; serializeJson(doc, s);
    Net::sendText(p.client, s);
}

void PongGame::broadcastStates() {
    if (playerCount() == 0) return;
    StaticJsonDocument<512> doc;
    doc["t"] = "sb";
    JsonArray b = doc.createNestedArray("b");
    b.add(bx_); b.add(by_);
    JsonArray pads = doc.createNestedArray("pd");
    for (auto &p : players_) {
        if (!p.active) continue;
        JsonArray r = pads.createNestedArray();
        r.add(p.id); r.add(p.edge); r.add(p.v);
    }
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void PongGame::broadcastPoint(uint8_t scorer, uint8_t on) {
    StaticJsonDocument<64> doc;
    doc["t"] = "point"; doc["scorer"] = scorer; doc["on"] = on;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void PongGame::broadcastScores() {
    StaticJsonDocument<512> doc;
    doc["t"] = "score";
    JsonArray arr = doc.createNestedArray("p");
    for (auto &p : players_) {
        if (!p.active) continue;
        JsonArray row = arr.createNestedArray();
        row.add(p.id); row.add(p.score); row.add(p.name);
    }
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void PongGame::broadcastRound() {
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

void PongGame::broadcastLeave(uint8_t id) {
    StaticJsonDocument<48> doc;
    doc["t"] = "leave"; doc["id"] = id;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

// ---------- host screen ----------

void PongGame::drawHostScreen() {
    switch (state_) {
        case GameState::Lobby:   drawLobby();  break;
        case GameState::Running:
        case GameState::Paused:  drawInGame(); break;
        case GameState::Ended:   drawEnd();    break;
    }
}

void PongGame::drawLogo(int cx, int cy, int r) {
    for (int y = cy - r; y < cy + r; y += 8) M5.Lcd.fillRect(cx - 1, y, 2, 4, TFT_DARKGREY);
    M5.Lcd.fillRect(cx - r, cy - r*3/10, 5, r*3/5, TFT_CYAN);    // left paddle
    M5.Lcd.fillRect(cx + r - 5, cy - r/10, 5, r*3/5, TFT_RED);   // right paddle
    M5.Lcd.fillCircle(cx + r/5, cy, 5, TFT_YELLOW);              // ball
}

void PongGame::drawLobby() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("BOUNCE");
    UI::drawJoinPanel();
    M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(8, 144);
    M5.Lcd.printf("Players %u/%u", (unsigned)playerCount(), Config::PONG_EDGES);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setTextSize(1);
    int row = 168;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        const PongPlayer &p = players_[i];
        if (!p.active) continue;
        M5.Lcd.setCursor(20, row);
        M5.Lcd.printf("%u. %s", p.id, p.name);
        row += 10;
        if (row > 195) break;
    }
    UI::drawButtons("Start", "Menu", "Pause");
}

void PongGame::drawInGame() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("BOUNCE");
    uint32_t s = remainingMs() / 1000;
    M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.setCursor(8, 32);
    M5.Lcd.printf("%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    int row = 76;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        const PongPlayer &p = players_[i];
        if (!p.active) continue;
        M5.Lcd.setCursor(8, row);
        M5.Lcd.printf("%u", p.id);
        M5.Lcd.setCursor(28, row);
        M5.Lcd.print(p.name);
        char sc[8];
        snprintf(sc, sizeof(sc), "%d", p.score);
        int w = (int)strlen(sc) * 12;
        M5.Lcd.setCursor(SCREEN_W - w - 12, row);
        M5.Lcd.print(sc);
        row += 22;
        if (row > 190) break;
    }
    UI::drawButtons("Start", "Menu",
                    state_ == GameState::Paused ? "Resume" : "Pause");
}

void PongGame::drawEnd() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("ROUND OVER");
    uint8_t wid = winnerId();
    const PongPlayer *w = nullptr;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i)
        if (players_[i].id == wid && players_[i].active) { w = &players_[i]; break; }
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
