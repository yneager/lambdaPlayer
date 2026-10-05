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
      const tuning = {'bt-max-peers': '256', 'bt-request-peer-speed-limit': '20M', 'max-download-limit': '0',
        'bt-force-encryption': 'false', 'bt-require-crypto': 'false', 'bt-min-crypto-level': config.encrypted ? 'arc4' : 'plain'};
      await Promise.allSettled(restored.filter(result => result.status === 'fulfilled').flatMap(result => result.value)
        .filter(task => task.bittorrent).map(async task => {
          const options = await client.getOption(task.gid);
          // RPC returns sizes as bytes; equivalent tuning must not restart peers.
          const changed = Object.fromEntries(Object.entries(tuning).filter(([key, value]) =>
            options[key] !== (value.endsWith('M') ? String(Number(value.slice(0, -1)) * 1024 * 1024) : value)));
          // Prefer encrypted handshakes, but allow legacy peers to connect too.
          // Old encryption-only tasks migrate once; later launches retain peers.
          if (Object.keys(changed).length) await client.changeOption(task.gid, changed);
        }));
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
    dialog.setAttribute('aria-label', 'Choose download files and destination');
    dialog.innerHTML = '<h2>Start download</h2><p data-source></p><strong>Save to</strong><span class="download-folder" data-destination></span><section data-files hidden><div class="download-file-toolbar"><strong>Choose files</strong><button type="button" class="pill-btn" data-all>Select all</button><button type="button" class="pill-btn" data-none>Clear</button></div><div class="download-file-list"></div><p data-selection aria-live="polite"></p></section><div class="install-actions"><button type="button" class="pill-btn" data-choose>Change folder…</button><button type="button" class="pill-btn" data-cancel>Cancel</button><button type="button" class="pill-btn primary" data-start>Start download</button></div><p data-error role="alert"></p>';
    const start = dialog.querySelector('[data-start]');
    const errorLabel = dialog.querySelector('[data-error]');
    const torrent = !!source.data || source.uri?.startsWith('magnet:');
    let rootGid, gid, files = [], selected = new Set(), closed = false, started = false, busy = false;
    const downloadClient = client;
    const previewDirectory = directory.replace(/\\/g, '/').replace(/\/$/, '');
    const cleanup = async () => {
      // Include the paused payload created by a magnet's metadata request.
      const ids = new Set([gid, rootGid].filter(Boolean));
      if (rootGid) {
        try { (await downloadClient.tellStatus(rootGid)).followedBy?.forEach(id => ids.add(id)); } catch {}
      }
      for (const id of ids) {
        try { await downloadClient.forceRemove(id); } catch {}
        try { await downloadClient.removeDownloadResult(id); } catch {}
      }
    };
    const updateSelection = () => {
      const bytes = files.filter(file => selected.has(file.index)).reduce((sum, file) => sum + Number(file.length), 0);
      dialog.querySelector('[data-selection]').textContent = `${selected.size} of ${files.length} files · ${formatBytes(bytes)} selected`;
      start.disabled = busy || (torrent && !selected.size);
    };
    dialog.querySelector('[data-source]').textContent = source.name || source.uri;
    dialog.querySelector('[data-destination]').textContent = directory;
    dialog.querySelector('[data-choose]').onclick = async () => {
      dialog.querySelector('[data-destination]').textContent = await chooseFolder();
    };
    dialog.querySelector('[data-all]').onclick = () => {
      files.forEach(file => selected.add(file.index));
      dialog.querySelectorAll('.download-file-list input').forEach(input => { input.checked = true; }); updateSelection();
    };
    dialog.querySelector('[data-none]').onclick = () => {
      selected.clear(); dialog.querySelectorAll('.download-file-list input').forEach(input => { input.checked = false; }); updateSelection();
    };
    start.onclick = async () => {
      busy = true; start.disabled = true;
      try {
        if (torrent) {
          if (!gid || !selected.size) throw new Error('Choose at least one file.');
          await downloadClient.changeOption(gid, {dir: directory, 'select-file': [...selected].join(','), 'pause-metadata': 'false'});
          await downloadClient.unpause(gid);
        } else await downloadClient.addUri([source.uri], {dir: directory});
        started = true; message('Download added.'); dialog.close(); await poll();
      } catch (error) { errorLabel.textContent = error.message; }
      finally { busy = false; updateSelection(); }
    };
    dialog.oncancel = event => { if (busy) event.preventDefault(); };
    dialog.querySelector('[data-cancel]').onclick = () => { if (!busy) dialog.close(); };
    dialog.onclose = () => {
      closed = true; dialog.remove();
      if (!started) cleanup().finally(() => { confirming = false; poll(); });
      else confirming = false;
    };
    document.body.append(dialog); dialog.showModal();
    if (!torrent) return;
    start.disabled = true;
    errorLabel.textContent = source.data ? 'Reading torrent files…' : 'Finding peers to fetch the file list… You can cancel at any time.';
    try {
      rootGid = source.data
        ? await downloadClient.addTorrent(source.data, [], {dir: directory, pause: 'true'})
        : await downloadClient.addUri([source.uri], {dir: directory, 'pause-metadata': 'true'});
      if (closed) { await cleanup(); return; }
      gid = rootGid;
      const deadline = Date.now() + 120000;
      while (!closed) {
        const task = await downloadClient.tellStatus(gid);
        if (task.followedBy?.length) { gid = task.followedBy[0]; continue; }
        if (task.status === 'error') throw new Error(task.errorMessage || 'Unable to fetch torrent metadata.');
        if (task.bittorrent?.info && task.files?.length) { files = task.files; break; }
        if (Date.now() > deadline) throw new Error('No file list received yet. Cancel and try again when more peers are available.');
        await new Promise(resolve => setTimeout(resolve, 500));
      }
      if (closed) { await cleanup(); return; }
      const list = dialog.querySelector('.download-file-list');
      files.forEach(file => {
        selected.add(file.index);
        const label = element('label', 'download-file');
        const checkbox = document.createElement('input'); checkbox.type = 'checkbox'; checkbox.checked = true;
        checkbox.onchange = () => { if (checkbox.checked) selected.add(file.index); else selected.delete(file.index); updateSelection(); };
        // Paths come from the engine; use textContent to keep torrent names inert.
        const path = file.path.replace(/\\/g, '/');
        const relativePath = path.startsWith(previewDirectory + '/') ? path.slice(previewDirectory.length + 1) : path;
        label.append(checkbox, element('span', '', relativePath), element('small', '', formatBytes(file.length)));
        label.title = file.path; list.append(label);
      });
      dialog.querySelector('[data-files]').hidden = false;
      errorLabel.textContent = 'Only selected files will download. Shared torrent pieces may contain a small amount of adjacent file data.';
      updateSelection();
    } catch (error) { if (!closed) errorLabel.textContent = error.message; }
  }

  function render() {
    const list = q('[data-download-list]');
    const existing = new Map(Array.from(list.querySelectorAll('.download-card')).map(card => [card.dataset.gid, card]));
    list.querySelectorAll(':scope > p').forEach(node => node.remove());
    const visible = tasks.filter(task => filter === 'all' || task.status === filter);
    if (!visible.length) list.append(element('p', 'page-lede', tasks.length ? 'No transfers in this view.' : 'Your downloads will appear here.'));
    for (const task of visible) {
      const files = (task.files || []).filter(file => file.selected !== 'false');
      const name = task.bittorrent?.info?.name || files[0]?.path?.split(/[\\/]/).pop() || files[0]?.uris?.[0]?.uri || 'Fetching torrent metadata…';
      let card = existing.get(task.gid);
      existing.delete(task.gid);
      // Keep cards, focus and scroll position stable during the one-second poll.
      if (card && card.dataset.status === task.status) {
        const title = card.querySelector('.download-heading strong');
        if (title.textContent !== name) title.textContent = name;
        const progress = card.querySelector('progress');
        progress.setAttribute('aria-label', name + ' progress');
        progress.max = Number(task.totalLength) || 1;
        progress.value = Number(task.completedLength) || 0;
        const peers = task.bittorrent ? ` · ${task.connections || 0} peers · ${task.numSeeders || 0} seeders` : '';
        const detail = task.errorMessage || `${formatBytes(task.completedLength)} / ${formatBytes(task.totalLength)} · ${formatSpeed(task.downloadSpeed)}${peers}`;
        const label = card.querySelector('.download-detail');
        if (label.textContent !== detail) label.textContent = detail;
        continue;
      }
      const previous = card;
      card = element('article', 'glass download-card');
      card.dataset.gid = task.gid;
      card.dataset.status = task.status;
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
      if (previous) previous.replaceWith(card); else list.append(card);
    }
    existing.forEach(card => card.remove());
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
