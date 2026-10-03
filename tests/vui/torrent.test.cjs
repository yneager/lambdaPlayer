// Real peer-to-peer transfer; no public trackers, DHT or internet downloads.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const http = require('node:http');
const net = require('node:net');
const crypto = require('node:crypto');
const {spawn} = require('node:child_process');
const {once} = require('node:events');
const {test} = require('node:test');
function encode(value) {
  if (Buffer.isBuffer(value)) return Buffer.concat([Buffer.from(value.length + ':'), value]);
  if (typeof value === 'string') return encode(Buffer.from(value));
  if (typeof value === 'number') return Buffer.from('i' + value + 'e');
  return Buffer.concat([Buffer.from('d'), ...Object.keys(value).sort().flatMap(key => [encode(key), encode(value[key])]), Buffer.from('e')]);
}
async function freePort() {
  const server = net.createServer(); server.listen(0, '127.0.0.1'); await once(server, 'listening');
  const port = server.address().port; await new Promise(resolve => server.close(resolve)); return port;
}
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
test('torrent engine transfers verified data between local peers without a speed cap', {timeout: 35000}, async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lambda-torrent-test-'));
  const children = [];
  const bytes = crypto.randomBytes(16 * 1024 * 1024), pieceLength = 256 * 1024;
  const pieces = [];
  for (let i = 0; i < bytes.length; i += pieceLength) pieces.push(crypto.createHash('sha1').update(bytes.subarray(i, i + pieceLength)).digest());
  const listen = await freePort();
  const tracker = http.createServer((req, res) => res.end(encode({interval: 1, complete: 1, incomplete: 1,
    peers: Buffer.from([127, 0, 0, 1, listen >> 8, listen & 255])})));
  tracker.listen(0, '127.0.0.1'); await once(tracker, 'listening');
  const torrent = encode({announce: `http://127.0.0.1:${tracker.address().port}/announce`, info: {
    length: bytes.length, name: 'peer-fixture.bin', 'piece length': pieceLength, pieces: Buffer.concat(pieces), private: 1
  }}).toString('base64');
  async function boot(name, peerPort) {
    const folder = path.join(dir, name); fs.mkdirSync(folder);
    if (name === 'seed') fs.writeFileSync(path.join(folder, 'peer-fixture.bin'), bytes);
    const port = await freePort();
    const child = spawn(path.resolve('tools/extra/win32/x64/aria2c.exe'), ['--no-conf=true', '--enable-rpc=true',
      `--rpc-listen-port=${port}`, '--rpc-secret=fixture', `--dir=${folder}`, `--listen-port=${peerPort}`,
      '--enable-dht=false', '--enable-dht6=false', '--bt-enable-lpd=false', '--disable-ipv6=true',
      '--bt-max-peers=256', '--disk-cache=64M', '--max-download-limit=0', '--max-overall-download-limit=0',
      '--socket-recv-buffer-size=1M', '--bt-force-encryption=false', '--bt-require-crypto=false', '--bt-min-crypto-level=arc4',
      '--bt-request-peer-speed-limit=20M', '--file-allocation=none', '--check-integrity=true', '--seed-time=60', '--seed-ratio=0', '--console-log-level=error'
    ], {windowsHide: true, stdio: 'ignore'});
    children.push(child);
    const rpc = async (method, params = []) => {
      const response = await (await fetch(`http://127.0.0.1:${port}/jsonrpc`, {method: 'POST', body: JSON.stringify({
        jsonrpc: '2.0', id: 1, method: 'aria2.' + method, params: ['token:fixture', ...params]
      })})).json();
      if (response.error) throw Error(response.error.message); return response.result;
    };
    for (let i = 0; i < 60; i++) { try { await rpc('getVersion'); return {rpc, folder}; } catch { await delay(100); } }
    throw Error('Engine did not start');
  }
  try {
    const seed = await boot('seed', listen);
    const seedGid = await seed.rpc('addTorrent', [torrent]);
    await delay(1500);
    assert.equal(Number((await seed.rpc('tellStatus', [seedGid])).completedLength), bytes.length, 'Seeder must contain all pieces');
    const download = await boot('download', await freePort());
    const gid = await download.rpc('addTorrent', [torrent]);
    let status;
    for (let i = 0; i < 230; i++) {
      status = await download.rpc('tellStatus', [gid]);
      if (status.status === 'error') throw Error(status.errorMessage);
      if (Number(status.completedLength) === bytes.length) break;
      await delay(100);
    }
    assert.equal(Number(status.completedLength), bytes.length);
    assert.deepEqual(fs.readFileSync(path.join(download.folder, 'peer-fixture.bin')), bytes);
    const options = await download.rpc('getGlobalOption');
    assert.equal(options['bt-max-peers'], '256'); assert.equal(options['max-overall-download-limit'], '0');
    for (const engine of [seed, download]) await engine.rpc('shutdown');
  } finally {
    for (const child of children) if (child.exitCode === null) { child.kill(); await once(child, 'exit'); }
    await new Promise(resolve => tracker.close(resolve));
    assert.ok(dir.startsWith(path.join(os.tmpdir(), 'lambda-torrent-test-')));
    fs.rmSync(dir, {recursive: true, force: true});
  }
});
