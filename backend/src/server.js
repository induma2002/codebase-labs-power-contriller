import http from 'node:http';
import crypto from 'node:crypto';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import cors from 'cors';
import dotenv from 'dotenv';
import express from 'express';
import { WebSocketServer } from 'ws';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const defaultUiDir = path.resolve(__dirname, '../../UI');
const envPath = path.resolve(__dirname, '../.env');

dotenv.config({ path: envPath });

function createBackendApp({
  uiDir = defaultUiDir,
  corsOrigin = process.env.CORS_ORIGIN || '*',
  commandTimeoutMs = Number(process.env.COMMAND_TIMEOUT_MS || 15000),
  deviceOnlineTtlMs = Number(process.env.DEVICE_ONLINE_TTL_MS || 15000),
  log = console
} = {}) {
  const app = express();
  const server = http.createServer(app);
  const deviceWss = new WebSocketServer({ noServer: true });

  const devices = new Map();
  const pendingCommands = new Map();

  app.use(cors({ origin: corsOrigin }));
  app.use(express.json());
  app.use(express.static(uiDir));

  function getOrCreateDevice(deviceId) {
    if (!devices.has(deviceId)) {
      devices.set(deviceId, {
        deviceId,
        ws: null,
        connectedAt: null,
        lastSeenAt: null,
        status: null,
        lastStatusReason: null,
        lastCommandResult: null,
        queuedCommands: []
      });
    }

    return devices.get(deviceId);
  }

  function isDeviceOnline(device) {
    if (device?.ws?.readyState === 1) {
      return true;
    }
    if (!device?.lastSeenAt) {
      return false;
    }
    return Date.now() - new Date(device.lastSeenAt).getTime() <= deviceOnlineTtlMs;
  }

  function sanitizeDevice(device) {
    return {
      deviceId: device.deviceId,
      connected: isDeviceOnline(device),
      connectedAt: device.connectedAt,
      lastSeenAt: device.lastSeenAt,
      status: device.status,
      lastStatusReason: device.lastStatusReason,
      lastCommandResult: device.lastCommandResult
    };
  }

  function sendDeviceCommandMessage(device, queued) {
    if (!device.ws || device.ws.readyState !== 1) {
      return false;
    }

    device.ws.send(JSON.stringify({
      type: 'command',
      requestId: queued.requestId,
      command: queued.command
    }));
    return true;
  }

  function flushQueuedCommands(device) {
    if (!device.ws || device.ws.readyState !== 1 || device.queuedCommands.length === 0) {
      return;
    }

    const queued = [...device.queuedCommands];
    device.queuedCommands = [];
    for (const command of queued) {
      if (!sendDeviceCommandMessage(device, command)) {
        device.queuedCommands.unshift(command);
        break;
      }
    }
  }

  function queueDeviceCommand(deviceId, command) {
    const device = getOrCreateDevice(deviceId);
    const requestId = crypto.randomUUID();

    const queued = {
      requestId,
      command,
      createdAt: new Date().toISOString()
    };

    const promise = new Promise((resolve, reject) => {
      const timeoutId = setTimeout(() => {
        pendingCommands.delete(requestId);
        reject(new Error('Command timed out'));
      }, commandTimeoutMs);

      pendingCommands.set(requestId, {
        deviceId,
        command,
        timeoutId,
        resolve,
        reject
      });
    });

    if (!sendDeviceCommandMessage(device, queued)) {
      device.queuedCommands.push(queued);
    }

    return { requestId, promise };
  }

  function resolveCommandResult(result) {
    const pending = pendingCommands.get(result.requestId);
    if (!pending) {
      return;
    }

    clearTimeout(pending.timeoutId);
    pendingCommands.delete(result.requestId);
    pending.resolve(result);
  }

  app.get('/health', (_req, res) => {
    res.json({
      ok: true,
      uptime: process.uptime(),
      devices: devices.size
    });
  });

  app.get('/api/devices', (_req, res) => {
    res.json({
      ok: true,
      devices: [...devices.values()].map(sanitizeDevice)
    });
  });

  app.get('/api/devices/:deviceId/status', (req, res) => {
    const device = devices.get(req.params.deviceId);
    if (!device) {
      res.status(404).json({ ok: false, error: 'Device not found' });
      return;
    }

    res.json({
      ok: true,
      device: sanitizeDevice(device)
    });
  });

  app.post('/api/devices/:deviceId/commands/:command', async (req, res) => {
    const { deviceId, command } = req.params;
    const acceptedCommands = new Set([
      'ups_on_toggle',
      'ups_off',
      'server_on',
      'server_off'
    ]);

    if (!acceptedCommands.has(command)) {
      res.status(400).json({ ok: false, error: 'Unsupported command' });
      return;
    }

    const queued = queueDeviceCommand(deviceId, command);
    try {
      const result = await queued.promise;
      res.json({
        ok: true,
        requestId: queued.requestId,
        result
      });
    } catch (error) {
      res.status(504).json({
        ok: false,
        requestId: queued.requestId,
        error: error.message
      });
    }
  });

  app.get('/', (_req, res) => {
    res.sendFile(path.join(uiDir, 'esp32_test_dashboard.html'));
  });

  deviceWss.on('connection', (ws, request, clientInfo) => {
    const device = getOrCreateDevice(clientInfo.deviceId);
    log.log(`[WS] device connected: ${device.deviceId} from ${request.socket.remoteAddress}`);

    if (device.ws && device.ws !== ws) {
      device.ws.close(4000, 'Replaced by newer connection');
    }

    device.ws = ws;
    const now = new Date().toISOString();
    if (!device.connectedAt) {
      device.connectedAt = now;
    }
    device.lastSeenAt = now;
    flushQueuedCommands(device);

    ws.on('message', (data) => {
      let message;
      try {
        message = JSON.parse(data.toString());
      } catch {
        return;
      }

      device.lastSeenAt = new Date().toISOString();

      if (message.type === 'hello') {
        log.log(`[WS] hello from ${device.deviceId}`);
        flushQueuedCommands(device);
        return;
      }

      if (message.type === 'status') {
        device.status = message.payload || null;
        device.lastStatusReason = message.reason || null;
        log.log(`[WS] status from ${device.deviceId} reason=${device.lastStatusReason || 'n/a'}`);
        return;
      }

      if (message.type === 'command_result') {
        const result = {
          requestId: message.requestId || null,
          deviceId: device.deviceId,
          command: message.payload?.command || null,
          ok: Boolean(message.payload?.ok),
          detail: message.payload?.detail || '',
          at: new Date().toISOString()
        };
        device.lastCommandResult = result;
        log.log(`[WS] command result from ${device.deviceId} command=${result.command} ok=${result.ok}`);
        resolveCommandResult(result);
        return;
      }
    });

    ws.on('close', (code, reasonBuffer) => {
      const reason = reasonBuffer?.toString?.() || '';
      log.log(`[WS] device disconnected: ${device.deviceId} code=${code} reason=${reason || 'n/a'}`);
      if (device.ws === ws) {
        device.ws = null;
      }
    });

    ws.on('error', (error) => {
      log.error(`[WS] device socket error: ${device.deviceId} ${error.message}`);
    });
  });

  server.on('upgrade', (request, socket, head) => {
    const requestUrl = new URL(request.url, 'http://localhost');
    if (requestUrl.pathname !== '/ws/device') {
      socket.destroy();
      return;
    }

    const deviceId = requestUrl.searchParams.get('deviceId');
    log.log(`[WS] upgrade request path=${requestUrl.pathname} deviceId=${deviceId || '<missing>'} ip=${request.socket.remoteAddress}`);
    if (!deviceId) {
      socket.write('HTTP/1.1 401 Unauthorized\r\n\r\n');
      socket.destroy();
      return;
    }

    deviceWss.handleUpgrade(request, socket, head, (ws) => {
      deviceWss.emit('connection', ws, request, { deviceId });
    });
  });

  return { app, server, devices };
}

export function startBackend(options = {}) {
  const port = Number(options.port || process.env.PORT || 8080);
  const host = options.host || process.env.HOST || '127.0.0.1';
  const instance = createBackendApp(options);

  return new Promise((resolve, reject) => {
    instance.server.once('error', reject);
    instance.server.listen(port, host, () => {
      instance.server.off('error', reject);
      console.log(`ESP32 backend listening on http://${host}:${port}`);
      resolve({
        ...instance,
        port,
        host,
        close: () => new Promise((closeResolve, closeReject) => {
          instance.server.close((err) => {
            if (err) {
              closeReject(err);
              return;
            }
            closeResolve();
          });
        })
      });
    });
  });
}

const directRun = process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1]);
if (directRun) {
  startBackend().catch((error) => {
    console.error(error);
    process.exit(1);
  });
}
