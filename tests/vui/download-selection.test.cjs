const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../../resources/vui/downloads.js'), 'utf8');

function fixture(client) {
  let dialog;
  class Node {
    constructor() { this.children = []; this.nodes = {}; this.disabled = false; }
    setAttribute() {}
    append(...nodes) { this.children.push(...nodes); }
    querySelector(key) { return this.nodes[key] ||= new Node(); }
    querySelectorAll() { return this.querySelector('.download-file-list').children.map(label => label.children[0]); }
    showModal() {}
    close() { this.onclose?.(); }
    remove() {}
  }
  const context = {
    client, confirming:false, directory:'D:/Downloads', URL, Set, Date, setTimeout,
    document: {createElement(tag) { const node = new Node(); if (tag === 'dialog') dialog = node; return node; }, body:{append() {}}},
    window:{lambdaStremio:{show() {}}}, show:async () => {}, message() {}, poll:async () => {},
    chooseFolder:async () => 'D:/Other', formatBytes:bytes => `${bytes} B`,
  };
  vm.createContext(context);
  for (const name of ['element', 'validUri', 'confirmDownload']) {
    const start = source.indexOf(`  ${name === 'confirmDownload' ? 'async ' : ''}function ${name}(`);
    const next = source.indexOf('\n  function ', start + 1);
    // confirmDownload is followed by render; other helpers have a plain function boundary.
    vm.runInContext(source.slice(start, next), context);
  }
  return {context, dialog:() => dialog};
}
function engine() {
  const calls=[];
  return {
    calls, isConnected:() => true,
    addTorrent:async (...args) => { calls.push(['addTorrent', ...args]); return 'payload'; },
    addUri:async (...args) => { calls.push(['addUri', ...args]); return 'metadata'; },
    tellStatus:async gid => gid === 'metadata' ? {followedBy:['payload']} : {status:'paused', bittorrent:{info:{name:'test'}}, files:[{index:'1', path:'pack/one.mkv', length:'100'}, {index:'2', path:'pack/two.srt', length:'20'}]},
    changeOption:async (...args) => calls.push(['changeOption', ...args]),
    unpause:async (...args) => calls.push(['unpause', ...args]),
    forceRemove:async (...args) => calls.push(['forceRemove', ...args]),
    removeDownloadResult:async (...args) => calls.push(['removeDownloadResult', ...args]),
  };
}
test('torrent selection starts paused, requires a file and applies selection before unpausing', async () => {
  const client=engine(), ui=fixture(client);
  await ui.context.confirmDownload({data:'base64', name:'test.torrent'});
  const dialog=ui.dialog();
  assert.equal(client.calls[0][3].pause, 'true');
  assert.equal(client.calls.some(call => call[0] === 'unpause'), false);
  dialog.querySelector('[data-none]').onclick();
  assert.equal(dialog.querySelector('[data-start]').disabled, true);
  const second = dialog.querySelectorAll()[1]; second.checked=true; second.onchange();
  assert.equal(dialog.querySelector('[data-selection]').textContent, '1 of 2 files · 20 B selected');
  await dialog.querySelector('[data-start]').onclick();
  assert.equal(client.calls.find(call => call[0] === 'changeOption')[2]['select-file'], '2');
  assert.equal(client.calls.at(-1)[0], 'unpause');
});
test('magnet selection follows metadata into a paused payload and cancel removes both', async () => {
  const client=engine(), ui=fixture(client);
  await ui.context.confirmDownload({uri:'magnet:?xt=urn:btih:123'});
  assert.equal(client.calls[0][2]['pause-metadata'], 'true');
  assert.equal(client.calls.some(call => call[0] === 'unpause'), false);
  ui.dialog().close();
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(client.calls.filter(call => call[0] === 'forceRemove').map(call => call[1]).sort(), ['metadata','payload']);
  assert.equal(ui.context.confirming, false);
});
test('cancel during add cleans up the task returned after the dialog closes', async () => {
  const client=engine(); let resolveAdd;
  client.addTorrent=() => new Promise(resolve => { resolveAdd=resolve; });
  const ui=fixture(client), pending=ui.context.confirmDownload({data:'base64'});
  await new Promise(resolve => setImmediate(resolve));
  ui.dialog().close(); resolveAdd('payload'); await pending;
  assert.ok(client.calls.some(call => call[0] === 'forceRemove' && call[1] === 'payload'));
  assert.equal(client.calls.some(call => call[0] === 'unpause'), false);
});
