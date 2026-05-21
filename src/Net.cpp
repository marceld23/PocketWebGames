#include "Net.h"

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <DNSServer.h>

#include "Config.h"
#include "Host.h"
#include "IGame.h"

namespace {

AsyncWebServer    server(Config::HTTP_PORT);
AsyncWebSocket    ws(Config::WS_PATH);
DNSServer         dnsServer;
IPAddress         apIp_(192, 168, 4, 1);
IPAddress         apNetmask_(255, 255, 255, 0);
bool              dnsActive_ = false;

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            Serial.printf("[ws] connect #%u from %s\n",
                          client->id(), client->remoteIP().toString().c_str());
            host.onClientConnect(client);
            break;
        case WS_EVT_DISCONNECT:
            Serial.printf("[ws] disconnect #%u\n", client->id());
            host.onClientDisconnect(client);
            break;
        case WS_EVT_DATA: {
            AwsFrameInfo *info = (AwsFrameInfo *)arg;
            if (info->final && info->index == 0 && info->len == len &&
                info->opcode == WS_TEXT) {
                String text;
                text.reserve(len + 1);
                for (size_t i = 0; i < len; ++i) text += (char)data[i];
                host.onClientText(client, text);
            }
            break;
        }
        case WS_EVT_PONG:
        case WS_EVT_ERROR:
        default:
            break;
    }
}

void mountFs() {
    if (!LittleFS.begin(false)) {
        Serial.println("[fs] LittleFS mount failed, formatting...");
        LittleFS.format();
        if (!LittleFS.begin(true)) {
            Serial.println("[fs] LittleFS still failed - serving fallback page only");
        }
    } else {
        Serial.println("[fs] LittleFS mounted");
    }
}

const char FALLBACK_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>PocketWebGames</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>body{font-family:sans-serif;background:#000;color:#cfe;text-align:center;padding:2em}</style>
</head><body>
<h1>PocketWebGames Host online</h1>
<p>No web assets installed on the device yet.</p>
<p>Upload <code>data/</code> to LittleFS:</p>
<pre>pio run -t uploadfs</pre>
</body></html>)HTML";

// Captive-portal trigger page. Served with HTTP 200 (not 302) because Samsung
// and other Android skins detect captive portals more reliably from a 200 +
// HTML body than from a redirect. Meta-refresh + JS + visible link all aim at
// the real game URL so any client that lands here ends up at the game.
const char PORTAL_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head>
<meta charset="utf-8">
<meta http-equiv="refresh" content="0;url=http://192.168.4.1/">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sign in to PocketWebGames</title>
<style>body{margin:0;font-family:-apple-system,sans-serif;background:#001018;color:#cfe;text-align:center;padding:2em}
h1{color:#6cf;letter-spacing:.15em;margin:.2em 0 1em}
a.go{display:inline-block;padding:1em 2em;margin-top:1em;background:#0af;color:#000;font-weight:700;text-decoration:none;border-radius:8px;font-size:1.2em;letter-spacing:.1em}</style>
</head><body>
<h1>POCKET WEB GAMES</h1>
<p>Tap to enter:</p>
<a class="go" href="http://192.168.4.1/">PLAY</a>
<script>location.replace("http://192.168.4.1/")</script>
</body></html>)HTML";

// Shown at "/" while the host is still in the launcher (no game active). The
// meta-refresh re-polls "/" so a phone that joined early auto-loads the game
// the moment the host launches one.
const char LAUNCHER_WAIT_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head>
<meta charset="utf-8">
<meta http-equiv="refresh" content="2">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PocketWebGames</title>
<style>body{margin:0;font-family:-apple-system,sans-serif;background:#001018;color:#cfe;text-align:center;padding:3em 2em}
h1{color:#6cf;letter-spacing:.12em}</style>
</head><body>
<h1>POCKET WEB GAMES</h1>
<p>The host is choosing a game...</p>
<p>This page loads it automatically once it starts.</p>
</body></html>)HTML";

void setupHttp() {
    // Root is dynamic: redirect to the active game's folder, or show the
    // "host is choosing" page while in the launcher. Registered before the
    // static handler so the exact-"/" match wins.
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (host.mode() == HostMode::InGame && host.active()) {
            String to = String("/") + host.active()->slug() + "/";
            r->redirect(to);
        } else {
            AsyncWebServerResponse *resp =
                r->beginResponse_P(200, "text/html", LAUNCHER_WAIT_PAGE);
            resp->addHeader("Cache-Control", "no-store");
            r->send(resp);
        }
    });

    // Static assets from LittleFS. The WebSocket handler is attached on /ws
    // below; everything else falls through to onNotFound, which implements
    // the captive portal behavior.
    // NOTE: cache disabled while we iterate fast on client code. Samsung
    // browsers cache aggressively and ship stale game.js otherwise. Re-enable
    // (e.g. "max-age=60") once the client is stable.
    server.serveStatic("/", LittleFS, "/")
          .setDefaultFile("index.html")
          .setCacheControl("no-store");

    // Captive portal: every unknown URL (probe or random) gets a 302 to our
    // root. Combined with the DNS hijack, this makes Android/iOS/Windows
    // pop the "Sign in to network" sheet automatically when joining the AP.
    //
    // Probe URLs we know about (any non-success response triggers the popup):
    //   /generate_204, /gen_204            -> Android
    //   /hotspot-detect.html                -> iOS / macOS
    //   /library/test/success.html          -> macOS
    //   /ncsi.txt, /connecttest.txt         -> Windows
    //   /redirect, /success.txt, /canonical.html -> Firefox / others
    server.onNotFound([](AsyncWebServerRequest *r) {
        const String hostHdr = r->host();
        const String myHost  = WiFi.softAPIP().toString();
        const String url     = r->url();

        // If a client somehow asks our IP for /index.html and LittleFS is
        // empty, fall back to the embedded page so the device is never dark.
        if (hostHdr == myHost && (url == "/" || url == "/index.html")) {
            r->send_P(200, "text/html", FALLBACK_PAGE);
            return;
        }

        // Everything else: serve the captive-portal page with 200 + HTML.
        // Samsung/Android trigger the captive-portal sheet much more reliably
        // when the probe gets a 200 + non-empty body than when it gets a 302.
        // Cache-Control: no-store forces every probe to be re-evaluated.
        AsyncWebServerResponse *resp =
            r->beginResponse_P(200, "text/html", PORTAL_PAGE);
        resp->addHeader("Cache-Control", "no-store");
        resp->addHeader("Connection", "close");
        r->send(resp);
    });

    server.addHandler(&ws);
    ws.onEvent(onWsEvent);
}

}  // namespace

namespace Net {

void begin() {
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIp_, apIp_, apNetmask_);
    bool ok = WiFi.softAP(Config::AP_SSID, Config::AP_PASSWORD,
                          Config::AP_CHANNEL, 0, Config::AP_MAX_CONN);
    Serial.printf("[wifi] softAP %s @ %s (ok=%d)\n",
                  Config::AP_SSID, WiFi.softAPIP().toString().c_str(), ok ? 1 : 0);

    // Capture-portal DNS: resolve every name to the AP IP.
    dnsActive_ = dnsServer.start(53, "*", apIp_);
    Serial.printf("[dns] captive portal: %s\n", dnsActive_ ? "on" : "off");

    mountFs();
    setupHttp();
    server.begin();
    Serial.println("[http] server up");
}

void loop() {
    if (dnsActive_) dnsServer.processNextRequest();
    ws.cleanupClients();
}

void sendText(AsyncWebSocketClient *client, const String &msg) {
    if (client && client->status() == WS_CONNECTED) {
        client->text(msg);
    }
}

void broadcastText(const String &msg) {
    // Flow-controlled broadcast: skip any client whose send queue is full,
    // so a momentarily congested phone can't stall the whole match or get
    // force-disconnected. State broadcasts are latest-wins, so dropping one
    // for a busy client is harmless.
    for (auto &c : ws.getClients()) {
        if (c.status() == WS_CONNECTED && c.canSend()) c.text(msg);
    }
}

void closeAllClients() {
    ws.closeAll();
}

IPAddress apIp()        { return WiFi.softAPIP(); }
uint8_t   stationCount(){ return WiFi.softAPgetStationNum(); }

}  // namespace Net
