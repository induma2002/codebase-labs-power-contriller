import { app, BrowserWindow, dialog, ipcMain } from 'electron';
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

app.commandLine.appendSwitch('no-sandbox');
app.commandLine.appendSwitch('disable-setuid-sandbox');
app.commandLine.appendSwitch('disable-dev-shm-usage');

const BACKEND_PORT = 8080;
const BACKEND_HOST = '127.0.0.1';
const BACKEND_URL = `http://${BACKEND_HOST}:${BACKEND_PORT}`;
const PUBLIC_BACKEND_URL = 'https://monitor.codebaselabs.online';
const TUNNEL_ARGS = ['tunnel', 'run', 'esp32-power-monitor'];
const IS_PACKAGED = app.isPackaged;
const UI_DIR = IS_PACKAGED
  ? path.join(process.resourcesPath, 'UI')
  : path.resolve(__dirname, '..');
const DASHBOARD_FILE = path.join(UI_DIR, 'esp32_test_dashboard.html');
const BACKEND_ENTRY = IS_PACKAGED
  ? path.join(process.resourcesPath, 'backend', 'src', 'server.js')
  : path.resolve(__dirname, '../../backend/src/server.js');

let mainWindow = null;
let backendHandle = null;
let tunnelProcess = null;
const statusState = {
  backend: { running: false, url: BACKEND_URL, error: null },
  tunnel: { running: false, command: `cloudflared ${TUNNEL_ARGS.join(' ')}`, error: null },
  logs: []
};

function pushLog(line) {
  statusState.logs.push(line);
  if (statusState.logs.length > 200) {
    statusState.logs.shift();
  }
  if (mainWindow && !mainWindow.isDestroyed() && !mainWindow.webContents.isDestroyed()) {
    try {
      mainWindow.webContents.send('app-status', statusState);
    } catch {
      // ignore renderer disposal during startup/shutdown transitions
    }
  }
}

function publishStatus() {
  if (mainWindow && !mainWindow.isDestroyed() && !mainWindow.webContents.isDestroyed()) {
    try {
      mainWindow.webContents.send('app-status', statusState);
    } catch {
      // ignore renderer disposal during startup/shutdown transitions
    }
  }
}

async function waitForBackendReady(timeoutMs = 15000) {
  const started = Date.now();
  while ((Date.now() - started) < timeoutMs) {
    try {
      const response = await fetch(`${BACKEND_URL}/health`);
      if (response.ok) {
        return;
      }
    } catch {
      // keep retrying until timeout
    }
    await new Promise((resolve) => setTimeout(resolve, 300));
  }
  throw new Error('Timed out waiting for embedded backend');
}

async function startEmbeddedBackend() {
  pushLog(`[backend] starting on ${BACKEND_URL}`);
  const { startBackend } = await import(pathToFileURL(BACKEND_ENTRY).href);
  backendHandle = await startBackend({
    host: BACKEND_HOST,
    port: BACKEND_PORT,
    uiDir: UI_DIR,
    log: {
      log: (...args) => pushLog(args.join(' ')),
      error: (...args) => pushLog(args.join(' '))
    }
  });
  await waitForBackendReady();
  statusState.backend.running = true;
  statusState.backend.error = null;
  pushLog(`[backend] ready at ${BACKEND_URL}`);
  publishStatus();
}

function startTunnelProcess() {
  pushLog(`[tunnel] starting: cloudflared ${TUNNEL_ARGS.join(' ')}`);
  tunnelProcess = spawn('cloudflared', TUNNEL_ARGS, {
    stdio: ['ignore', 'pipe', 'pipe']
  });

  statusState.tunnel.running = true;
  statusState.tunnel.error = null;
  publishStatus();

  tunnelProcess.stdout.on('data', (chunk) => {
    pushLog(`[tunnel] ${chunk.toString().trim()}`);
  });

  tunnelProcess.stderr.on('data', (chunk) => {
    pushLog(`[tunnel] ${chunk.toString().trim()}`);
  });

  tunnelProcess.on('error', (error) => {
    statusState.tunnel.running = false;
    statusState.tunnel.error = error.message;
    pushLog(`[tunnel] failed: ${error.message}`);
    publishStatus();
    dialog.showErrorBox('Tunnel failed to start', error.message);
  });

  tunnelProcess.on('exit', (code, signal) => {
    statusState.tunnel.running = false;
    if (!statusState.tunnel.error) {
      statusState.tunnel.error = `Exited (${signal || code})`;
    }
    pushLog(`[tunnel] exited (${signal || code})`);
    publishStatus();
  });
}

async function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1440,
    height: 960,
    backgroundColor: '#0b1016',
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false
    }
  });

  try {
    await mainWindow.loadFile(path.join(__dirname, 'splash.html'));
  } catch (error) {
    pushLog(`[ui] splash load failed: ${error.message}`);
  }
}

async function openDashboard() {
  await mainWindow.loadFile(DASHBOARD_FILE, {
    query: {
      mode: 'cloud',
      backendUrl: PUBLIC_BACKEND_URL,
      deviceId: 'esp32-power-monitor-1',
      autoconnect: '1'
    }
  });
}

async function shutdownServices() {
  if (tunnelProcess && !tunnelProcess.killed) {
    tunnelProcess.kill();
    tunnelProcess = null;
  }

  if (backendHandle) {
    try {
      await backendHandle.close();
    } catch {
      // ignore shutdown errors
    }
    backendHandle = null;
  }
}

ipcMain.handle('app-status', () => statusState);

app.on('window-all-closed', async () => {
  await shutdownServices();
  if (process.platform !== 'darwin') {
    app.quit();
  }
});

app.whenReady().then(async () => {
  await createWindow();

  try {
    await startEmbeddedBackend();
    startTunnelProcess();
    await openDashboard();
  } catch (error) {
    statusState.backend.error = error.message;
    publishStatus();
    dialog.showErrorBox('Application startup failed', error.message);
  }
});

app.on('before-quit', async (event) => {
  event.preventDefault();
  await shutdownServices();
  app.exit(0);
});
