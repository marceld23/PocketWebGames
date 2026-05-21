// PocketWebGames - M5Stack Core2 host firmware.
// Wi-Fi SoftAP + HTTP + WebSocket match server hosting several browser games
// (one at a time, chosen from the on-device launcher). The display is the host
// console. See architecture.md for the architecture and protocol.

#include <Arduino.h>
#include <M5Core2.h>

#include "Config.h"
#include "Host.h"
#include "Net.h"
#include "UI.h"

void drawSplash() {
    M5.Lcd.fillScreen(TFT_BLACK);

    M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Lcd.setTextSize(3);
    const char *title = "PocketWebGames";
    M5.Lcd.setCursor((320 - (int)strlen(title) * 18) / 2, 90);
    M5.Lcd.print(title);

    M5.Lcd.drawFastHLine(34, 120, 252, TFT_DARKCYAN);

    M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Lcd.setTextSize(1);
    const char *sub = "local multiplayer game console";
    M5.Lcd.setCursor((320 - (int)strlen(sub) * 6) / 2, 130);
    M5.Lcd.print(sub);

    // A row of dots in the six game colours.
    const uint16_t pal[6] = {TFT_CYAN, TFT_RED, TFT_YELLOW,
                             TFT_GREEN, 0xC81F /*purple*/, TFT_ORANGE};
    for (int i = 0; i < 6; ++i)
        M5.Lcd.fillCircle(115 + i * 18, 162, 5, pal[i]);
}

void setup() {
    M5.begin(true, false, true, true);

    Serial.begin(115200);
    delay(50);
    Serial.println();
    Serial.println("=== PocketWebGames host starting ===");

    drawSplash();
    uint32_t splashStart = millis();

    // Heavy init runs while the splash is showing.
    host.begin();
    Net::begin();

    // Keep the splash visible for ~4 s total.
    while (millis() - splashStart < 4000) {
        Net::loop();          // service DNS/WS so early joiners aren't dropped
        delay(10);
    }

    UI::begin();
}

void loop() {
    uint32_t now = millis();
    Net::loop();
    host.update(now);
    UI::update(now);

    // Give the AsyncWebServer/AsyncTCP background tasks room to breathe.
    delay(2);
}
