#include "SnakeGame.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <M5Core2.h>
#include <string.h>

#include "Config.h"
#include "Net.h"
#include "UI.h"

namespace {
constexpr int SCREEN_W = 320;
const float SPAWN_FX[6] = {0.20f, 0.80f, 0.50f, 0.20f, 0.80f, 0.50f};
const float SPAWN_FY[6] = {0.25f, 0.25f, 0.50f, 0.75f, 0.75f, 0.50f};
}

// ---------- lifecycle ----------

void SnakeGame::begin() {
    memset(occ_, 0, sizeof(occ_));
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        snakes_[i] = Snake{};
        snakes_[i].id = i + 1;
    }
    for (uint8_t i = 0; i < Config::SNK_FOOD; ++i) foodActive_[i] = false;
    state_ = GameState::Lobby;
    lastUpdate_ = millis();
    stepAccum_ = 0;
    lastDrawnState_       = (GameState)0xFF;
    lastDrawnPlayerCount_ = 255;
    lastDrawnSec_         = 0xFFFFFFFF;
    for (auto &s : lastDrawnScores_) s = 0;
    Serial.println("[snk] begin");
}

void SnakeGame::end() {
    for (auto &s : snakes_) { s.active = false; s.client = nullptr; }
    state_ = GameState::Lobby;
}

// ---------- helpers ----------

void SnakeGame::clearBody(Snake &s) {
    for (uint16_t i = 0; i < s.len; ++i) {
        uint16_t c = s.body[(s.headIdx + i) % Config::SNK_MAXLEN];
        if (occ_[c] == s.id) occ_[c] = 0;
    }
    s.len = 0;
}

void SnakeGame::spawnSnake(Snake &s) {
    // Default spawn from the per-id fraction, falling back to a free cell.
    uint8_t k = (s.id - 1) % 6;
    int x = (int)(SPAWN_FX[k] * Config::SNK_GW);
    int y = (int)(SPAWN_FY[k] * Config::SNK_GH);
    int cell = cellOf(x, y);
    if (occ_[cell] != 0) cell = freeCell();
    if (cell < 0) cell = cellOf(1, 1);
    s.headIdx = 0;
    s.body[0] = (uint16_t)cell;
    s.len = 1;
    s.dx = s.pdx = (cx(cell) < Config::SNK_GW / 2) ? 1 : -1;
    s.dy = s.pdy = 0;
    s.alive = true;
    s.growBy = 0;
    occ_[cell] = s.id;
}

void SnakeGame::resetSnake(Snake &s) {
    clearBody(s);
    spawnSnake(s);
}

int SnakeGame::freeCell() const {
    for (int tries = 0; tries < 300; ++tries) {
        int c = esp_random() % CELLS;
        if (occ_[c] != 0) continue;
        bool onFood = false;
        for (uint8_t i = 0; i < Config::SNK_FOOD; ++i)
            if (foodActive_[i] && food_[i] == c) { onFood = true; break; }
        if (!onFood) return c;
    }
    return -1;
}

void SnakeGame::spawnFood(uint8_t slot) {
    int c = freeCell();
    if (c < 0) { foodActive_[slot] = false; return; }
    food_[slot] = (uint16_t)c;
    foodActive_[slot] = true;
    broadcastFoodSpawn(cx(c), cy(c));
}

// ---------- tick ----------

void SnakeGame::update(uint32_t now) {
    uint32_t dt = now - lastUpdate_;
    lastUpdate_ = now;

    for (auto &s : snakes_) {
        if (!s.active) continue;
        if ((int32_t)(now - s.lastSeen) > (int32_t)Config::PLAYER_TIMEOUT_MS) {
            clearBody(s);
            s.active = false;
            s.client = nullptr;
            broadcastLeave(s.id);
        }
    }

    if (state_ == GameState::Running) {
        stepAccum_ += dt;
        uint8_t guard = 0;
        while (stepAccum_ >= Config::SNK_STEP_MS && guard++ < 4) {
            stepAccum_ -= Config::SNK_STEP_MS;
            doStep();
        }
        for (auto &s : snakes_) {
            if (s.active && !s.alive && s.respawnAt && now >= s.respawnAt) {
                resetSnake(s);
                s.respawnAt = 0;
                broadcastRespawn(s);
            }
        }
        if (remainingMs() == 0) {
            state_ = GameState::Ended;
            broadcastRound();
        }
    } else {
        stepAccum_ = 0;
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

void SnakeGame::doStep() {
    int  newcell[Config::MAX_PLAYERS];
    bool moving[Config::MAX_PLAYERS] = {false};
    bool dead[Config::MAX_PLAYERS]   = {false};

    // pass 1: new heads + wall deaths
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        Snake &s = snakes_[i];
        newcell[i] = -1;
        if (!s.active || !s.alive) continue;
        moving[i] = true;
        if (!(s.pdx == -s.dx && s.pdy == -s.dy)) { s.dx = s.pdx; s.dy = s.pdy; }
        int nx = cx(s.headCell()) + s.dx;
        int ny = cy(s.headCell()) + s.dy;
        if (nx < 0 || ny < 0 || nx >= Config::SNK_GW || ny >= Config::SNK_GH) dead[i] = true;
        else newcell[i] = cellOf(nx, ny);
    }
    // head-to-head
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        if (!moving[i] || dead[i] || newcell[i] < 0) continue;
        for (uint8_t j = i + 1; j < Config::MAX_PLAYERS; ++j) {
            if (!moving[j] || dead[j]) continue;
            if (newcell[j] == newcell[i]) { dead[i] = dead[j] = true; }
        }
    }
    // body collisions against the snapshot occupancy
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        if (!moving[i] || dead[i] || newcell[i] < 0) continue;
        if (occ_[newcell[i]] != 0) dead[i] = true;
    }
    // apply deaths first so survivors move into a clean grid
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i)
        if (moving[i] && dead[i]) killSnake(snakes_[i]);

    // apply moves
    uint8_t sid[Config::MAX_PLAYERS], shx[Config::MAX_PLAYERS],
            shy[Config::MAX_PLAYERS], sgr[Config::MAX_PLAYERS], n = 0;
    bool anyAte = false;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        if (!moving[i] || dead[i]) continue;
        Snake &s = snakes_[i];
        uint16_t cell = (uint16_t)newcell[i];

        int ate = -1;
        for (uint8_t f = 0; f < Config::SNK_FOOD; ++f)
            if (foodActive_[f] && food_[f] == cell) { ate = f; break; }

        s.headIdx = (s.headIdx + Config::SNK_MAXLEN - 1) % Config::SNK_MAXLEN;
        s.body[s.headIdx] = cell;
        s.len++;
        occ_[cell] = s.id;

        if (ate >= 0) {
            s.score++;
            anyAte = true;
            foodActive_[ate] = false;
            broadcastFoodEat(cx(cell), cy(cell));
            spawnFood(ate);
            s.growBy += Config::SNK_GROW;
        }

        bool kept;
        if (s.growBy > 0 && s.len < Config::SNK_MAXLEN) { s.growBy--; kept = true; }
        else {
            uint16_t tail = s.tailCell();
            if (occ_[tail] == s.id) occ_[tail] = 0;
            s.len--;
            kept = false;
        }

        sid[n] = s.id; shx[n] = cx(cell); shy[n] = cy(cell); sgr[n] = kept ? 1 : 0;
        n++;
    }
    if (n) broadcastStep(sid, shx, shy, sgr, n);
    if (anyAte) broadcastScores();
}

void SnakeGame::killSnake(Snake &s) {
    clearBody(s);
    s.alive = false;
    s.respawnAt = millis() + Config::SNK_RESPAWN_MS;
    broadcastDead(s.id);
}

// ---------- net ----------

void SnakeGame::onClientText(AsyncWebSocketClient *client, const String &text) {
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, text)) return;
    const char *type = doc["t"] | "";

    if (strcmp(type, "join") == 0) {
        const char *name = doc["name"] | "Snake";
        int8_t id = addPlayer(client, name);
        if (id < 0) {
            client->text("{\"t\":\"err\",\"m\":\"full\"}");
            client->close();
            return;
        }
        Snake *s = findById((uint8_t)id);
        if (s) sendInitTo(*s);
        return;
    }

    Snake *s = findByClient(client);
    if (!s) return;
    s->lastSeen = millis();
    if (strcmp(type, "d") == 0) {
        onDir(s->id, (int8_t)(int)(doc["x"] | 0), (int8_t)(int)(doc["y"] | 0));
    } else if (strcmp(type, "ping") == 0) {
        client->text("{\"t\":\"pong\"}");
    }
}

void SnakeGame::onClientDisconnect(AsyncWebSocketClient *client) {
    removeByClient(client);
}

int8_t SnakeGame::addPlayer(AsyncWebSocketClient *client, const char *name) {
    for (auto &s : snakes_) {
        if (!s.active) {
            s.active = true;
            s.client = client;
            strncpy(s.name, name, sizeof(s.name) - 1);
            s.name[sizeof(s.name) - 1] = 0;
            s.score = 0;
            s.lastSeen = millis();
            s.respawnAt = 0;
            s.len = 0;
            if (state_ == GameState::Running) spawnSnake(s);
            else s.alive = false;
            Serial.printf("[snk] join id=%u name=%s\n", s.id, s.name);
            return (int8_t)s.id;
        }
    }
    return -1;
}

void SnakeGame::removeByClient(AsyncWebSocketClient *client) {
    for (auto &s : snakes_) {
        if (s.active && s.client == client) {
            clearBody(s);
            s.active = false;
            s.client = nullptr;
            broadcastLeave(s.id);
            return;
        }
    }
}

Snake *SnakeGame::findByClient(AsyncWebSocketClient *client) {
    for (auto &s : snakes_) if (s.active && s.client == client) return &s;
    return nullptr;
}

Snake *SnakeGame::findById(uint8_t id) {
    if (id == 0 || id > Config::MAX_PLAYERS) return nullptr;
    Snake &s = snakes_[id - 1];
    return s.active ? &s : nullptr;
}

void SnakeGame::onDir(uint8_t id, int8_t dx, int8_t dy) {
    Snake *s = findById(id);
    if (!s) return;
    // normalize to a single cardinal direction
    if (dx) { dx = dx > 0 ? 1 : -1; dy = 0; }
    else if (dy) { dy = dy > 0 ? 1 : -1; dx = 0; }
    else return;
    if (s->len > 1 && dx == -s->dx && dy == -s->dy) return;  // no reversing
    s->pdx = dx; s->pdy = dy;
}

// ---------- match control ----------

void SnakeGame::resetAll() {
    memset(occ_, 0, sizeof(occ_));
    for (auto &s : snakes_) {
        if (!s.active) continue;
        s.score = 0;
        s.len = 0;
        s.respawnAt = 0;
        spawnSnake(s);
    }
    for (uint8_t i = 0; i < Config::SNK_FOOD; ++i) { foodActive_[i] = false; }
    for (uint8_t i = 0; i < Config::SNK_FOOD; ++i) spawnFood(i);
}

void SnakeGame::startRound() {
    resetAll();
    roundStartedAt_ = millis();
    pausedAt_ = 0;
    pausedAccumMs_ = 0;
    stepAccum_ = 0;
    state_ = GameState::Running;
    for (auto &s : snakes_)
        if (s.active && s.client) sendInitTo(s);
    broadcastRound();
    broadcastScores();
    Serial.println("[snk] round start");
}

void SnakeGame::resetRound() {
    state_ = GameState::Lobby;
    roundStartedAt_ = 0;
    pausedAt_ = 0;
    pausedAccumMs_ = 0;
    memset(occ_, 0, sizeof(occ_));
    for (auto &s : snakes_) { if (s.active) { s.len = 0; s.alive = false; s.score = 0; } }
    for (uint8_t i = 0; i < Config::SNK_FOOD; ++i) foodActive_[i] = false;
    broadcastRound();
    broadcastScores();
}

void SnakeGame::togglePause() {
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

uint32_t SnakeGame::remainingMs() const {
    if (state_ != GameState::Running && state_ != GameState::Paused) return Config::ROUND_MS;
    uint32_t now = millis();
    uint32_t pauseExtra = (state_ == GameState::Paused) ? (now - pausedAt_) : 0;
    uint32_t elapsed = now - roundStartedAt_ - pausedAccumMs_ - pauseExtra;
    if (elapsed >= Config::ROUND_MS) return 0;
    return Config::ROUND_MS - elapsed;
}

uint8_t SnakeGame::playerCount() const {
    uint8_t n = 0;
    for (const auto &s : snakes_) if (s.active) ++n;
    return n;
}

uint8_t SnakeGame::winnerId() const {
    int best = -1; uint8_t id = 0;
    for (const auto &s : snakes_) {
        if (!s.active) continue;
        if (s.score > best) { best = s.score; id = s.id; }
    }
    return id;
}

bool SnakeGame::scoresChanged() {
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        int s = snakes_[i].active ? snakes_[i].score : -1;
        if (s != lastDrawnScores_[i]) { lastDrawnScores_[i] = s; return true; }
    }
    return false;
}

// ---------- outbound ----------

void SnakeGame::sendInitTo(const Snake &p) {
    // Full snapshot built manually (bodies can be long; avoids a huge doc).
    String s;
    s.reserve(512);
    s += "{\"t\":\"init\",\"id\":"; s += p.id;
    s += ",\"w\":"; s += Config::SNK_GW;
    s += ",\"h\":"; s += Config::SNK_GH;
    s += ",\"snakes\":[";
    bool first = true;
    for (const auto &sn : snakes_) {
        if (!sn.active) continue;
        if (!first) s += ",";
        first = false;
        s += "{\"id\":"; s += sn.id;
        s += ",\"a\":"; s += (sn.alive ? 1 : 0);
        s += ",\"c\":[";
        for (uint16_t i = 0; i < sn.len; ++i) {
            uint16_t c = sn.body[(sn.headIdx + i) % Config::SNK_MAXLEN];
            if (i) s += ",";
            s += "["; s += cx(c); s += ","; s += cy(c); s += "]";
        }
        s += "]}";
    }
    s += "],\"food\":[";
    bool ff = true;
    for (uint8_t i = 0; i < Config::SNK_FOOD; ++i) {
        if (!foodActive_[i]) continue;
        if (!ff) s += ",";
        ff = false;
        s += "["; s += cx(food_[i]); s += ","; s += cy(food_[i]); s += "]";
    }
    s += "]}";
    Net::sendText(p.client, s);
}

void SnakeGame::broadcastStep(const uint8_t *ids, const uint8_t *hx,
                              const uint8_t *hy, const uint8_t *grew, uint8_t n) {
    StaticJsonDocument<512> doc;
    doc["t"] = "step";
    JsonArray m = doc.createNestedArray("m");
    for (uint8_t i = 0; i < n; ++i) {
        JsonArray r = m.createNestedArray();
        r.add(ids[i]); r.add(hx[i]); r.add(hy[i]); r.add(grew[i]);
    }
    String s; serializeJson(doc, s);
    Net::broadcastText(s);
}

void SnakeGame::broadcastDead(uint8_t id) {
    StaticJsonDocument<48> doc;
    doc["t"] = "dead"; doc["id"] = id;
    String s; serializeJson(doc, s); Net::broadcastText(s);
}

void SnakeGame::broadcastRespawn(const Snake &sn) {
    StaticJsonDocument<96> doc;
    doc["t"] = "respawn"; doc["id"] = sn.id;
    doc["x"] = cx(sn.headCell()); doc["y"] = cy(sn.headCell());
    String s; serializeJson(doc, s); Net::broadcastText(s);
}

void SnakeGame::broadcastFoodSpawn(int x, int y) {
    StaticJsonDocument<48> doc;
    doc["t"] = "food"; doc["x"] = x; doc["y"] = y;
    String s; serializeJson(doc, s); Net::broadcastText(s);
}

void SnakeGame::broadcastFoodEat(int x, int y) {
    StaticJsonDocument<48> doc;
    doc["t"] = "eat"; doc["x"] = x; doc["y"] = y;
    String s; serializeJson(doc, s); Net::broadcastText(s);
}

void SnakeGame::broadcastScores() {
    StaticJsonDocument<512> doc;
    doc["t"] = "score";
    JsonArray arr = doc.createNestedArray("p");
    for (auto &s : snakes_) {
        if (!s.active) continue;
        JsonArray row = arr.createNestedArray();
        row.add(s.id); row.add(s.score); row.add(s.name);
    }
    String out; serializeJson(doc, out); Net::broadcastText(out);
}

void SnakeGame::broadcastRound() {
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
    String s; serializeJson(doc, s); Net::broadcastText(s);
}

void SnakeGame::broadcastLeave(uint8_t id) {
    StaticJsonDocument<48> doc;
    doc["t"] = "leave"; doc["id"] = id;
    String s; serializeJson(doc, s); Net::broadcastText(s);
}

// ---------- host screen ----------

void SnakeGame::drawHostScreen() {
    switch (state_) {
        case GameState::Lobby:   drawLobby();  break;
        case GameState::Running:
        case GameState::Paused:  drawInGame(); break;
        case GameState::Ended:   drawEnd();    break;
    }
}

void SnakeGame::drawLogo(int cx, int cy, int r) {
    int s = r*2/5;
    M5.Lcd.fillRoundRect(cx - 2*s, cy + s, s - 1, s - 1, 2, TFT_GREEN);
    M5.Lcd.fillRoundRect(cx - s,     cy + s, s - 1, s - 1, 2, TFT_GREEN);
    M5.Lcd.fillRoundRect(cx,         cy + s, s - 1, s - 1, 2, TFT_GREEN);
    M5.Lcd.fillRoundRect(cx,         cy,     s - 1, s - 1, 2, TFT_GREEN);
    M5.Lcd.fillRoundRect(cx,         cy - s, s - 1, s - 1, 2, TFT_GREENYELLOW);  // head
    M5.Lcd.fillCircle(cx + s/2, cy - s + s/3, 2, TFT_BLACK);                     // eye
    M5.Lcd.fillCircle(cx - 2*s + s/2, cy - s + s/2, 3, TFT_RED);                 // food
}

void SnakeGame::drawLobby() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("SNAKE ARENA");
    UI::drawJoinPanel();
    M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(8, 144);
    M5.Lcd.printf("Players %u/%u", (unsigned)playerCount(), Config::MAX_PLAYERS);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setTextSize(1);
    int row = 168;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        const Snake &p = snakes_[i];
        if (!p.active) continue;
        M5.Lcd.setCursor(20, row);
        M5.Lcd.printf("%u. %s", p.id, p.name);
        row += 10;
        if (row > 195) break;
    }
    UI::drawButtons("Start", "Menu", "Pause");
}

void SnakeGame::drawInGame() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("SNAKE ARENA");
    uint32_t s = remainingMs() / 1000;
    M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Lcd.setTextSize(3);
    M5.Lcd.setCursor(8, 32);
    M5.Lcd.printf("%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    int row = 76;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i) {
        const Snake &p = snakes_[i];
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

void SnakeGame::drawEnd() {
    M5.Lcd.fillScreen(TFT_BLACK);
    UI::drawHeader("ROUND OVER");
    uint8_t wid = winnerId();
    const Snake *w = nullptr;
    for (uint8_t i = 0; i < Config::MAX_PLAYERS; ++i)
        if (snakes_[i].id == wid && snakes_[i].active) { w = &snakes_[i]; break; }
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
