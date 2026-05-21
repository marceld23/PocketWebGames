#include "CurveFeverGame.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <M5Core2.h>
#include <math.h>
#include <string.h>

#include "Config.h"
#include "Net.h"
#include "UI.h"

namespace { constexpr int SCREEN_W = 320; }

// ---------- lifecycle ----------

void CurveFeverGame::begin() {
    memset(grid_, 0, sizeof(grid_));
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        players_[i] = CfPlayer{};
        players_[i].id = i + 1;
    }
    state_ = GameState::Lobby;
    miniActive_ = false;
    miniRestartAt_ = 0;
    lastUpdate_ = millis();
    lastDrawnState_       = (GameState)0xFF;
    lastDrawnPlayerCount_ = 255;
    lastDrawnSec_         = 0xFFFFFFFF;
    for (auto &s : lastDrawnScores_) s = 0;
    Serial.println("[cf] begin");
}

void CurveFeverGame::end() {
    for (auto &p : players_) { p.active = false; p.client = nullptr; }
    state_ = GameState::Lobby;
    miniActive_ = false;
}

// ---------- tick ----------

void CurveFeverGame::update(uint32_t now) {
    float dt = (now - lastUpdate_) / 1000.0f;
    lastUpdate_ = now;
    if (dt > 0.05f) dt = 0.05f;

    // Drop stale clients.
    for (auto &p : players_) {
        if (!p.active) continue;
        if ((int32_t)(now - p.lastSeen) > (int32_t)Config::PLAYER_TIMEOUT_MS) {
            p.active = false;
            p.client = nullptr;
            broadcastLeave(p.id);
        }
    }

    if (state_ == GameState::Running) {
        if (miniActive_) {
            for (auto &p : players_)
                if (p.active && p.alive) integrate(p, now, dt);

            uint8_t na = playerCount();
            uint8_t al = aliveCount();
            if ((na >= 2 && al <= 1) || (na <= 1 && al == 0)) {
                if (al == 1) {
                    for (auto &p : players_)
                        if (p.active && p.alive) { p.score++; break; }
                    broadcastScores();
                }
                miniActive_ = false;
                miniRestartAt_ = now + Config::CF_MINIROUND_GAP_MS;
            }
        } else if (now >= miniRestartAt_) {
            if (playerCount() >= 1) startMiniRound(now);
            else miniRestartAt_ = now + Config::CF_MINIROUND_GAP_MS;
        }

        if (remainingMs() == 0) {
            state_ = GameState::Ended;
            miniActive_ = false;
            broadcastRound();
        }
    }

    if (now - lastStateBcast_ >= Config::CF_STATE_MS) {
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

// ---------- net ----------

void CurveFeverGame::onClientText(AsyncWebSocketClient *client, const String &text) {
    StaticJsonDocument<192> doc;
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
        CfPlayer *p = findById((uint8_t)id);
        if (p) sendInitTo(*p);
        return;
    }

    CfPlayer *p = findByClient(client);
    if (!p) return;
    p->lastSeen = millis();   // any message keeps the player alive in the lobby
    if (strcmp(type, "i") == 0) {
        onInput(p->id, (int8_t)(int)(doc["d"] | 0));
    } else if (strcmp(type, "ping") == 0) {
        client->text("{\"t\":\"pong\"}");
    }
}

void CurveFeverGame::onClientDisconnect(AsyncWebSocketClient *client) {
    removeByClient(client);
}

int8_t CurveFeverGame::addPlayer(AsyncWebSocketClient *client, const char *name) {
    uint32_t now = millis();
    for (auto &p : players_) {
        if (!p.active) {
            p.active = true;
            p.client = client;
            strncpy(p.name, name, sizeof(p.name) - 1);
            p.name[sizeof(p.name) - 1] = 0;
            p.score = 0;
            p.turn = 0;
            p.lastSeen = now;
            spawnFor(p.id, p.x, p.y, p.heading);
            if (state_ == GameState::Running && miniActive_) {
                p.alive = true;
                p.graceUntil = now + Config::CF_SPAWN_GRACE_MS;
                p.gapUntil = 0;
                p.nextGap = now + Config::CF_GAP_INTERVAL_MS;
                p.cellX = (int)floorf(p.x);
                p.cellY = (int)floorf(p.y);
                grid_[gidx(p.cellX, p.cellY)] = p.id;
            } else {
                p.alive = false;
            }
            Serial.printf("[cf] join id=%u name=%s\n", p.id, p.name);
            return (int8_t)p.id;
        }
    }
    return -1;
}

void CurveFeverGame::removeByClient(AsyncWebSocketClient *client) {
    for (auto &p : players_) {
        if (p.active && p.client == client) {
            p.active = false;
            p.client = nullptr;
            broadcastLeave(p.id);
            return;
        }
    }
}

CfPlayer *CurveFeverGame::findByClient(AsyncWebSocketClient *client) {
    for (auto &p : players_) if (p.active && p.client == client) return &p;
    return nullptr;
}

CfPlayer *CurveFeverGame::findById(uint8_t id) {
    if (id == 0 || id > Config::MAX_PLAYERS) return nullptr;
    CfPlayer &p = players_[id - 1];
    return p.active ? &p : nullptr;
}

void CurveFeverGame::onInput(uint8_t id, int8_t dir) {
    CfPlayer *p = findById(id);
    if (!p) return;
    p->turn = dir < 0 ? -1 : (dir > 0 ? 1 : 0);
    p->lastSeen = millis();
}

// ---------- simulation ----------

void CurveFeverGame::spawnFor(uint8_t id, float &x, float &y, float &heading) const {
    static const float fx[6] = {0.22f, 0.78f, 0.50f, 0.22f, 0.78f, 0.50f};
    static const float fy[6] = {0.25f, 0.25f, 0.50f, 0.75f, 0.75f, 0.50f};
    uint8_t i = (id - 1) % 6;
    x = fx[i] * Config::CF_GW;
    y = fy[i] * Config::CF_GH;
    heading = atan2f(Config::CF_GH * 0.5f - y, Config::CF_GW * 0.5f - x);
}

void CurveFeverGame::startMiniRound(uint32_t now) {
    memset(grid_, 0, sizeof(grid_));
    for (auto &p : players_) {
        if (!p.active) continue;
        spawnFor(p.id, p.x, p.y, p.heading);
        p.turn = 0;
        p.alive = true;
        p.gapUntil = 0;
        p.nextGap = now + Config::CF_GAP_INTERVAL_MS;
        p.graceUntil = now + Config::CF_SPAWN_GRACE_MS;
        p.cellX = (int)floorf(p.x);
        p.cellY = (int)floorf(p.y);
        grid_[gidx(p.cellX, p.cellY)] = p.id;
    }
    miniActive_ = true;
    miniRestartAt_ = 0;
    broadcastClear();
    broadcastStates();
}

void CurveFeverGame::integrate(CfPlayer &p, uint32_t now, float dt) {
    p.heading += p.turn * Config::CF_TURN * dt;
    p.x += cosf(p.heading) * Config::CF_SPEED * dt;
    p.y += sinf(p.heading) * Config::CF_SPEED * dt;

    int cx = (int)floorf(p.x);
    int cy = (int)floorf(p.y);
    if (cx < 0 || cy < 0 || cx >= Config::CF_GW || cy >= Config::CF_GH) {
        killPlayer(p);
        return;
    }

    if (now >= p.nextGap) {
        p.gapUntil = now + Config::CF_GAP_LEN_MS;
        p.nextGap  = now + Config::CF_GAP_INTERVAL_MS;
    }

    // Only react when the head crosses into a NEW cell — never collide with
    // the cell we are already standing on (our own freshly-laid trail).
    if (cx == p.cellX && cy == p.cellY) return;

    bool inGap = now < p.gapUntil;
    bool grace = now < p.graceUntil;
    if (!grace && occupied(cx, cy)) {
        killPlayer(p);
        return;
    }
    if (!inGap) grid_[gidx(cx, cy)] = p.id;
    p.cellX = cx;
    p.cellY = cy;
}

void CurveFeverGame::killPlayer(CfPlayer &p) {
    if (!p.alive) return;
    p.alive = false;
    broadcastDead(p.id);
}

uint8_t CurveFeverGame::aliveCount() const {
    uint8_t n = 0;
    for (const auto &p : players_) if (p.active && p.alive) ++n;
    return n;
}

void CurveFeverGame::resetAllStats() {
    for (auto &p : players_) {
        if (!p.active) continue;
        p.score = 0;
        p.alive = false;
        p.turn = 0;
    }
}

// ---------- match control ----------

void CurveFeverGame::startRound() {
    resetAllStats();
    memset(grid_, 0, sizeof(grid_));
    roundStartedAt_ = millis();
    pausedAt_ = 0;
    pausedAccumMs_ = 0;
    miniActive_ = false;
    miniRestartAt_ = millis();   // first mini-round starts on the next tick
    state_ = GameState::Running;
    broadcastRound();
    Serial.println("[cf] round start");
}

void CurveFeverGame::resetRound() {
    state_ = GameState::Lobby;
    roundStartedAt_ = 0;
    pausedAt_ = 0;
    pausedAccumMs_ = 0;
    miniActive_ = false;
    resetAllStats();
    memset(grid_, 0, sizeof(grid_));
    broadcastClear();
    broadcastRound();
    broadcastScores();
}

void CurveFeverGame::togglePause() {
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

uint32_t CurveFeverGame::remainingMs() const {
    if (state_ != GameState::Running && state_ != GameState::Paused) return Config::ROUND_MS;
    uint32_t now = millis();
    uint32_t pauseExtra = (state_ == GameState::Paused) ? (now - pausedAt_) : 0;
    uint32_t elapsed = now - roundStartedAt_ - pausedAccumMs_ - pauseExtra;
    if (elapsed >= Config::ROUND_MS) return 0;
    return Config::ROUND_MS - elapsed;
}

uint8_t CurveFeverGame::playerCount() const {
    uint8_t n = 0;
    for (const auto &p : players_) if (p.active) ++n;
    return n;
}

uint8_t CurveFeverGame::winnerId() const {
    int best = -1; uint8_t id = 0;
    for (const auto &p : players_) {
        if (!p.active) continue;
        if (p.score > best) { best = p.score; id = p.id; }
    }
    return id;
}

bool CurveFeverGame::scoresChanged() {
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        int s = players_[i].active ? players_[i].score : -1;
        if (s != lastDrawnScores_[i]) { lastDrawnScores_[i] = s; return true; }
    }
    return false;
}

// ---------- outbound ----------

void CurveFeverGame::sendInitTo(const CfPlayer &p) {
    StaticJsonDocument<128> doc;
    doc["t"]  = "init";
    doc["id"] = p.id;
    doc["w"]  = Config::CF_GW;
    doc["h"]  = Config::CF_GH;
    String s; serializeJson(doc, s);
    Net::sendText(p.client, s);
}

void CurveFeverGame::broadcastClear() {
    Net::broadcastText("{\"t\":\"clear\"}");
}

void CurveFeverGame::broadcastStates() {
    if (playerCount() == 0) return;
    uint32_t now = millis();
    StaticJsonDocument<768> doc;
    doc["t"] = "sb";
    JsonArray arr = doc.createNestedArray("p");
    for (auto &p : players_) {
        if (!p.active) continue;
        JsonObject o = arr.createNestedObject();
        o["id"] = p.id;
        o["x"]  = p.x;
        o["y"]  = p.y;
        o["a"]  = p.alive ? 1 : 0;
        o["g"]  = (now < p.gapUntil) ? 1 : 0;
    }
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void CurveFeverGame::broadcastDead(uint8_t id) {
    StaticJsonDocument<48> doc;
    doc["t"]  = "dead";
    doc["id"] = id;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void CurveFeverGame::broadcastScores() {
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

void CurveFeverGame::broadcastRound() {
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

void CurveFeverGame::broadcastLeave(uint8_t id) {
    StaticJsonDocument<48> doc;
    doc["t"]  = "leave";
    doc["id"] = id;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

// ---------- host screen ----------

void CurveFeverGame::drawHostScreen() {
    switch (state_) {
        case GameState::Lobby:   drawLobby();  break;
        case GameState::Running:
        case GameState::Paused:  drawInGame(); break;
        case GameState::Ended:   drawEnd();    break;
    }
}

void CurveFeverGame::drawLogo(int cx, int cy, int r) {
    int prevx = cx - r, prevy = cy;
    for (int i = 1; i <= 24; ++i) {
        float t = i / 24.0f;
        int x = cx - r + (int)(t * 2 * r);
        int y = cy + (int)(sinf(t * 6.2832f) * r * 0.5f);
        M5.Lcd.drawLine(prevx, prevy, x, y, TFT_CYAN);
        M5.Lcd.drawLine(prevx, prevy + 1, x, y + 1, TFT_CYAN);
        prevx = x; prevy = y;
    }
    M5.Lcd.fillCircle(prevx, prevy, 4, TFT_WHITE);   // head
}

void CurveFeverGame::drawLobby() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("TRAILS");
    UI::drawJoinPanel();
    M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(8, 144);
    M5.Lcd.printf("Players %u/%u", (unsigned)playerCount(), Config::MAX_PLAYERS);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setTextSize(1);
    int row = 168;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        const CfPlayer &p = players_[i];
        if (!p.active) continue;
        M5.Lcd.setCursor(20, row);
        M5.Lcd.printf("%u. %s", p.id, p.name);
        row += 10;
        if (row > 195) break;
    }
    UI::drawButtons("Start", "Menu", "Pause");
}

void CurveFeverGame::drawInGame() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("TRAILS");
    uint32_t s = remainingMs() / 1000;
    M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.setCursor(8, 32);
    M5.Lcd.printf("%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    int row = 76;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        const CfPlayer &p = players_[i];
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

void CurveFeverGame::drawEnd() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("ROUND OVER");
    uint8_t wid = winnerId();
    const CfPlayer *w = nullptr;
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
