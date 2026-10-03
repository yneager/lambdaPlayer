const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const {test} = require('node:test');
const source = fs.readFileSync(path.join(__dirname, '../../resources/vui/downloads.js'), 'utf8');
async function restore(options) {
  const changes = [];
  const task = {gid: 'restored', bittorrent: {}};
  class Client {
    async connect() {} isConnected() { return true; }
    async getOption() { return options; }
    async changeOption(gid, value) { changes.push({gid, value}); }
    async multicallSettled(calls) {
      return calls.map(call => ({status: 'fulfilled', value: call.method === 'aria2.tellActive' ? [task] : call.method === 'aria2.getGlobalStat' ? {downloadSpeed: 0, uploadSpeed: 0} : []}));
    }
  }
  const node = {classList: {toggle() {}}};
  const document = {body: {dataset: {view: 'home'}}, querySelector: () => node, querySelectorAll: () => [], addEventListener() {}};
  const window = {Motrix: {Aria2RpcClient: Client, JsonRpcProtocol: class {}, formatBytes: String, formatSpeed: String}};
  vm.runInNewContext(source, {window, document, URL, WebSocket: class {}, setTimeout() {}, clearTimeout() {}});
  const signal = {connect() {}};
  window.lambdaDownloads.attach({start(callback) { callback({port: 1, secret: 'fixture', directory: 'fixture', encrypted: true}); }, engineStopped: signal, downloadDropped: signal}, {});
  await window.lambdaDownloads.show();
  return JSON.parse(JSON.stringify(changes));
}
test('restored encrypted torrent retains peers when saved tuning already matches', async () => {
  assert.deepEqual(await restore({'bt-max-peers': '256', 'bt-request-peer-speed-limit': '20971520', 'max-download-limit': '0', 'bt-force-encryption': 'false', 'bt-require-crypto': 'false', 'bt-min-crypto-level': 'arc4'}), []);
});
test('older restored torrent migrates live peer options without an encryption restart', async () => {
  assert.deepEqual(await restore({'bt-max-peers': '128', 'bt-request-peer-speed-limit': '10485760', 'max-download-limit': '0', 'bt-force-encryption': 'false', 'bt-require-crypto': 'false', 'bt-min-crypto-level': 'arc4'}), [{gid: 'restored', value: {'bt-max-peers': '256', 'bt-request-peer-speed-limit': '20M'}}]);
});

test('encryption-only transfers migrate once to compatible peer handshakes', async () => {
  const changes = await restore({'bt-max-peers': '256', 'bt-request-peer-speed-limit': '20971520', 'max-download-limit': '0', 'bt-force-encryption': 'true', 'bt-require-crypto': 'false', 'bt-min-crypto-level': 'arc4'});
  assert.deepEqual(changes, [{gid: 'restored', value: {'bt-force-encryption': 'false'}}]);
});
