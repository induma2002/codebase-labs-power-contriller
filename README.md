# ESP32 Power Monitor

A complete power-monitoring and remote-control system for a home server UPS setup.  
An ESP32 reads live AC voltages and battery state, serves a local REST + WebSocket API, and bridges to a cloud backend so the server can be monitored and controlled from anywhere via a desktop app or browser.

---

## Table of Contents

- [System Architecture](#system-architecture)
- [Hardware](#hardware)
  - [Wiring](#wiring)
  - [AC Voltage Sensing](#ac-voltage-sensing)
  - [Battery Voltage Sensing](#battery-voltage-sensing)
  - [Button Control (Optocouplers)](#button-control-optocouplers)
- [Firmware](#firmware)
  - [Prerequisites](#prerequisites)
  - [Configuration](#configuration)
  - [Calibration](#calibration)
  - [Build & Flash](#build--flash)
  - [Local REST API](#local-rest-api)
  - [Local WebSocket](#local-websocket)
  - [JSON Payload Format](#json-payload-format)
  - [Timing & Watchdogs](#timing--watchdogs)
- [Cloud Backend](#cloud-backend)
  - [Running the Backend](#running-the-backend)
  - [Backend REST API](#backend-rest-api)
  - [Device WebSocket Protocol](#device-websocket-protocol)
  - [Environment Variables](#environment-variables)
- [Desktop App (Electron)](#desktop-app-electron)
  - [Running in Development](#running-in-development)
  - [Building a Package](#building-a-package)
- [Project Structure](#project-structure)
- [Libraries & Dependencies](#libraries--dependencies)

---

## System Architecture

```
┌─────────────────────────────────────────────────────┐
│                     Your Home                       │
│                                                     │
│  ZMPT101B ×2  ──┐                                   │
│  Resistor divider┤──► ESP32 ──► Local Wi-Fi LAN     │
│  PC817 ×2     ──┘      │                            │
│                        │  wss://                    │
└────────────────────────┼────────────────────────────┘
                         │
                         ▼
              ┌──────────────────────┐
              │  Cloud Backend       │
              │  (Node.js/Express)   │
              │  monitor.codebase    │
              │  labs.online         │
              └──────────┬───────────┘
                         │  HTTP + WS
                         ▼
              ┌──────────────────────┐
              │  Desktop App         │
              │  (Electron)          │
              │  Embeds backend +    │
              │  cloudflared tunnel  │
              └──────────────────────┘
```

**Data flow:**
1. ESP32 samples sensors every 500 ms and broadcasts over its local WebSocket every 1.5 s.
2. ESP32 maintains a persistent `wss://` connection to the cloud backend, pushing status updates every 1.5 s.
3. The desktop app (or any browser) connects to the cloud backend to read live data and send commands.
4. Commands travel: Desktop → Cloud Backend → ESP32 → GPIO → UPS / Server.

---

## Hardware

### Wiring

| Signal | GPIO | Notes |
|--------|------|-------|
| Grid AC (ZMPT101B #1 OUT) | 34 | ADC1 — 230 V grid input |
| Backup AC (ZMPT101B #2 OUT) | 35 | ADC1 — UPS 230 V output |
| Battery voltage | 32 | ADC1 — via 100 kΩ / 22 kΩ divider |
| UPS power button | 25 | Output → 330 Ω → PC817 anode |
| Server power button | 26 | Output → 330 Ω → PC817 anode |

> **ADC1 only.** GPIO 34, 35, and 32 are all on ADC1. ADC2 cannot be used while Wi-Fi is active on ESP32.

### AC Voltage Sensing

Two **ZMPT101B** AC voltage transformer modules measure the grid and UPS output voltages.

- The module output is centred at VCC/2 ≈ 1.65 V (DC offset).
- The firmware samples 500 points over exactly 20 ms (one full 50 Hz cycle), subtracts the running mean to remove the DC offset, computes RMS of the AC swing, then multiplies by a calibration constant to get real volts.

```
Vrms = RMS(samples − mean) × (3.3 V / 4095) × CAL
```

### Battery Voltage Sensing

A **100 kΩ / 22 kΩ resistor divider** scales the 12 V battery terminal voltage down into the ESP32's 0–3.3 V ADC range.

```
Divider ratio  = 22 / (100 + 22) = 0.18033
Battery volts  = (ADC volts / 0.18033) × BATTERY_CAL
```

64 samples are averaged per reading to reduce ADC noise.

**State-of-charge curve (12 V SLA / AGM):**

| Voltage | Charge |
|---------|--------|
| ≥ 12.70 V | 100 % |
| 12.50 – 12.70 V | 75 – 100 % |
| 12.40 – 12.50 V | 50 – 75 % |
| 12.20 – 12.40 V | 25 – 50 % |
| 11.90 – 12.20 V | 0 – 25 % |
| < 11.90 V | 0 % |

### Button Control (Optocouplers)

Two **PC817** optocouplers isolate the ESP32 GPIOs from the UPS and server power button circuits.

```
ESP32 GPIO ──► 330 Ω resistor ──► PC817 LED anode ──► GND
                                  PC817 collector ──► button terminal
                                  PC817 emitter  ──► button terminal
```

| Action | Duration | Command |
|--------|----------|---------|
| UPS power on (hold) | GPIO held HIGH | `ups_on_toggle` |
| UPS power off (force) | 5 000 ms pulse | `ups_off` |
| Server power on | 500 ms pulse | `server_on` |
| Server power off (force) | 5 000 ms pulse | `server_off` |

---

## Firmware

### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- ESP32 Dev Module board

### Configuration

Edit **`include/config.h`** before flashing:

```cpp
// Wi-Fi
#define WIFI_SSID   "your-network"
#define WIFI_PASS   "your-password"

// Cloud backend (leave empty to disable)
#define BACKEND_WS_URL  "wss://monitor.codebaselabs.online/ws/device"
#define DEVICE_ID       "esp32-power-monitor-1"
```

All timing, thresholds, pin assignments, and calibration constants are also in `config.h`.

### Calibration

1. Flash with `GRID_CAL 1.0` and `BACKUP_CAL 1.0`.
2. Open Serial Monitor at **115 200 baud**.
3. Measure the same socket with a real multimeter.
4. Calculate: `new_CAL = multimeter_reading / esp32_reading`
5. Set the new value in `config.h` and reflash.

Current calibrated values:

```cpp
#define GRID_CAL    727.5715f  // 214.0 V actual vs 225.5 V raw
#define BACKUP_CAL  722.0889f  // 230.0 V actual vs 244.2 V raw
#define BATTERY_CAL 1.0744f   // 12.265 V actual vs 16.245 V raw (divider)
```

### Build & Flash

```bash
# Build
pio run

# Build and upload
pio run --target upload

# Open serial monitor
pio device monitor --baud 115200
```

After boot the serial monitor prints the device's local IP and all available endpoints:

```
=== ESP32 Power Monitor ===
[WiFi] Connected!  IP: 192.168.1.x
──────────────────────────────────────────────
  WebSocket   ws://192.168.1.x/ws
  Status      GET  http://192.168.1.x/api/status
  UPS toggle  POST http://192.168.1.x/api/ups/on
  UPS off     POST http://192.168.1.x/api/ups/off
  Server on   POST http://192.168.1.x/api/server/on
  Server off  POST http://192.168.1.x/api/server/off
──────────────────────────────────────────────
```

### Local REST API

All endpoints return JSON. CORS headers are included on every response.

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/status` | Current sensor readings |
| `POST` | `/api/ups/on` | Toggle UPS hold on/off |
| `POST` | `/api/ups/off` | Force-off UPS (5 s long press) |
| `POST` | `/api/server/on` | Power on server (500 ms pulse) |
| `POST` | `/api/server/off` | Force-off server (5 s long press) |

### Local WebSocket

Connect to `ws://<ESP32-IP>/ws`.

- On connect the ESP32 immediately sends the current reading as JSON.
- Subsequent readings are pushed every `WS_BROADCAST_INTERVAL_MS` (1 500 ms).
- The WebSocket is read-only from the client perspective; commands go through the REST API or the cloud backend.

### JSON Payload Format

```json
{
  "grid":    { "voltage": 230.5, "ok": true  },
  "backup":  { "voltage": 229.1, "ok": true  },
  "battery": { "voltage": 12.68, "percent": 95 },
  "controls":{ "upsHeld": false },
  "uptime":  3742
}
```

| Field | Type | Description |
|-------|------|-------------|
| `grid.voltage` | float (1 dp) | Grid AC RMS voltage |
| `grid.ok` | bool | `true` if voltage ≥ 210 V |
| `backup.voltage` | float (1 dp) | UPS output AC RMS voltage |
| `backup.ok` | bool | `true` if voltage ≥ 210 V |
| `battery.voltage` | float (2 dp) | Battery terminal voltage |
| `battery.percent` | int 0–100 | Estimated state of charge |
| `controls.upsHeld` | bool | UPS button currently held HIGH |
| `uptime` | int | Seconds since last boot |

### Timing & Watchdogs

| Constant | Default | Description |
|----------|---------|-------------|
| `SENSOR_INTERVAL_MS` | 500 ms | Sensor sampling rate |
| `WS_BROADCAST_INTERVAL_MS` | 1 500 ms | Local WebSocket push interval |
| `WIFI_CHECK_INTERVAL_MS` | 10 000 ms | Wi-Fi watchdog check interval |
| `BACKEND_RETRY_INTERVAL_MS` | 5 000 ms | Cloud reconnect attempt interval |
| `BACKEND_HEARTBEAT_MS` | 15 000 ms | WSS ping/pong keepalive interval |
| `REBOOT_INTERVAL_MS` | 30 min | Scheduled reboot for stability |

**Wi-Fi watchdog:** checks every 10 s; if disconnected, calls `WiFi.begin()` and immediately resets the cloud backend retry timer so reconnection is attempted as soon as Wi-Fi comes back.

**Cloud reconnect logic:** a `backendConnecting` flag prevents cancelling an in-progress SSL handshake. New attempts are only started after the previous one resolves (connected, error, or 20 s timeout).

**Scheduled reboot:** after 30 minutes of uptime the device calls `esp_restart()`. This keeps the Wi-Fi stack and cloud connection fresh over long periods.

**Voltage smoothing (EMA filter):**

```cpp
#define AC_SMOOTHING_ALPHA   0.98f  // AC readings (slow, steady)
#define BATT_SMOOTHING_ALPHA 0.85f  // Battery (faster response)
```

---

## Cloud Backend

A Node.js / Express server that acts as a bridge between the ESP32 and the dashboard UI.

- ESP32 connects as a **device** over WebSocket at `/ws/device?deviceId=<id>`.
- Browsers / the desktop app connect over HTTP and REST.
- Commands from the UI are queued and forwarded to the device; results are awaited and returned synchronously to the HTTP caller (15 s timeout).

### Running the Backend

```bash
cd backend
npm install
npm start          # production
npm run dev        # auto-restart on file changes
```

Listens on `http://127.0.0.1:8080` by default.

### Backend REST API

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/health` | Server health check |
| `GET` | `/api/devices` | List all known devices and their status |
| `GET` | `/api/devices/:deviceId/status` | Status of a specific device |
| `POST` | `/api/devices/:deviceId/commands/:command` | Send a command and wait for result |

**Accepted commands:** `ups_on_toggle`, `ups_off`, `server_on`, `server_off`

**Command response:**
```json
{
  "ok": true,
  "requestId": "uuid",
  "result": {
    "command": "server_on",
    "ok": true,
    "detail": "Server power button pulsed",
    "at": "2026-05-17T10:00:00.000Z"
  }
}
```

### Device WebSocket Protocol

The ESP32 connects to `wss://<host>/ws/device?deviceId=<id>`.

**ESP32 → Backend:**

| Type | When | Payload |
|------|------|---------|
| `hello` | On connect | `{ type, deviceId }` |
| `status` | Every 1.5 s or after a command | `{ type, deviceId, reason, payload: <readings JSON> }` |
| `pong` | In reply to `ping` | `{ type, deviceId, timestamp }` |
| `command_result` | After executing a command | `{ type, deviceId, requestId, payload: { command, ok, detail } }` |

**Backend → ESP32:**

| Type | When | Payload |
|------|------|---------|
| `command` | On UI request | `{ type, requestId, command }` |
| `ping` | Keepalive | `{ type }` |

### Environment Variables

Create `backend/.env` to override defaults:

```env
PORT=8080
HOST=127.0.0.1
CORS_ORIGIN=*
COMMAND_TIMEOUT_MS=15000
DEVICE_ONLINE_TTL_MS=15000
```

---

## Desktop App (Electron)

The Electron app bundles the Node.js backend and launches it as an embedded process. It also starts a **cloudflared** tunnel so the dashboard is accessible remotely at `https://monitor.codebaselabs.online`.

**Startup sequence:**
1. Show splash screen
2. Start embedded backend on `http://127.0.0.1:8080`
3. Start `cloudflared tunnel run esp32-power-monitor`
4. Load the dashboard, connecting to the public backend URL

**Prerequisites:**
- [cloudflared](https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/downloads/) installed and authenticated
- The tunnel `esp32-power-monitor` configured in your Cloudflare dashboard

### Running in Development

```bash
cd UI/desktop
npm install
npm start
```

### Building a Package

```bash
cd UI/desktop

# .deb package (Debian / Ubuntu)
npm run build:deb

# AppImage (universal Linux)
npm run build:appimage

# Both
npm run build:linux
```

Output goes to `UI/desktop/dist/`.

---

## Project Structure

```
esp32_power_monitor/
├── include/
│   ├── config.h          # All user-configurable settings
│   ├── sensors.h         # Readings struct + sensor function declarations
│   └── remote_backend.h  # Cloud backend API
├── src/
│   ├── main.cpp          # Setup, loop, REST API, local WebSocket
│   ├── sensors.cpp       # ADC sampling, RMS calculation, EMA filter
│   └── remote_backend.cpp# WSS client, reconnect logic, command handling
├── backend/
│   ├── src/server.js     # Express + WebSocket bridge server
│   └── package.json
├── UI/
│   ├── esp32_test_dashboard.html  # Web dashboard (served by backend)
│   └── desktop/
│       ├── main.js       # Electron main process
│       ├── preload.js    # IPC bridge
│       ├── splash.html   # Loading screen
│       └── package.json
└── platformio.ini        # PlatformIO build config
```

---

## Libraries & Dependencies

### Firmware (PlatformIO)

| Library | Version | Purpose |
|---------|---------|---------|
| ESPAsyncWebServer | ^3.7.6 | Async HTTP + WebSocket server |
| AsyncTCP | ^3.3.6 | Async TCP layer for ESPAsyncWebServer |
| ArduinoJson | ^7.3.1 | JSON serialisation / deserialisation |
| WebSockets | ^2.6.1 | WSS client for cloud backend connection |

### Backend (Node.js)

| Package | Purpose |
|---------|---------|
| express | HTTP server and routing |
| ws | WebSocket server (device connections) |
| cors | Cross-origin request headers |
| dotenv | Environment variable loading |

### Desktop (Electron)

| Package | Purpose |
|---------|---------|
| electron | Desktop app framework |
| electron-builder | Package to `.deb` / AppImage |
