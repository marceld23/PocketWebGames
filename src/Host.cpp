#include "Host.h"

#include <Arduino.h>

#include "BombermanGame.h"
#include "CurveFeverGame.h"
#include "DogfightGame.h"
#include "PongGame.h"
#include "RacingGame.h"
#include "SnakeGame.h"
#include "Net.h"
#include "UI.h"

// Statically allocated games. Each game struct is only a few KB, so keeping
// every registered game resident (rather than heap-allocating on launch)
// avoids fragmentation and is well within the Core2's RAM budget.
static DogfightGame   dogfightGame_;
static BombermanGame  bombermanGame_;
static CurveFeverGame curveFeverGame_;
static RacingGame     racingGame_;
static SnakeGame      snakeGame_;
static PongGame       pongGame_;

Host host;

void Host::begin() {
    count_ = 0;
    games_[count_++] = &dogfightGame_;
    games_[count_++] = &bombermanGame_;
    games_[count_++] = &curveFeverGame_;
    games_[count_++] = &racingGame_;
    games_[count_++] = &snakeGame_;
    games_[count_++] = &pongGame_;
    // Add future games here, e.g. games_[count_++] = &someOtherGame_;
    active_ = nullptr;
    mode_   = HostMode::Launcher;
    Serial.printf("[host] %u game(s) registered, launcher mode\n", count_);
}

void Host::update(uint32_t now) {
    if (mode_ == HostMode::InGame && active_) {
        active_->update(now);
    }
}

void Host::launch(uint8_t index) {
    if (index >= count_) return;
    if (active_) active_->end();
    // Drop any browsers connected to the previous game / launcher page so they
    // reconnect and fetch the newly active game's assets.
    Net::closeAllClients();
    active_ = games_[index];
    mode_   = HostMode::InGame;
    active_->begin();
    Serial.printf("[host] launch '%s' (slug=%s)\n", active_->name(), active_->slug());
    UI::invalidate();
}

void Host::stop() {
    if (active_) active_->end();
    Net::closeAllClients();
    active_ = nullptr;
    mode_   = HostMode::Launcher;
    Serial.println("[host] back to launcher");
    UI::invalidate();
}

void Host::onClientConnect(AsyncWebSocketClient *client) {
    if (mode_ == HostMode::InGame && active_) active_->onClientConnect(client);
}

void Host::onClientText(AsyncWebSocketClient *client, const String &msg) {
    if (mode_ == HostMode::InGame && active_) active_->onClientText(client, msg);
}

void Host::onClientDisconnect(AsyncWebSocketClient *client) {
    if (active_) active_->onClientDisconnect(client);
}
