#pragma once

#include <Arduino.h>

#include "IGame.h"

// Top-level host modes. The Core2 boots into the launcher; selecting a game
// switches to IN_GAME with exactly one active game.
enum class HostMode : uint8_t {
    Launcher = 0,
    InGame   = 1,
};

// Owns the registry of available games and the single active game. The Net
// layer forwards WebSocket events here; the UI layer reads the mode and game
// list to draw the launcher / delegate the in-game screen.
class Host {
public:
    static constexpr uint8_t MAX_GAMES = 8;

    void begin();                 // register games, start in launcher mode
    void update(uint32_t nowMs);  // ticks the active game (if any)

    HostMode mode() const { return mode_; }
    uint8_t  gameCount() const { return count_; }
    IGame   *gameAt(uint8_t i) const { return i < count_ ? games_[i] : nullptr; }
    IGame   *active() const { return active_; }

    void launch(uint8_t index);   // -> IN_GAME, begin() the chosen game
    void stop();                  // -> Launcher, end() the active game

    // Net forwarding (no-ops while in the launcher).
    void onClientConnect(AsyncWebSocketClient *client);
    void onClientText(AsyncWebSocketClient *client, const String &msg);
    void onClientDisconnect(AsyncWebSocketClient *client);

private:
    IGame   *games_[MAX_GAMES] = {nullptr};
    uint8_t  count_  = 0;
    IGame   *active_ = nullptr;
    HostMode mode_   = HostMode::Launcher;
};

extern Host host;
