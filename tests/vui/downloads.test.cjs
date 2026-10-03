// Exercise the copied Motrix client against the actual pinned engine and local HTTP fixture.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const http = require('node:http');
const net = require('node:net');
const {spawn} = require('node:child_process');
const {once} = require('node:events');
const {test} = require('node:test');
const root = path.resolve(__dirname, '../..');
const context = {window: {}, setTimeout, clearTimeout, performance};
vm.runInNewContext(fs.readFileSync(path.join(root, 'resources/vui/motrix-client.js'), 'utf8'), context);
const {Aria2RpcClient, JsonRpcProtocol} = context.window.Motrix;
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(fn) {
  for (let i = 0; i < 100; i++) { const value = await fn(); if (value) return value; await delay(100); }
  throw new Error('Timed out waiting for engine state');
}

test('Motrix engine: download, pause/resume, faults, cancellation and SQLite restart', {timeout: 40000}, async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'lambda-download-test-'));
  const bytes = Buffer.alloc(1024 * 1024, 0x5a);
  const server = http.createServer((req, res) => {
    if (req.url === '/missing.mp4') { res.writeHead(404); res.end(); return; }
    const start = Number((req.headers.range || '').match(/bytes=(\d+)-/)?.[1] || 0);
    const end = Math.min(bytes.length - 1, Number((req.headers.range || '').match(/-(\d+)$/)?.[1] || bytes.length - 1));
    res.writeHead(req.headers.range ? 206 : 200, {'Content-Length': end - start + 1, 'Accept-Ranges': 'bytes', ...(req.headers.range ? {'Content-Range': `bytes ${start}-${end}/${bytes.length}`} : {})});
    res.end(bytes.subarray(start, end + 1));
  });
  server.listen(0, '127.0.0.1'); await once(server, 'listening');
  const socket = net.createServer(); socket.listen(0, '127.0.0.1'); await once(socket, 'listening');
  const port = socket.address().port; await new Promise(resolve => socket.close(resolve));
  let child, client;
  async function boot() {
    child = spawn(path.join(root, 'tools/extra/win32/x64/aria2c.exe'), [
      '--no-conf=true', '--enable-rpc=true', '--rpc-listen-all=false', `--rpc-listen-port=${port}`, '--rpc-secret=test-token',
      '--enable-sqlite3-persistence=true', `--sqlite3-db-path=${path.join(directory, 'aria2.db')}`,
      '--sqlite3-history-limit=10000', `--dir=${directory}`, '--file-allocation=none', '--enable-dht=false', '--enable-dht6=false', '--disable-ipv6=true', '--console-log-level=error'
    ], {windowsHide: true, stdio: 'ignore'});
    const transport = {
      connected: false,
      async connect() {
        const response = await fetch(`http://127.0.0.1:${port}/jsonrpc`, {method: 'POST', body: JSON.stringify({jsonrpc: '2.0', id: 'ping', method: 'aria2.getVersion', params: ['token:test-token']})});
        assert.equal(response.ok, true); this.connected = true;
      },
      isConnected() { return this.connected; }, disconnect() { this.connected = false; },
      onMessage(fn) { this.message = fn; },
      send(data) {
        fetch(`http://127.0.0.1:${port}/jsonrpc`, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: data})
          .then(response => response.text()).then(text => this.message(text));
      }
    };
    client = new Aria2RpcClient(transport, new JsonRpcProtocol(transport), 'test-token');
    await client.connect(port, 30, 100);
  }
  async function shutdown() {
    const exited = once(child, 'exit'); await client.shutdown(); await exited;
  }
  try {
    await boot();
    const uri = `http://127.0.0.1:${server.address().port}/video.mp4`;
    const gid = await client.addUri([uri], {out: 'video.mp4', 'max-download-limit': '64K'});
    await until(async () => Number((await client.tellStatus(gid)).completedLength) > 0);
    assert.equal(await client.pause(gid), gid);
    await until(async () => (await client.tellStatus(gid)).status === 'paused');
    await shutdown(); await boot();
    assert.equal((await client.tellStatus(gid)).status, 'paused');
    await client.unpause(gid);
    await client.changeOption(gid, {'max-download-limit': '0'});
    await until(async () => (await client.tellStatus(gid)).status === 'complete');
    assert.deepEqual(fs.readFileSync(path.join(directory, 'video.mp4')), bytes);
    const batch = await client.multicallSettled([
      {method: 'aria2.tellStatus', params: [gid]}, {method: 'aria2.tellStatus', params: ['ffffffffffffffff']}
    ]);
    assert.equal(batch[0].status, 'fulfilled'); assert.equal(batch[1].status, 'rejected');
    await shutdown(); await boot();
    assert.ok((await client.searchDownloadResult({}, 0, 100)).some(task => task.gid === gid && task.status === 'complete'));
    const cancel = await client.addUri([uri], {out: 'cancel.mp4', pause: 'true'});
    await client.remove(cancel);
    await client.removeDownloadResult(gid);
    assert.ok(!(await client.searchDownloadResult({}, 0, 100)).some(task => task.gid === gid));
    const failed = await client.addUri([uri.replace('/video.mp4', '/missing.mp4')], {'max-tries': '1', 'max-file-not-found': '1'});
    await until(async () => (await client.tellStatus(failed)).status === 'error');
    const retried = await client.requeueDownloadResult(failed, {dir: directory});
    assert.ok(retried.gid && retried.gid !== failed);
    await until(async () => (await client.tellStatus(retried.gid)).status === 'error');
    await shutdown();
  } finally {
    if (child && child.exitCode === null) { child.kill(); await once(child, 'exit'); }
    await new Promise(resolve => server.close(resolve));
    // Remove only this test's newly-created, resolved temporary directory.
    assert.ok(directory.startsWith(path.join(os.tmpdir(), 'lambda-download-test-')));
    fs.rmSync(directory, {recursive: true, force: true});
  }
});
