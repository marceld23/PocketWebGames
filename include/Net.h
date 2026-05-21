#pragma once

#include <Arduino.h>

class AsyncWebSocketClient;

namespace Net {

void begin();
void loop();

// Send helpers used by the Game layer.
void sendText(AsyncWebSocketClient *client, const String &msg);
void broadcastText(const String &msg);

// Close every WebSocket client. Used by the Host when switching games so
// browsers reconnect and fetch the newly active game's assets.
void closeAllClients();

IPAddress apIp();
uint8_t   stationCount();

}  // namespace Net
