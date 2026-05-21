#pragma once

#include <Arduino.h>

namespace UI {

void begin();
void update(uint32_t nowMs);
void invalidate();   // force full redraw

// --- Shared drawing helpers for IGame::drawHostScreen() implementations ---
// Games call these so the host display stays visually consistent and games
// don't each reimplement the header / button hints / join panel.

// Top navy header bar with a title and a tappable "MENU" chip at the right
// (the chip returns to the launcher; the tap is handled in UI::update).
void drawHeader(const char *title);

// Bottom hint row labelling the three hardware buttons (BtnA/B/C). Pass "" to
// leave a slot blank.
void drawButtons(const char *a, const char *b, const char *c);

// Connection panel: Wi-Fi SSID / password / URL on the left and a Wi-Fi-join
// QR code on the right. Identical for every game (same AP, same URL).
void drawJoinPanel();

}  // namespace UI
