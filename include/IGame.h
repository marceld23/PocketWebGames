#pragma once

#include <Arduino.h>

class AsyncWebSocketClient;

// Per-game match state. Shared by every game so the host UI can show a
// consistent lobby/in-game/end flow regardless of which game is active.
enum class GameState : uint8_t {
    Lobby   = 0,
    Running = 1,
    Paused  = 2,
    Ended   = 3,
};

// A pluggable game hosted by the Core2. Exactly one game is active at a time
// (see Host). The Net layer forwards WebSocket events to the active game, and
// the UI layer asks the active game to draw its own host screen.
//
// Each game owns its own wire protocol: the Net layer hands it raw text and
// the game parses it however it likes.
class IGame {
public:
    virtual ~IGame() {}

    // --- Metadata (used by the launcher and asset routing) ---
    virtual const char *name() const = 0;   // human label, e.g. "Space Dogfight"
    virtual const char *slug() const = 0;    // url/asset folder, e.g. "dogfight"

    // --- Lifecycle ---
    virtual void begin() = 0;                // called when the game is launched
    virtual void end()   = 0;                // called when returning to launcher
    virtual void update(uint32_t nowMs) = 0; // ticked only while this game is active

    // --- Net (forwarded by the Net layer) ---
    virtual void onClientConnect(AsyncWebSocketClient *client) { (void)client; }
    virtual void onClientText(AsyncWebSocketClient *client, const String &msg) = 0;
    virtual void onClientDisconnect(AsyncWebSocketClient *client) = 0;

    // --- Host control (from UI buttons) + display ---
    virtual void startRound() = 0;
    virtual void resetRound() = 0;
    virtual void togglePause() = 0;
    virtual GameState state() const = 0;
    virtual void drawHostScreen() = 0;       // draws this game's host display

    // Draw a small logo/icon for the launcher carousel, centred at (cx,cy)
    // within radius r, using M5.Lcd primitives. Default: nothing.
    virtual void drawLogo(int cx, int cy, int r) { (void)cx; (void)cy; (void)r; }
};
