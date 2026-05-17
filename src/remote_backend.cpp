#include "remote_backend.h"

#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <WiFi.h>

#include "config.h"

namespace {
WebSocketsClient backendWs;
BackendStatusBuilder statusBuilderCb = nullptr;
BackendCommandHandler commandHandlerCb = nullptr;

bool backendEnabled = false;
bool backendConfigured = false;
bool backendConnected = false;
bool backendConnecting = false;       // true while SSL handshake is in progress
unsigned long connectingStartTime = 0;

static constexpr unsigned long CONNECT_TIMEOUT_MS = 20000; // abort a stuck handshake after 20 s

String backendHost;
String backendPath;
uint16_t backendPort = 0;
bool backendSecure = false;
unsigned long lastConnectAttempt = 0;

String trimTrailingSlash(const String& value) {
    if (value.length() > 0 && value.endsWith("/")) {
        return value.substring(0, value.length() - 1);
    }
    return value;
}

bool parseBackendWsUrl(const String& wsUrl) {
    String url = trimTrailingSlash(wsUrl);
    if (!(url.startsWith("ws://") || url.startsWith("wss://"))) {
        Serial.println("[BACKEND] Invalid BACKEND_WS_URL: use ws:// or wss://");
        return false;
    }

    backendSecure = url.startsWith("wss://");
    const int schemeOffset = backendSecure ? 6 : 5;
    const String remainder = url.substring(schemeOffset);
    const int slashIndex = remainder.indexOf('/');
    const String hostPort = slashIndex >= 0 ? remainder.substring(0, slashIndex) : remainder;
    backendPath = slashIndex >= 0 ? remainder.substring(slashIndex) : "/";

    const int colonIndex = hostPort.indexOf(':');
    if (colonIndex >= 0) {
        backendHost = hostPort.substring(0, colonIndex);
        backendPort = hostPort.substring(colonIndex + 1).toInt();
    } else {
        backendHost = hostPort;
        backendPort = backendSecure ? 443 : 80;
    }

    if (backendHost.length() == 0 || backendPort == 0) {
        Serial.println("[BACKEND] Invalid BACKEND_WS_URL: bad host or port");
        return false;
    }

    backendPath += (backendPath.indexOf('?') >= 0 ? "&" : "?");
    backendPath += "deviceId=" + String(DEVICE_ID);
    return true;
}

String payloadToString(const uint8_t* payload, size_t length) {
    String message;
    message.reserve(length);
    for (size_t i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    return message;
}

void sendJson(JsonDocument& doc) {
    if (!backendConnected) {
        return;
    }

    String out;
    serializeJson(doc, out);
    backendWs.sendTXT(out);
}

void sendHello() {
    JsonDocument doc;
    doc["type"] = "hello";
    doc["deviceId"] = DEVICE_ID;
    sendJson(doc);
}

void sendStatus(const char* reason) {
    if (!backendConnected || statusBuilderCb == nullptr) {
        return;
    }

    JsonDocument statusDoc;
    DeserializationError err = deserializeJson(statusDoc, statusBuilderCb());
    if (err) {
        Serial.printf("[BACKEND] Status JSON parse failed: %s\n", err.c_str());
        return;
    }

    JsonDocument doc;
    doc["type"] = "status";
    doc["deviceId"] = DEVICE_ID;
    doc["reason"] = reason;
    doc["payload"] = statusDoc.as<JsonVariantConst>();
    sendJson(doc);
}

void handleTextMessage(const String& message) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, message);
    if (err) {
        Serial.printf("[BACKEND] Ignored invalid JSON: %s\n", err.c_str());
        return;
    }

    const char* type = doc["type"] | "";
    if (strcmp(type, "command") == 0) {
        const String command = doc["command"] | "";
        const String requestId = doc["requestId"] | "";
        Serial.printf("[BACKEND] Command received: %s\n", command.c_str());
        if (commandHandlerCb != nullptr && command.length() > 0) {
            commandHandlerCb(command, requestId);
        }
        return;
    }

    if (strcmp(type, "ping") == 0) {
        JsonDocument pong;
        pong["type"] = "pong";
        pong["deviceId"] = DEVICE_ID;
        pong["timestamp"] = millis();
        sendJson(pong);
    }
}

void onBackendEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            backendConnected = false;
            backendConnecting = false;
            Serial.println("[BACKEND] Disconnected");
            break;

        case WStype_CONNECTED:
            backendConnected = true;
            backendConnecting = false;
            Serial.printf("[BACKEND] Connected to %s://%s:%u%s\n",
                          backendSecure ? "wss" : "ws",
                          backendHost.c_str(),
                          backendPort,
                          backendPath.c_str());
            sendHello();
            sendStatus("connect");
            break;

        case WStype_TEXT:
            handleTextMessage(payloadToString(payload, length));
            break;

        case WStype_ERROR:
            backendConnected = false;
            backendConnecting = false;
            Serial.println("[BACKEND] WebSocket error");
            break;

        default:
            break;
    }
}

void connectBackend() {
    if (!backendConfigured || WiFi.status() != WL_CONNECTED) {
        return;
    }

    const unsigned long now = millis();

    // If an SSL handshake is already in progress, leave it alone unless it
    // has been stuck for longer than CONNECT_TIMEOUT_MS.
    if (backendConnecting) {
        if ((now - connectingStartTime) < CONNECT_TIMEOUT_MS) {
            return;
        }
        Serial.println("[BACKEND] Handshake timed out — restarting");
        backendConnecting = false;
    }

    if ((now - lastConnectAttempt) < BACKEND_RETRY_INTERVAL_MS) {
        return;
    }

    lastConnectAttempt = now;
    connectingStartTime = now;
    backendConnecting = true;

    Serial.printf("[BACKEND] Connecting to %s://%s:%u%s\n",
                  backendSecure ? "wss" : "ws",
                  backendHost.c_str(),
                  backendPort,
                  backendPath.c_str());

    backendWs.disconnect();
    backendWs.setExtraHeaders(BACKEND_EXTRA_HEADERS);
    if (backendSecure) {
        backendWs.beginSSL(backendHost.c_str(), backendPort, backendPath.c_str(), "");
    } else {
        backendWs.begin(backendHost.c_str(), backendPort, backendPath.c_str());
    }
}
}

void backendBegin(BackendStatusBuilder statusBuilder, BackendCommandHandler commandHandler) {
    statusBuilderCb = statusBuilder;
    commandHandlerCb = commandHandler;

    backendEnabled = String(BACKEND_WS_URL).length() > 0;
    if (!backendEnabled) {
        Serial.println("[BACKEND] Disabled");
        return;
    }

    backendConfigured = parseBackendWsUrl(String(BACKEND_WS_URL));
    if (!backendConfigured) {
        return;
    }

    backendWs.onEvent(onBackendEvent);
    backendWs.enableHeartbeat(BACKEND_HEARTBEAT_MS, 3000, 2);
    connectBackend();
}

void backendLoop() {
    if (!backendEnabled || !backendConfigured) {
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        backendConnected = false;
        return;
    }

    backendWs.loop();
    if (!backendConnected) {
        connectBackend();
    }
}

void backendPublishStatus(bool force) {
    if (!backendEnabled || !backendConnected) {
        return;
    }

    sendStatus(force ? "manual" : "interval");
}

void backendResetRetryTimer() {
    lastConnectAttempt = 0;
}

bool backendIsEnabled() {
    return backendEnabled;
}

bool backendIsConnected() {
    return backendConnected;
}

void backendSendCommandResult(const String& requestId,
                              const String& command,
                              bool ok,
                              const String& detail) {
    if (!backendEnabled || !backendConnected) {
        return;
    }

    JsonDocument doc;
    doc["type"] = "command_result";
    doc["deviceId"] = DEVICE_ID;
    doc["requestId"] = requestId;
    doc["payload"]["command"] = command;
    doc["payload"]["ok"] = ok;
    doc["payload"]["detail"] = detail;
    sendJson(doc);
}
