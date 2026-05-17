import { contextBridge, ipcRenderer } from 'electron';

contextBridge.exposeInMainWorld('desktopApp', {
  getStatus: () => ipcRenderer.invoke('app-status'),
  onStatus: (callback) => {
    const handler = (_event, payload) => callback(payload);
    ipcRenderer.on('app-status', handler);
    return () => ipcRenderer.removeListener('app-status', handler);
  }
});
