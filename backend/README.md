# ESP32 Power Monitor Backend

This backend lets an `ESP32` keep its existing LAN API/UI while also maintaining a cloud connection for internet users.

## What it does

- Accepts a device WebSocket from the ESP32 at `/ws/device`
- Stores the latest status pushed by the ESP32 over the socket
- Exposes REST APIs for user apps
- Sends remote power commands to the ESP32 over the socket

## Environment

Copy `.env.example` to `.env` and set at least:

```env
PORT=8080
CORS_ORIGIN=*
```

## Run

```bash
cd backend
npm install
npm start
```

## REST API

`GET /health`

`GET /api/devices`

`GET /api/devices/:deviceId/status`

`POST /api/devices/:deviceId/commands/:command`

Supported commands:

- `ups_on_toggle`
- `ups_off`
- `server_on`
- `server_off`

Example:

```bash
curl -X POST https://monitor.codebaselabs.online/api/devices/esp32-power-monitor-1/commands/server_on
```

## ESP32 configuration

In `include/config.h` set:

```cpp
#define BACKEND_WS_URL     "wss://monitor.codebaselabs.online/ws/device"
#define DEVICE_ID          "esp32-power-monitor-1"
#define BACKEND_RETRY_INTERVAL_MS 5000
```

The ESP32 will keep serving the local LAN API and dashboard, and will also connect outward to:

`wss://monitor.codebaselabs.online/ws/device?deviceId=...`
