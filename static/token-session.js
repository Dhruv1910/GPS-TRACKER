'use strict';

// Serialized storage operations prevent a delayed save from undoing Disconnect.
class ViewerSession {
  constructor(store) {
    this.store = store;
    this.version = 0;
    this.token = '';
    this.pending = Promise.resolve();
  }
  enqueue(action) {
    const result = this.pending.then(action);
    this.pending = result.catch(() => {});
    return result;
  }
  async restore() {
    const version = this.version;
    const token = await this.enqueue(() => this.store.get());
    if (version !== this.version) return null;
    this.token = token || '';
    return this.token;
  }
  connect(token) {
    this.version++;
    this.token = token;
    return this.enqueue(() => this.store.set(token));
  }
  disconnect() {
    this.version++;
    this.token = '';
    return this.enqueue(() => this.store.remove());
  }
}

async function createViewerSession() {
  if (window.Capacitor?.isNativePlatform()) {
    const { registerPlugin } = await import('@capacitor/core');
    const storage = registerPlugin('ViewerTokenStorage');
    return new ViewerSession({
      get: async () => (await storage.get()).token,
      set: token => storage.set({ token }),
      remove: () => storage.remove()
    });
  }
  const name = 'field-tracker-view-token';
  return new ViewerSession({
    get: () => window.localStorage.getItem(name),
    set: token => window.localStorage.setItem(name, token),
    remove: () => window.localStorage.removeItem(name)
  });
}

if (typeof window !== 'undefined') window.createViewerSession = createViewerSession;
if (typeof module !== 'undefined') module.exports = { ViewerSession };
