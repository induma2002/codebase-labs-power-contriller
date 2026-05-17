#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "config.h"
#include "remote_backend.h"
#include "sensors.h"

// ─────────────────────────────────────────────────────────────
//  Globals
// ─────────────────────────────────────────────────────────────
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

Readings live;   // updated by sensorsUpdate() in loop()
bool upsButtonHeld    = false;

unsigned long lastSensorRead  = 0;
unsigned long lastWsBroadcast = 0;
unsigned long lastWifiCheck   = 0;

#define REBOOT_INTERVAL_MS  (30UL * 60UL * 1000UL)  // 30 minutes

// ─────────────────────────────────────────────────────────────
//  JSON builder — single source of truth for both WS and REST
// ─────────────────────────────────────────────────────────────
String buildJson() {
    JsonDocument doc;

    doc["grid"]["voltage"]    = roundf(live.gridVoltage   * 10) / 10.0f;
    doc["grid"]["ok"]         = live.gridOk;

    doc["backup"]["voltage"]  = roundf(live.backupVoltage * 10) / 10.0f;
    doc["backup"]["ok"]       = live.backupOk;

    doc["battery"]["voltage"] = roundf(live.battVoltage   * 100) / 100.0f;
    doc["battery"]["percent"] = live.battPercent;

    doc["controls"]["upsHeld"] = upsButtonHeld;

    doc["uptime"] = live.uptime;

    String out;
    serializeJson(doc, out);
    return out;
}

// ─────────────────────────────────────────────────────────────
//  Button helpers
// ─────────────────────────────────────────────────────────────
void pulseButton(int pin, int ms, const char* label) {
    Serial.printf("[BTN] %s → GPIO %d HIGH for %d ms\n", label, pin, ms);
    digitalWrite(pin, HIGH);
    delay(ms);
    digitalWrite(pin, LOW);
    Serial.printf("[BTN] %s → GPIO %d LOW\n", label, pin);
}

void setButtonHeld(int pin, bool& heldState, bool hold, const char* label) {
    heldState = hold;
    digitalWrite(pin, hold ? HIGH : LOW);
    Serial.printf("[BTN] %s → GPIO %d %s\n", label, pin, hold ? "HOLD" : "RELEASE");
}

String buildControlResponse(const char* device, const char* action, bool held) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["device"] = device;
    doc["action"] = action;
    doc["held"] = held;

    String out;
    serializeJson(doc, out);
    return out;
}

bool runCommandByName(const String& command, String& detail) {
    if (command == "ups_on_toggle") {
        setButtonHeld(PIN_UPS_BTN, upsButtonHeld, !upsButtonHeld, "UPS ON");
        detail = upsButtonHeld ? "UPS hold enabled" : "UPS hold released";
        return true;
    }

    if (command == "ups_off") {
        if (upsButtonHeld) {
            setButtonHeld(PIN_UPS_BTN, upsButtonHeld, false, "UPS ON");
        }
        pulseButton(PIN_UPS_BTN, PRESS_LONG_MS, "UPS OFF");
        detail = "UPS forced off";
        return true;
    }

    if (command == "server_on") {
        pulseButton(PIN_SERVER_BTN, PRESS_SHORT_MS, "SERVER ON");
        detail = "Server power button pulsed";
        return true;
    }

    if (command == "server_off") {
        pulseButton(PIN_SERVER_BTN, PRESS_LONG_MS, "SERVER OFF");
        detail = "Server forced off";
        return true;
    }

    detail = "Unknown command";
    return false;
}

void handleBackendCommand(const String& command, const String& requestId) {
    String detail;
    const bool ok = runCommandByName(command, detail);
    backendSendCommandResult(requestId, command, ok, detail);
    backendPublishStatus(true);
}

// ─────────────────────────────────────────────────────────────
//  CORS helper  (lets a browser page call the API)
// ─────────────────────────────────────────────────────────────
void addCors(AsyncWebServerResponse* r) {
    r->addHeader("Access-Control-Allow-Origin",  "*");
    r->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    r->addHeader("Access-Control-Allow-Headers", "Content-Type");
}

// ─────────────────────────────────────────────────────────────
//  WebSocket event handler
// ─────────────────────────────────────────────────────────────
void onWsEvent(AsyncWebSocket* s, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            Serial.printf("[WS]  Client #%u connected  %s\n",
                          client->id(),
                          client->remoteIP().toString().c_str());
            client->text(buildJson());   // send current data immediately
            break;

        case WS_EVT_DISCONNECT:
            Serial.printf("[WS]  Client #%u disconnected\n", client->id());
            break;

        case WS_EVT_ERROR:
            Serial.printf("[WS]  Client #%u error\n", client->id());
            break;

        default:
            break;
    }
}

// ─────────────────────────────────────────────────────────────
//  Wi-Fi connect (blocking at boot)
// ─────────────────────────────────────────────────────────────
void wifiConnect() {
    Serial.printf("\n[WiFi] Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(500);
        Serial.print(".");
        tries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connected!  IP: %s\n",
                      WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n[WiFi] Could not connect — check SSID / password in config.h");
    }
}

// ─────────────────────────────────────────────────────────────
//  Print all endpoints to Serial after boot
// ─────────────────────────────────────────────────────────────
void printEndpoints() {
    String ip = WiFi.localIP().toString();
    Serial.println("──────────────────────────────────────────────");
    Serial.printf("  WebSocket   ws://%s/ws\n",              ip.c_str());
    Serial.printf("  Status      GET  http://%s/api/status\n",ip.c_str());
    Serial.printf("  UPS toggle  POST http://%s/api/ups/on\n", ip.c_str());
    Serial.printf("  UPS off     POST http://%s/api/ups/off\n",ip.c_str());
    Serial.printf("  Server on   POST http://%s/api/server/on\n", ip.c_str());
    Serial.printf("  Server off  POST http://%s/api/server/off\n",ip.c_str());
    Serial.println("──────────────────────────────────────────────");
}

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== ESP32 Power Monitor ===");

    // GPIO
    pinMode(PIN_UPS_BTN,    OUTPUT);
    pinMode(PIN_SERVER_BTN, OUTPUT);
    digitalWrite(PIN_UPS_BTN,    LOW);
    digitalWrite(PIN_SERVER_BTN, LOW);

    // ADC
    sensorsInit();

    // Wi-Fi
    wifiConnect();
    backendBegin(buildJson, handleBackendCommand);

    // WebSocket
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // ── REST: CORS preflight ────────────────────────────────
    server.on("/api/*", HTTP_OPTIONS, [](AsyncWebServerRequest* req) {
        auto* r = req->beginResponse(204);
        addCors(r);
        req->send(r);
    });

    // ── GET /api/status ─────────────────────────────────────
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        Serial.println("[API] GET  /api/status");
        auto* r = req->beginResponse(200, "application/json", buildJson());
        addCors(r);
        req->send(r);
    });

    // ── POST /api/ups/on  (toggle hold / release) ───────────
    server.on("/api/ups/on", HTTP_POST, [](AsyncWebServerRequest* req) {
        Serial.println("[API] POST /api/ups/on");
        String detail;
        runCommandByName("ups_on_toggle", detail);
        auto* r = req->beginResponse(200, "application/json",
            buildControlResponse("ups", upsButtonHeld ? "hold" : "release", upsButtonHeld));
        addCors(r);
        req->send(r);
        backendPublishStatus(true);
    });

    // ── POST /api/ups/off  (long press — 5 s) ───────────────
    server.on("/api/ups/off", HTTP_POST, [](AsyncWebServerRequest* req) {
        Serial.println("[API] POST /api/ups/off");
        String detail;
        runCommandByName("ups_off", detail);
        auto* r = req->beginResponse(200, "application/json",
            "{\"ok\":true,\"device\":\"ups\",\"action\":\"force_off\",\"ms\":" + String(PRESS_LONG_MS) + ",\"held\":false}");
        addCors(r);
        req->send(r);
        backendPublishStatus(true);
    });

    // ── POST /api/server/on  (short press — 500 ms) ─────────
    server.on("/api/server/on", HTTP_POST, [](AsyncWebServerRequest* req) {
        Serial.println("[API] POST /api/server/on");
        String detail;
        runCommandByName("server_on", detail);
        auto* r = req->beginResponse(200, "application/json",
            "{\"ok\":true,\"action\":\"server_on\",\"ms\":" + String(PRESS_SHORT_MS) + "}");
        addCors(r);
        req->send(r);
        backendPublishStatus(true);
    });

    // ── POST /api/server/off  (long press — 5 s) ────────────
    server.on("/api/server/off", HTTP_POST, [](AsyncWebServerRequest* req) {
        Serial.println("[API] POST /api/server/off");
        String detail;
        runCommandByName("server_off", detail);
        auto* r = req->beginResponse(200, "application/json",
            "{\"ok\":true,\"action\":\"server_off\",\"ms\":" + String(PRESS_LONG_MS) + "}");
        addCors(r);
        req->send(r);
        backendPublishStatus(true);
    });

    // ── 404 ─────────────────────────────────────────────────
    server.onNotFound([](AsyncWebServerRequest* req) {
        auto* r = req->beginResponse(404, "application/json",
            "{\"error\":\"not found\",\"path\":\"" + req->url() + "\"}");
        addCors(r);
        req->send(r);
    });

    server.begin();
    Serial.println("[HTTP] Server started");
    printEndpoints();
}

// ─────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────
void loop() {
    ws.cleanupClients();
    backendLoop();

    unsigned long now = millis();

    // ── Read sensors ─────────────────────────────────────────
    if (now - lastSensorRead >= SENSOR_INTERVAL_MS) {
        lastSensorRead = now;
        sensorsUpdate(live);

        Serial.printf("[READ] Grid: %.1f V (%s)  Backup: %.1f V (%s)  Batt: %.2f V (%d%%)\n",
            live.gridVoltage,
            live.gridOk   ? "OK"   : "DOWN",
            live.backupVoltage,
            live.backupOk ? "OK"   : "DOWN",
            live.battVoltage,
            live.battPercent);
    }

    // ── Broadcast over WebSocket ──────────────────────────────
    if (now - lastWsBroadcast >= WS_BROADCAST_INTERVAL_MS) {
        lastWsBroadcast = now;
        if (ws.count() > 0) {
            ws.textAll(buildJson());
            Serial.printf("[WS]  Broadcast → %u client(s)\n", ws.count());
        }
        backendPublishStatus(false);
    }

    // ── Scheduled reboot ─────────────────────────────────────
    if (now >= REBOOT_INTERVAL_MS) {
        Serial.println("[SYS] 30-minute scheduled reboot...");
        delay(200);
        esp_restart();
    }

    // ── Wi-Fi watchdog ────────────────────────────────────────
    if (now - lastWifiCheck >= WIFI_CHECK_INTERVAL_MS) {
        lastWifiCheck = now;
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Lost connection — reconnecting...");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASS);
            backendResetRetryTimer();  // reconnect backend immediately once Wi-Fi is back
        }
    }
}
