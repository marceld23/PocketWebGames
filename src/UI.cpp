#include "UI.h"

#include <M5Core2.h>

#include "Config.h"
#include "Host.h"
#include "IGame.h"
#include "Net.h"

namespace {

constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;

// Bottom-row "button" hint zones (mapped to M5.BtnA/B/C touch areas).
constexpr int BTN_Y = 200;
constexpr int BTN_H = 36;

// Header geometry. Battery icon sits at the far right; the "MENU" chip (which
// returns to the launcher) sits just left of it while in a game.
constexpr int HEADER_H = 24;
constexpr int BATT_X   = 290;          // battery icon left edge
constexpr int MENU_X0  = 224;          // MENU chip hit zone (in-game)
constexpr int MENU_X1  = 286;

// Launcher carousel geometry.
constexpr int CARD_X = 46, CARD_Y = 40, CARD_W = 228, CARD_H = 128;
constexpr int LOGO_CX = 160, LOGO_CY = 86, LOGO_R = 34;

HostMode lastMode_    = (HostMode)0xFF;
bool     forceRedraw_ = true;
uint8_t  selIdx_      = 0;             // selected game in the launcher
bool     powerConfirm_ = false;       // launcher shutdown confirmation showing

// Touch tracking for swipe vs tap.
bool wasPressed_ = false;
int  startX_ = 0, startY_ = 0, lastX_ = 0, lastY_ = 0;

// Battery refresh throttle.
uint32_t lastBatPoll_ = 0;
int      lastBatBucket_ = -1;

int batteryPercent() {
    float v = M5.Axp.GetBatVoltage();
    int pct = (int)((v - 3.20f) / (4.20f - 3.20f) * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

void drawBatteryIcon() {
    int pct = batteryPercent();
    int x = BATT_X, y = 6, w = 22, h = 12;
    uint16_t col = (pct > 50) ? TFT_GREEN : (pct > 20 ? TFT_YELLOW : TFT_RED);
    // body + terminal nub
    M5.Lcd.drawRect(x, y, w, h, TFT_WHITE);
    M5.Lcd.fillRect(x + w, y + 3, 2, h - 6, TFT_WHITE);
    // clear + fill
    M5.Lcd.fillRect(x + 1, y + 1, w - 2, h - 2, TFT_BLACK);
    int fw = (w - 4) * pct / 100;
    if (fw > 0) M5.Lcd.fillRect(x + 2, y + 2, fw, h - 4, col);
}

void drawCarousel() {
    M5.Lcd.fillScreen(TFT_BLACK);

    // Header
    M5.Lcd.fillRect(0, 0, SCREEN_W, HEADER_H, TFT_NAVY);
    // Power-off icon (top-left): a power symbol; tap to shut down (confirmed).
    M5.Lcd.drawCircle(15, 13, 7, TFT_RED);
    M5.Lcd.drawCircle(15, 13, 6, TFT_RED);
    M5.Lcd.fillRect(11, 3, 8, 6, TFT_NAVY);    // gap at top of the ring
    M5.Lcd.fillRect(14, 4, 2, 8, TFT_RED);     // vertical bar
    M5.Lcd.setTextColor(TFT_WHITE, TFT_NAVY);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(32, 4);
    M5.Lcd.print("SELECT A GAME");
    drawBatteryIcon();

    uint8_t n = host.gameCount();
    if (selIdx_ >= n) selIdx_ = 0;
    IGame *g = host.gameAt(selIdx_);

    // Card
    M5.Lcd.fillRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 10, 0x0A29);  // deep blue
    M5.Lcd.drawRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 10, TFT_CYAN);

    if (g) {
        g->drawLogo(LOGO_CX, LOGO_CY, LOGO_R);
        const char *nm = g->name();
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(TFT_WHITE, 0x0A29);
        int w = (int)strlen(nm) * 12;
        int tx = LOGO_CX - w / 2;
        if (tx < CARD_X + 6) tx = CARD_X + 6;
        M5.Lcd.setCursor(tx, 138);
        M5.Lcd.print(nm);
    }

    // Side arrows
    M5.Lcd.fillTriangle(34, LOGO_CY, 18, LOGO_CY - 14, 18, LOGO_CY + 14, TFT_CYAN);
    M5.Lcd.fillTriangle(286, LOGO_CY, 302, LOGO_CY - 14, 302, LOGO_CY + 14, TFT_CYAN);

    // Page dots
    int dotY = 178;
    int gap = 16;
    int x0 = SCREEN_W / 2 - (n - 1) * gap / 2;
    for (uint8_t i = 0; i < n; ++i) {
        if (i == selIdx_) M5.Lcd.fillCircle(x0 + i * gap, dotY, 4, TFT_CYAN);
        else              M5.Lcd.drawCircle(x0 + i * gap, dotY, 3, TFT_DARKGREY);
    }

    UI::drawButtons("< Prev", "PLAY", "Next >");
}

// Shutdown confirmation. CANCEL on the left, POWER OFF on the right.
void drawPowerConfirm() {
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setTextSize(3);
    const char *t = "POWER OFF?";
    M5.Lcd.setCursor(160 - (int)strlen(t) * 9, 44);
    M5.Lcd.print(t);

    M5.Lcd.fillRoundRect(30, 110, 120, 56, 8, TFT_DARKCYAN);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_DARKCYAN);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(30 + (120 - 6 * 12) / 2, 130);
    M5.Lcd.print("CANCEL");

    M5.Lcd.fillRoundRect(170, 110, 120, 56, 8, TFT_RED);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_RED);
    M5.Lcd.setCursor(170 + (120 - 3 * 12) / 2, 130);
    M5.Lcd.print("OFF");
}

}  // namespace

namespace UI {

void begin() {
    M5.Lcd.fillScreen(TFT_BLACK);
    selIdx_ = 0;
    powerConfirm_ = false;
    forceRedraw_ = true;
}

void invalidate() { forceRedraw_ = true; }

void drawHeader(const char *title) {
    M5.Lcd.fillRect(0, 0, SCREEN_W, HEADER_H, TFT_NAVY);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_NAVY);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(8, 4);
    M5.Lcd.print(title);

    // MENU chip (returns to the launcher) — tap handled in update().
    M5.Lcd.fillRoundRect(MENU_X0, 2, MENU_X1 - MENU_X0, HEADER_H - 4, 4, TFT_MAROON);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_MAROON);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(MENU_X0 + 8, 8);
    M5.Lcd.print("< MENU");

    drawBatteryIcon();
}

void drawButtons(const char *a, const char *b, const char *c) {
    M5.Lcd.fillRect(0, BTN_Y, SCREEN_W, BTN_H + 4, TFT_DARKGREY);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_DARKGREY);
    M5.Lcd.setTextSize(2);
    const int third = SCREEN_W / 3;
    auto label = [&](const char *t, int col) {
        int x = col * third + (third - (int)strlen(t) * 12) / 2;
        if (x < col * third + 4) x = col * third + 4;
        M5.Lcd.setCursor(x, BTN_Y + 10);
        M5.Lcd.print(t);
    };
    label(a, 0);
    label(b, 1);
    label(c, 2);
}

void drawJoinPanel() {
    char wifiQr[96];
    snprintf(wifiQr, sizeof(wifiQr), "WIFI:T:WPA;S:%s;P:%s;;",
             Config::AP_SSID, Config::AP_PASSWORD);
    M5.Lcd.qrcode(wifiQr, 190, 30, 120, 3);
    M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(208, 154);
    M5.Lcd.print("scan to join WLAN");

    char url[40];
    snprintf(url, sizeof(url), "http://%s/", Net::apIp().toString().c_str());

    M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(8, 32); M5.Lcd.print("SSID");
    M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(8, 44); M5.Lcd.print(Config::AP_SSID);

    M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(8, 70); M5.Lcd.print("PASSWORD");
    M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(8, 82); M5.Lcd.print(Config::AP_PASSWORD);

    M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(8, 108); M5.Lcd.print("URL");
    M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Lcd.setCursor(8, 120); M5.Lcd.print(url);
}

void update(uint32_t now) {
    M5.update();

    TouchPoint_t pt = M5.Touch.getPressPoint();
    bool pressed = (pt.x >= 0 && pt.y >= 0);
    bool rising  = pressed && !wasPressed_;
    bool falling = !pressed && wasPressed_;
    if (rising) { startX_ = pt.x; startY_ = pt.y; lastX_ = pt.x; lastY_ = pt.y; }
    if (pressed) { lastX_ = pt.x; lastY_ = pt.y; }
    wasPressed_ = pressed;

    if (host.mode() == HostMode::Launcher && powerConfirm_) {
        // Shutdown confirmation dialog.
        if (falling) {
            int x = startX_, y = startY_;
            if (x >= 170 && x <= 290 && y >= 110 && y <= 166) {
                M5.Axp.PowerOff();                  // confirmed -> shut down
            } else {
                powerConfirm_ = false; invalidate(); // anything else cancels
            }
        }
        if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) {
            powerConfirm_ = false; invalidate();
        }
    } else if (host.mode() == HostMode::Launcher) {
        uint8_t n = host.gameCount();
        if (falling && n > 0) {
            int dx = lastX_ - startX_, dy = lastY_ - startY_;
            if (abs(dx) < 22 && abs(dy) < 22 && startX_ <= 30 && startY_ <= HEADER_H) {
                powerConfirm_ = true; invalidate();              // power icon
            } else if (abs(dx) > 40 && abs(dx) > abs(dy)) {
                // swipe: left -> next, right -> prev
                selIdx_ = (dx < 0) ? (selIdx_ + 1) % n : (selIdx_ + n - 1) % n;
                invalidate();
            } else if (abs(dx) < 22 && abs(dy) < 22) {
                // tap
                int x = startX_, y = startY_;
                if (x < 44 && y > 60 && y < 140) {            // left arrow
                    selIdx_ = (selIdx_ + n - 1) % n; invalidate();
                } else if (x > 276 && y > 60 && y < 140) {     // right arrow
                    selIdx_ = (selIdx_ + 1) % n; invalidate();
                } else if (x >= CARD_X && x <= CARD_X + CARD_W &&
                           y >= CARD_Y && y <= CARD_Y + CARD_H) {
                    host.launch(selIdx_);                      // tap the card
                }
            }
        }
        // hardware fallback: A = prev, B = play, C = next
        if (n > 0) {
            if (M5.BtnA.wasPressed()) { selIdx_ = (selIdx_ + n - 1) % n; invalidate(); }
            if (M5.BtnC.wasPressed()) { selIdx_ = (selIdx_ + 1) % n; invalidate(); }
            if (M5.BtnB.wasPressed()) host.launch(selIdx_);
        }
    } else {  // IN_GAME
        IGame *g = host.active();
        // The three bottom buttons (left/middle/right) work by touch AND via
        // the hardware A/B/C buttons. Middle = back to the game selection.
        auto act = [&](int i) {
            if (!g) return;
            if (i == 0) {                          // Start / New round
                if (g->state() == GameState::Lobby || g->state() == GameState::Ended) {
                    g->startRound(); invalidate();
                }
            } else if (i == 1) {                   // Menu -> back to selection
                host.stop();
            } else {                               // Pause / Resume
                if (g->state() == GameState::Running || g->state() == GameState::Paused) {
                    g->togglePause(); invalidate();
                }
            }
        };

        if (rising && pt.x >= MENU_X0 && pt.x <= MENU_X1 && pt.y <= HEADER_H) {
            host.stop();                           // header MENU chip
        } else if (rising && pt.y >= BTN_Y && pt.y <= BTN_Y + BTN_H + 4) {
            int third = pt.x / (SCREEN_W / 3);     // on-screen button row
            if (third > 2) third = 2;
            if (third < 0) third = 0;
            act(third);
        }

        if (M5.BtnA.wasPressed()) act(0);
        if (M5.BtnB.wasPressed()) act(1);
        if (M5.BtnC.wasPressed()) act(2);
    }

    // Refresh the battery icon when its level bucket changes (~every 10 s).
    if (now - lastBatPoll_ > 10000) {
        lastBatPoll_ = now;
        int b = batteryPercent() / 5;
        if (b != lastBatBucket_) { lastBatBucket_ = b; forceRedraw_ = true; }
    }

    if (host.mode() != lastMode_) {
        lastMode_ = host.mode();
        powerConfirm_ = false;
        forceRedraw_ = true;
    }

    if (forceRedraw_) {
        if (host.mode() == HostMode::Launcher) {
            if (powerConfirm_) drawPowerConfirm(); else drawCarousel();
        } else if (host.active()) {
            host.active()->drawHostScreen();
        }
        forceRedraw_ = false;
    }
}

}  // namespace UI
