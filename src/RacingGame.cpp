#include "RacingGame.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <M5Core2.h>
#include <math.h>

#include "Config.h"
#include "Net.h"
#include "UI.h"

namespace { constexpr int SCREEN_W = 320; }

// ---------- lifecycle ----------

void RacingGame::begin() {
    buildTrack();
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        players_[i] = RcPlayer{};
        players_[i].id = i + 1;
    }
    state_ = GameState::Lobby;
    winner_ = 0;
    lastDrawnState_       = (GameState)0xFF;
    lastDrawnPlayerCount_ = 255;
    lastDrawnSec_         = 0xFFFFFFFF;
    for (auto &s : lastDrawnScores_) s = 0;
    Serial.println("[race] begin");
}

void RacingGame::end() {
    for (auto &p : players_) { p.active = false; p.client = nullptr; }
    state_ = GameState::Lobby;
}

void RacingGame::buildTrack() {
    for (uint8_t i = 0; i < Config::RACE_N; ++i) {
        float a = (float)i / Config::RACE_N * 2.0f * (float)M_PI;
        float rx = 1.0f + 0.18f * sinf(3 * a);
        float rz = 1.0f + 0.10f * cosf(2 * a);
        trackX_[i] = cosf(a) * Config::RACE_RX * rx;
        trackZ_[i] = sinf(a) * Config::RACE_RZ * rz;
    }
}

void RacingGame::startPose(uint8_t id, float &x, float &z, float &h) const {
    float fx = trackX_[1] - trackX_[0];
    float fz = trackZ_[1] - trackZ_[0];
    float fl = sqrtf(fx * fx + fz * fz);
    if (fl < 1e-3f) fl = 1;
    fx /= fl; fz /= fl;
    float rx = fz, rz = -fx;           // right vector
    uint8_t row = (id - 1) / 2;
    float lane = ((id - 1) % 2) * 8.0f - 4.0f;
    x = trackX_[0] - fx * (8.0f + row * 7.0f) + rx * lane;
    z = trackZ_[0] - fz * (8.0f + row * 7.0f) + rz * lane;
    h = atan2f(fx, fz);
}

// ---------- tick ----------

void RacingGame::update(uint32_t now) {
    for (auto &p : players_) {
        if (!p.active) continue;
        if ((int32_t)(now - p.lastSeen) > (int32_t)Config::PLAYER_TIMEOUT_MS) {
            p.active = false;
            p.client = nullptr;
            broadcastLeave(p.id);
        }
    }

    if (state_ == GameState::Running && remainingMs() == 0) {
        state_ = GameState::Ended;
        winner_ = winnerId();
        broadcastRound();
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

void RacingGame::onClientText(AsyncWebSocketClient *client, const String &text) {
    StaticJsonDocument<192> doc;
    if (deserializeJson(doc, text)) return;
    const char *type = doc["t"] | "";

    if (strcmp(type, "join") == 0) {
        const char *name = doc["name"] | "Racer";
        int8_t id = addPlayer(client, name);
        if (id < 0) {
            client->text("{\"t\":\"err\",\"m\":\"full\"}");
            client->close();
            return;
        }
        RcPlayer *p = findById((uint8_t)id);
        if (p) sendInitTo(*p);
        return;
    }

    RcPlayer *p = findByClient(client);
    if (!p) return;
    p->lastSeen = millis();
    if (strcmp(type, "s") == 0) {
        onState(p->id, doc["x"] | 0.0f, doc["z"] | 0.0f, doc["h"] | 0.0f);
    } else if (strcmp(type, "ping") == 0) {
        client->text("{\"t\":\"pong\"}");
    }
}

void RacingGame::onClientDisconnect(AsyncWebSocketClient *client) {
    removeByClient(client);
}

int8_t RacingGame::addPlayer(AsyncWebSocketClient *client, const char *name) {
    for (auto &p : players_) {
        if (!p.active) {
            p.active = true;
            p.client = client;
            strncpy(p.name, name, sizeof(p.name) - 1);
            p.name[sizeof(p.name) - 1] = 0;
            p.lap = 0;
            p.nextCp = 1;
            p.lastSeen = millis();
            startPose(p.id, p.x, p.z, p.h);
            Serial.printf("[race] join id=%u name=%s\n", p.id, p.name);
            return (int8_t)p.id;
        }
    }
    return -1;
}

void RacingGame::removeByClient(AsyncWebSocketClient *client) {
    for (auto &p : players_) {
        if (p.active && p.client == client) {
            p.active = false;
            p.client = nullptr;
            broadcastLeave(p.id);
            return;
        }
    }
}

RcPlayer *RacingGame::findByClient(AsyncWebSocketClient *client) {
    for (auto &p : players_) if (p.active && p.client == client) return &p;
    return nullptr;
}

RcPlayer *RacingGame::findById(uint8_t id) {
    if (id == 0 || id > Config::MAX_PLAYERS) return nullptr;
    RcPlayer &p = players_[id - 1];
    return p.active ? &p : nullptr;
}

void RacingGame::onState(uint8_t id, float x, float z, float h) {
    RcPlayer *p = findById(id);
    if (!p) return;
    p->x = x; p->z = z; p->h = h;

    if (state_ != GameState::Running) return;

    float dx = x - trackX_[p->nextCp];
    float dz = z - trackZ_[p->nextCp];
    if (dx * dx + dz * dz < Config::RACE_CP_RADIUS * Config::RACE_CP_RADIUS) {
        if (p->nextCp == 0) {
            p->lap++;
            p->nextCp = 1;
            broadcastLap(p->id, p->lap);
            broadcastScores();
            if (p->lap >= Config::RACE_LAPS) {
                state_ = GameState::Ended;
                winner_ = p->id;
                broadcastRound();
            }
        } else {
            p->nextCp = (p->nextCp + 1) % Config::RACE_N;
        }
    }
}

void RacingGame::resetAllStats() {
    for (auto &p : players_) {
        if (!p.active) continue;
        p.lap = 0;
        p.nextCp = 1;
        startPose(p.id, p.x, p.z, p.h);
    }
}

// ---------- match control ----------

void RacingGame::startRound() {
    buildTrack();
    resetAllStats();
    roundStartedAt_ = millis();
    pausedAt_ = 0;
    pausedAccumMs_ = 0;
    winner_ = 0;
    state_ = GameState::Running;
    for (auto &p : players_)
        if (p.active && p.client) sendInitTo(p);
    broadcastRound();
    Serial.println("[race] round start");
}

void RacingGame::resetRound() {
    state_ = GameState::Lobby;
    roundStartedAt_ = 0;
    pausedAt_ = 0;
    pausedAccumMs_ = 0;
    winner_ = 0;
    resetAllStats();
    broadcastRound();
    broadcastScores();
}

void RacingGame::togglePause() {
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

uint32_t RacingGame::remainingMs() const {
    if (state_ != GameState::Running && state_ != GameState::Paused) return Config::ROUND_MS;
    uint32_t now = millis();
    uint32_t pauseExtra = (state_ == GameState::Paused) ? (now - pausedAt_) : 0;
    uint32_t elapsed = now - roundStartedAt_ - pausedAccumMs_ - pauseExtra;
    if (elapsed >= Config::ROUND_MS) return 0;
    return Config::ROUND_MS - elapsed;
}

uint8_t RacingGame::playerCount() const {
    uint8_t n = 0;
    for (const auto &p : players_) if (p.active) ++n;
    return n;
}

uint8_t RacingGame::winnerId() const {
    if (winner_) return winner_;
    int best = -1; uint8_t id = 0;
    for (const auto &p : players_) {
        if (!p.active) continue;
        if (p.lap > best) { best = p.lap; id = p.id; }
    }
    return id;
}

bool RacingGame::scoresChanged() {
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        int s = players_[i].active ? players_[i].lap : -1;
        if (s != lastDrawnScores_[i]) { lastDrawnScores_[i] = s; return true; }
    }
    return false;
}

// ---------- outbound ----------

void RacingGame::sendInitTo(const RcPlayer &p) {
    StaticJsonDocument<1536> doc;
    doc["t"]     = "init";
    doc["id"]    = p.id;
    doc["width"] = Config::RACE_WIDTH;
    doc["laps"]  = Config::RACE_LAPS;
    doc["sx"]    = p.x;
    doc["sz"]    = p.z;
    doc["sh"]    = p.h;
    JsonArray tr = doc.createNestedArray("track");
    for (uint8_t i = 0; i < Config::RACE_N; ++i) {
        JsonArray pt = tr.createNestedArray();
        pt.add(trackX_[i]);
        pt.add(trackZ_[i]);
    }
    String s; serializeJson(doc, s);
    Net::sendText(p.client, s);
}

void RacingGame::broadcastStates() {
    if (playerCount() == 0) return;
    StaticJsonDocument<768> doc;
    doc["t"] = "sb";
    JsonArray arr = doc.createNestedArray("p");
    for (auto &p : players_) {
        if (!p.active) continue;
        JsonObject o = arr.createNestedObject();
        o["id"] = p.id;
        o["x"]  = p.x;
        o["z"]  = p.z;
        o["h"]  = p.h;
        o["l"]  = p.lap;
    }
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void RacingGame::broadcastLap(uint8_t id, int lap) {
    StaticJsonDocument<64> doc;
    doc["t"]   = "lap";
    doc["id"]  = id;
    doc["lap"] = lap;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void RacingGame::broadcastScores() {
    StaticJsonDocument<512> doc;
    doc["t"] = "score";
    JsonArray arr = doc.createNestedArray("p");
    for (auto &p : players_) {
        if (!p.active) continue;
        JsonArray row = arr.createNestedArray();
        row.add(p.id);
        row.add(p.lap);
        row.add(p.name);
    }
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void RacingGame::broadcastRound() {
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

void RacingGame::broadcastLeave(uint8_t id) {
    StaticJsonDocument<48> doc;
    doc["t"]  = "leave";
    doc["id"] = id;
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

// ---------- host screen ----------

void RacingGame::drawHostScreen() {
    switch (state_) {
        case GameState::Lobby:   drawLobby();  break;
        case GameState::Running:
        case GameState::Paused:  drawInGame(); break;
        case GameState::Ended:   drawEnd();    break;
    }
}

void RacingGame::drawLogo(int cx, int cy, int r) {
    M5.Lcd.fillCircle(cx - r*11/20, cy + r*9/20, r/5, TFT_BLACK);          // wheels
    M5.Lcd.fillCircle(cx + r*11/20, cy + r*9/20, r/5, TFT_BLACK);
    M5.Lcd.fillRoundRect(cx - r, cy - r/5, 2*r, r*7/10, 6, TFT_RED);        // body
    M5.Lcd.fillRoundRect(cx - r*2/5, cy - r*3/5, r*4/5, r*9/20, 4, 0x2D7F); // cabin
}

void RacingGame::drawLobby() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("3D RACING");
    UI::drawJoinPanel();
    M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(8, 144);
    M5.Lcd.printf("Players %u/%u", (unsigned)playerCount(), Config::MAX_PLAYERS);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setTextSize(1);
    int row = 168;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        const RcPlayer &p = players_[i];
        if (!p.active) continue;
        M5.Lcd.setCursor(20, row);
        M5.Lcd.printf("%u. %s", p.id, p.name);
        row += 10;
        if (row > 195) break;
    }
    UI::drawButtons("Start", "Menu", "Pause");
}

void RacingGame::drawInGame() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("3D RACING");
    uint32_t s = remainingMs() / 1000;
    M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.setCursor(8, 32);
    M5.Lcd.printf("%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    int row = 76;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        const RcPlayer &p = players_[i];
        if (!p.active) continue;
        M5.Lcd.setCursor(8, row);
        M5.Lcd.printf("%u", p.id);
        M5.Lcd.setCursor(28, row);
        M5.Lcd.print(p.name);
        char lap[12];
        snprintf(lap, sizeof(lap), "L%d/%d", p.lap, Config::RACE_LAPS);
        int w = (int)strlen(lap) * 12;
        M5.Lcd.setCursor(SCREEN_W - w - 12, row);
        M5.Lcd.print(lap);
        row += 22;
        if (row > 190) break;
    }
    UI::drawButtons("Start", "Menu",
                    state_ == GameState::Paused ? "Resume" : "Pause");
}

void RacingGame::drawEnd() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("RACE OVER");
    uint8_t wid = winnerId();
    const RcPlayer *w = nullptr;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i)
        if (players_[i].id == wid && players_[i].active) { w = &players_[i]; break; }
    M5.Lcd.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.setCursor(8, 48);
    if (w) M5.Lcd.printf("Winner: %s", w->name);
    else   M5.Lcd.print("No winner");
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(8, 96);
    if (w) M5.Lcd.printf("Laps: %d", w->lap);
    UI::drawButtons("NewRnd", "Menu", "");
}
