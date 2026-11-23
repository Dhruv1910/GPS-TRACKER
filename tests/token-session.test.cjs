const { test } = require('node:test');
const assert = require('node:assert/strict');
const { ViewerSession } = require('../static/token-session.js');

function storage() {
  let token = '';
  return { get: async () => token, set: async value => { token = value; },
    remove: async () => { token = ''; } };
}

test('saved login survives recreation; Disconnect removes it', async () => {
  const store = storage();
  await new ViewerSession(store).connect('test-token');
  const reopened = new ViewerSession(store);
  assert.equal(await reopened.restore(), 'test-token');
  await reopened.disconnect();
  assert.equal(await new ViewerSession(store).restore(), '');
});

test('Disconnect wins over an in-flight slow save', async () => {
  const store = storage();
  let finish;
  const started = new Promise(resolve => { finish = resolve; });
  const originalSet = store.set;
  store.set = async token => { await started; await originalSet(token); };
  const session = new ViewerSession(store);
  const save = session.connect('test-token');
  const disconnect = session.disconnect();
  finish();
  await Promise.all([save, disconnect]);
  assert.equal(await new ViewerSession(store).restore(), '');
});

test('a delayed restore cannot resurrect a disconnected session', async () => {
  const store = storage();
  await store.set('old-token');
  const session = new ViewerSession(store);
  const restored = session.restore();
  const disconnect = session.disconnect();
  assert.equal(await restored, null);
  await disconnect;
  assert.equal(session.token, '');
});

test('storage errors do not block a later Disconnect', async () => {
  const store = storage();
  store.set = async () => { throw new Error('disk unavailable'); };
  const session = new ViewerSession(store);
  await assert.rejects(session.connect('test-token'));
  await session.disconnect();
  assert.equal(await session.restore(), '');
});
