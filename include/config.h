#pragma once

// ============================================================
//  config.h  —  Edit this file before flashing
// ============================================================

// ── Wi-Fi ────────────────────────────────────────────────────
#define WIFI_SSID   "slt fiber"
#define WIFI_PASS   "Induma@20020822"

// ── Optional cloud backend ───────────────────────────────────
//  Leave BACKEND_WS_URL empty to disable the cloud bridge.
//  Example: "wss://monitor.codebaselabs.online/ws/device"
#define BACKEND_WS_URL     "wss://monitor.codebaselabs.online/ws/device"
#define DEVICE_ID          "esp32-power-monitor-1"
#define BACKEND_EXTRA_HEADERS ""

// ── ADC input pins  (ADC1 only — ADC2 conflicts with Wi-Fi) ──
#define PIN_GRID_AC     34   // ZMPT101B #1 OUT  → grid 230 V AC
#define PIN_BACKUP_AC   35   // ZMPT101B #2 OUT  → UPS output 230 V AC
#define PIN_BATTERY     32   // 100 kΩ / 22 kΩ divider midpoint → 12 V battery

// ── Digital output pins  (GPIO → 330 Ω → PC817 anode) ───────
#define PIN_UPS_BTN     25   // optocoupler #1 → UPS power button
#define PIN_SERVER_BTN  26   // optocoupler #2 → server power button

// ── AC sensor calibration ─────────────────────────────────────
//  How to calibrate:
//    1. Flash with CAL = 1.00
//    2. Open Serial Monitor (115200 baud)
//    3. Measure the same socket with a real multimeter
//    4. new_CAL = multimeter_reading / esp32_reading
//    5. Set the new value here and reflash
#define GRID_CAL    727.5715f // calibrated from 214.0 V actual vs 225.5 V shown
#define BACKUP_CAL  722.0889f // calibrated from 230.0 V actual vs 244.2 V shown
#define BATTERY_CAL 1.0744f // calibrated from 12.265 V actual vs 16.245 V shown

// ── Battery divider ratio ─────────────────────────────────────
//  Using 100 kΩ (top) and 22 kΩ (bottom):
//  ratio = 22 / (100 + 22) = 0.18033
#define BATT_RATIO  (22.0f / (100.0f + 22.0f))

// ── Thresholds ────────────────────────────────────────────────
#define AC_OK_VOLTS     210.0f   // below this → supply is "low"
#define BATT_FULL_V     13.50f   // 100%
#define BATT_DEAD_V     11.00f   //   0%

// ── Timing ───────────────────────────────────────────────────
#define SENSOR_INTERVAL_MS      500    // how often to read sensors
#define WS_BROADCAST_INTERVAL_MS 1500  // how often to push over WebSocket
#define WIFI_CHECK_INTERVAL_MS  10000  // Wi-Fi watchdog interval
#define BACKEND_RETRY_INTERVAL_MS 5000 // cloud reconnect backoff
#define BACKEND_HEARTBEAT_MS    15000  // backend heartbeat interval

// ── Reading smoothing (0.0 = no smoothing, 1.0 = frozen) ────
//  Lower values respond faster. Higher values are steadier.
#define AC_SMOOTHING_ALPHA      0.98f
#define BATT_SMOOTHING_ALPHA    0.85f

// ── Button pulse durations ───────────────────────────────────
#define PRESS_SHORT_MS  500    // power on / graceful shutdown
#define PRESS_LONG_MS   5000   // force power off (5-second hold)
