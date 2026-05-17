# Power Monitor Desktop

Electron desktop app for the ESP32 Power Monitor.

## What it does

- Starts the embedded Node backend on `http://127.0.0.1:8080`
- Runs `cloudflared tunnel run esp32-power-monitor` in the background
- Opens the existing dashboard UI in a desktop window

## Requirements

- `cloudflared` must already be installed
- the tunnel `esp32-power-monitor` must already exist on the PC

## Run

```bash
cd UI/desktop
npm install
npm start
```

## Package For Ubuntu

This Electron app can be packaged as:

- `.deb`
- `AppImage`

Build commands:

```bash
cd UI/desktop
npm install
npm run build:deb
```

Or build both Linux targets:

```bash
cd UI/desktop
npm install
npm run build:linux
```

Artifacts are written to:

```bash
UI/desktop/dist/
```

Packaging notes:

- The packaged app includes the embedded backend
- The dashboard HTML is bundled into the app resources
- `cloudflared` is still expected to be installed on the target machine
- the tunnel `esp32-power-monitor` must already exist on that machine
