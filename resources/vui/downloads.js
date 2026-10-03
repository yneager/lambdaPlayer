// LAMBDA presentation and Qt host adapter for the copied Motrix RPC client.
(() => {
  'use strict';
  const q = s => document.querySelector(s);
  const {Aria2RpcClient, JsonRpcProtocol, formatBytes, formatSpeed} = window.Motrix;
  let bridge, home, client, transport, connecting, directory = '', tasks = [], filter = 'all', timer, polling = false;

  // Motrix's transport interface backed by Chromium's WebSocket instead of Node ws.
  class BrowserTransport {
    onMessage(handler) { this.message = handler; }
    onClose(handler) { this.closed = handler; }
    onError(handler) { this.error = handler; }
    isConnected() { return this.socket?.readyState === WebSocket.OPEN; }
    connect(url) {
      return new Promise((resolve, reject) => {
        const socket = this.socket = new WebSocket(url);
        const timeout = setTimeout(() => { socket.close(); reject(new Error('Download engine connection timed out.')); }, 3000);
        socket.onopen = () => { clearTimeout(timeout); resolve(); };
        socket.onmessage = event => this.message?.(event.data);
        socket.onerror = () => { clearTimeout(timeout); reject(new Error('Could not connect to the download engine.')); };
        socket.onclose = event => { clearTimeout(timeout); reject(new Error('Download engine disconnected.')); this.closed?.(event.code, event.reason); };
      });
    }
    disconnect() { this.socket?.close(); }
    send(data) {
      if (!this.isConnected()) throw new Error('Download engine disconnected.');
      this.socket.send(data);
    }
  }
  function message(text, warning = false) {
    q('[data-download-message]').textContent = text;
    q('[data-download-message]').classList.toggle('error', warning);
  }
  function controls(enabled) {
    document.querySelectorAll('[data-download-control]').forEach(button => { button.disabled = !enabled; });
  }
  function stopped() {
    clearTimeout(timer); controls(false);
    message('Download engine disconnected. Reconnect to continue.', true);
    q('[data-download-retry]').hidden = false;
  }
  async function show() {
    if (!bridge || connecting) return connecting;
    if (client?.isConnected()) { render(); return; }
    connecting = (async () => {
      controls(false); message('Starting download engine…');
      const config = await new Promise(resolve => bridge.start(resolve));
      if (config.error) throw new Error(config.error);
      directory = config.directory;
      q('[data-download-directory]').textContent = directory;
      transport = new BrowserTransport();
      transport.onClose(stopped);
      client = new Aria2RpcClient(transport, new JsonRpcProtocol(transport), config.secret);
      await client.connect(config.port);
      // Restored torrents can retain their old per-task peer/speed options.
      const restored = await client.multicallSettled([
        {method: 'aria2.tellActive', params: []}, {method: 'aria2.tellWaiting', params: [0, 1000]}
      ]);
      await Promise.allSettled(restored.filter(result => result.status === 'fulfilled').flatMap(result => result.value)
        .filter(task => task.bittorrent).map(task => client.changeOption(task.gid,
          {'bt-max-peers': '128', 'bt-request-peer-speed-limit': '10M', 'max-download-limit': '0',
            'bt-require-crypto': config.encrypted ? 'true' : 'false'})));
      controls(true); message('Ready to download.');
      q('[data-download-retry]').hidden = true;
      await poll();
    })().catch(error => {
      clearTimeout(timer); controls(false); message(error.message, true);
      q('[data-download-retry]').hidden = false;
    }).finally(() => { connecting = null; });
    return connecting;
  }
  async function poll() {
    if (polling || !client?.isConnected()) return;
    polling = true;
    try {
      const result = await client.multicallSettled([
        {method: 'aria2.tellActive', params: []},
        {method: 'aria2.tellWaiting', params: [0, 1000]},
        {method: 'aria2.tellStopped', params: [0, 1000]},
        {method: 'aria2.searchDownloadResult', params: [{}, 0, 1000]},
        {method: 'aria2.getGlobalStat', params: []}
      ]);
      const failed = result.find(entry => entry.status === 'rejected');
      if (failed) throw failed.reason;
      tasks = [...new Map(result.slice(0, 4).flatMap(entry => entry.value).map(task => [task.gid, task])).values()];
      const stats = result[4].value;
      q('[data-download-stats]').textContent = '↓ ' + formatSpeed(stats.downloadSpeed) + ' · ↑ ' + formatSpeed(stats.uploadSpeed);
      if (document.body.dataset.view === 'downloads') render();
    } catch (error) { message(error.message, true); }
    finally {
      polling = false;
      clearTimeout(timer);
      if (client?.isConnected()) timer = setTimeout(poll, 1000);
    }
  }
  async function action(fn, success) {
    try {
      await fn();
      if (success) message(success);
      await poll();
    } catch (error) { message(error.message, true); }
  }
  function element(tag, className, text) {
    const node = document.createElement(tag);
    node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }
  function validUri(uri) {
    try { return ['http:', 'https:', 'ftp:', 'magnet:'].includes(new URL(uri).protocol); }
    catch { return false; }
  }
  function chooseFolder() {
    return new Promise(resolve => bridge.chooseFolder(path => {
      directory = path;
      q('[data-download-directory]').textContent = path;
      resolve(path);
    }));
  }
  let confirming = false;
  async function confirmDownload(source) {
    if (confirming) return;
    if (source.error) { window.LambdaWindow?.toast(source.error, 'warn'); return; }
    if (!source.data && !validUri(source.uri)) return;
    confirming = true;
    window.lambdaStremio?.show('downloads');
    await show();
    if (!client?.isConnected()) { confirming = false; return; }
    const dialog = element('dialog', 'download-confirm');
    dialog.setAttribute('aria-label', 'Choose download destination');
    dialog.innerHTML = '<h2>Start download</h2><p data-source></p><strong>Save to</strong><span class="download-folder" data-destination></span><div class="install-actions"><button type="button" class="pill-btn" data-choose>Change folder…</button><button type="button" class="pill-btn" data-cancel>Cancel</button><button type="button" class="pill-btn primary" data-start>Start download</button></div><p data-error role="alert"></p>';
    dialog.querySelector('[data-source]').textContent = source.name || source.uri;
    dialog.querySelector('[data-destination]').textContent = directory;
    dialog.querySelector('[data-choose]').onclick = async () => {
      dialog.querySelector('[data-destination]').textContent = await chooseFolder();
    };
    dialog.querySelector('[data-cancel]').onclick = () => dialog.close();
    dialog.querySelector('[data-start]').onclick = async event => {
      event.target.disabled = true;
      try {
        if (source.data) await client.addTorrent(source.data, [], {dir: directory});
        else await client.addUri([source.uri], {dir: directory});
        message('Download added.'); dialog.close(); await poll();
      } catch (error) { dialog.querySelector('[data-error]').textContent = error.message; }
      finally { event.target.disabled = false; }
    };
    dialog.onclose = () => { confirming = false; dialog.remove(); };
    document.body.append(dialog); dialog.showModal();
  }
  function render() {
    const list = q('[data-download-list]');
    // Avoid replacing focused buttons while the user interacts with transfers.
    if (list.contains(document.activeElement)) return;
    list.replaceChildren();
    const visible = tasks.filter(task => filter === 'all' || task.status === filter);
    if (!visible.length) list.append(element('p', 'page-lede', tasks.length ? 'No transfers in this view.' : 'Your downloads will appear here.'));
    for (const task of visible) {
      const files = (task.files || []).filter(file => file.selected !== 'false');
      const name = task.bittorrent?.info?.name || files[0]?.path?.split(/[\\/]/).pop() || files[0]?.uris?.[0]?.uri || 'Fetching torrent metadata…';
      const card = element('article', 'glass download-card');
      const heading = element('div', 'download-heading');
      heading.append(element('strong', '', name), element('span', 'download-status', task.status));
      const progress = document.createElement('progress');
      progress.max = Number(task.totalLength) || 1;
      progress.value = Number(task.completedLength) || 0;
      progress.setAttribute('aria-label', name + ' progress');
      const peers = task.bittorrent ? ` · ${task.connections || 0} peers · ${task.numSeeders || 0} seeders` : '';
      const detail = task.errorMessage || `${formatBytes(task.completedLength)} / ${formatBytes(task.totalLength)} · ${formatSpeed(task.downloadSpeed)}${peers}`;
      const actions = element('div', 'install-actions');
      const button = (label, fn) => {
        const node = element('button', 'pill-btn', label); node.type = 'button';
        node.onclick = () => { node.disabled = true; action(fn).finally(() => { node.disabled = false; node.blur(); render(); }); };
        actions.append(node);
      };
      if (task.status === 'active' || task.status === 'waiting') button('Pause', () => client.pause(task.gid));
      if (task.status === 'paused') button('Resume', () => client.unpause(task.gid));
      if (task.status === 'error' || task.status === 'removed') button('Retry', () => client.requeueDownloadResult(task.gid, {dir: directory}));
      if (task.status === 'complete') {
        const videos = files.filter(file => /\.(mkv|mp4|avi|mov|webm|m4v|ts|m2ts|mpg|mpeg|wmv)$/i.test(file.path));
        videos.forEach(file => button(videos.length === 1 ? 'Play' : 'Play ' + file.path.split(/[\\/]/).pop(), async () => home.openRecent(file.path)));
        button('Show folder', async () => bridge.reveal(files[0]?.path || ''));
      }
      button(['active', 'waiting', 'paused'].includes(task.status) ? 'Cancel' : 'Remove', () =>
        ['active', 'waiting', 'paused'].includes(task.status) ? client.remove(task.gid) : client.removeDownloadResult(task.gid));
      card.append(heading, progress, element('small', 'download-detail', detail), actions);
      list.append(card);
    }
  }
  function attach(next, homeBridge) {
    bridge = next; home = homeBridge;
    if (!bridge) return;
    bridge.engineStopped.connect(stopped);
    bridge.downloadDropped.connect(confirmDownload);
    q('[data-download-form]').onsubmit = event => {
      event.preventDefault();
      const input = event.currentTarget.elements.url;
      const uri = input.value.trim();
      let parsed;
      try { parsed = new URL(uri); } catch { message('Enter a valid download link or magnet URL.', true); return; }
      if (!['http:', 'https:', 'ftp:', 'magnet:'].includes(parsed.protocol)) { message('Use an HTTP, HTTPS, FTP or magnet link.', true); return; }
      confirmDownload({uri});
    };
    q('[data-download-torrent]').onclick = () => bridge.chooseTorrent(result => {
      if (result.error) message(result.error, true);
      else if (result.data) confirmDownload(result);
    });
    q('[data-download-folder]').onclick = chooseFolder;
    q('[data-download-pause]').onclick = () => action(() => client.pauseAll(), 'Downloads paused.');
    q('[data-download-resume]').onclick = () => action(() => client.unpauseAll(), 'Downloads resumed.');
    q('[data-download-retry]').onclick = show;
    q('[data-download-filters]').onclick = event => {
      const button = event.target.closest('[data-filter]');
      if (!button) return;
      filter = button.dataset.filter;
      document.querySelectorAll('[data-filter]').forEach(node => node.classList.toggle('selected', node === button));
      render();
    };
    if (document.body.dataset.view === 'downloads') show();
  }
  // Browser text drops (including links dragged from inside the page).
  let dragDepth = 0;
  const isDownloadDrag = event => Array.from(event.dataTransfer?.types || []).some(type => ['Files', 'text/plain', 'text/uri-list'].includes(type));
  document.addEventListener('dragenter', event => { if (isDownloadDrag(event)) { event.preventDefault(); dragDepth++; document.body.classList.add('download-drag'); } });
  document.addEventListener('dragover', event => { if (isDownloadDrag(event)) event.preventDefault(); });
  document.addEventListener('dragleave', () => { if (--dragDepth <= 0) document.body.classList.remove('download-drag'); });
  document.addEventListener('drop', async event => {
    document.body.classList.remove('download-drag'); dragDepth = 0;
    const file = event.dataTransfer?.files?.[0];
    const uri = (event.dataTransfer?.getData('text/uri-list') || event.dataTransfer?.getData('text/plain') || '').split(/\r?\n/).find(line => line && !line.startsWith('#'))?.trim();
    if (file?.name.toLowerCase().endsWith('.torrent')) {
      event.preventDefault();
      if (file.size > 16 * 1024 * 1024) { message('Torrent files must be smaller than 16 MB.', true); return; }
      const reader = new FileReader();
      reader.onload = () => confirmDownload({name: file.name, data: reader.result.split(',')[1]});
      reader.readAsDataURL(file);
    } else if (uri && validUri(uri)) { event.preventDefault(); confirmDownload({uri}); }
  });
  window.lambdaDownloads = {attach, show, chooseFolder, confirmDownload};
})();
