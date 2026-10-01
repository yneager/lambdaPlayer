// LAMBDA Player window chrome shared by the Home and Player pages.
//
// * Builds the custom minimize / maximize / close buttons into every
//   element marked [data-window-controls].
// * Reports title-bar-like areas ([data-drag], plus [data-drag-mini] in the
//   mini player) and the interactive "holes" inside them to C++, which uses
//   them for native window dragging, Aero Snap and double-click maximize.
// * Mirrors the native window state as <html> classes: is-maximized,
//   is-fullscreen, is-mini.
(() => {
  const ICONS = {
    min: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M6.5 12h11"/></svg>',
    max: '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="6.5" y="6.5" width="11" height="11" rx="2.6"/></svg>',
    restore: '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="6" y="8.5" width="9.5" height="9.5" rx="2.3"/><path d="M9 6h7a2.5 2.5 0 0 1 2.5 2.5v7"/></svg>',
    close: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m7.2 7.2 9.6 9.6m0-9.6-9.6 9.6"/></svg>'
  };

  const root = document.documentElement;
  let bridge = null;
  let pending = false;
  let lastRegions = '';

  const HOLE_SELECTOR = [
    '[data-drag] button', '[data-drag] a', '[data-drag] [data-nodrag]', '[data-drag] input',
    '.window-controls', '.lambda-update-notice',
    'html.is-mini [data-drag-mini] button', 'html.is-mini [data-drag-mini] a',
    'html.is-mini [data-drag-mini] [data-nodrag]', 'html.is-mini [data-drag-mini] .timeline',
    'html.is-mini [data-drag-mini] .volume-track', '.lambda-settings.open', '.lambda-popover.open'
  ].join(',');

  function buildControls() {
    document.querySelectorAll('[data-window-controls]').forEach(host => {
      if (host.dataset.ready) return;
      host.dataset.ready = '1';
      host.classList.add('window-controls');
      host.setAttribute('data-nodrag', '');
      host.innerHTML =
        '<button class="wc-btn wc-min" type="button" aria-label="Minimize" title="Minimize">' + ICONS.min + '</button>' +
        '<button class="wc-btn wc-max" type="button" aria-label="Maximize" title="Maximize">' + ICONS.max + '</button>' +
        '<button class="wc-btn wc-close" type="button" aria-label="Close" title="Close">' + ICONS.close + '</button>';
      host.querySelector('.wc-min').addEventListener('click', () => bridge && bridge.minimize());
      host.querySelector('.wc-max').addEventListener('click', () => bridge && bridge.toggleMaximize());
      host.querySelector('.wc-close').addEventListener('click', () => bridge && bridge.close());
    });
  }

  function applyState() {
    if (!bridge) return;
    const max = !!bridge.maximized, fs = !!bridge.fullscreen, mini = !!bridge.mini;
    root.classList.toggle('is-maximized', max);
    root.classList.toggle('is-fullscreen', fs);
    root.classList.toggle('is-mini', mini);
    document.querySelectorAll('.wc-max').forEach(b => {
      b.innerHTML = max ? ICONS.restore : ICONS.max;
      b.title = max ? 'Restore' : 'Maximize';
      b.setAttribute('aria-label', b.title);
    });
    scheduleRegions();
  }

  function applyUpdate() {
    if (!bridge) return;
    const update = bridge.update || {};
    const updating = ['downloading', 'verifying', 'preparing', 'restarting'].includes(update.status);
    const message = update.status === 'downloading' ? 'Downloading update · ' + (update.progress || 0) + '%'
      : update.status === 'verifying' ? 'Verifying download…'
      : update.status === 'preparing' ? 'Preparing update. Keep LAMBDA open…'
      : update.status === 'restarting' ? 'Restarting LAMBDA…'
      : update.status === 'error' && update.available && update.errorStage !== 'checking' ? 'Update failed. Your current version is safe. Try again.'
      : 'Update and restart. Your library and playback positions are kept.';
    let notice = document.querySelector('.lambda-update-notice');
    if (update.showNotification && !notice) {
      notice = document.createElement('div'); notice.className = 'lambda-update-notice';
      notice.setAttribute('role', 'status'); notice.setAttribute('aria-live', 'polite');
      notice.innerHTML = '<strong></strong><span></span><progress max="100" hidden></progress><div><button type="button" data-update-download>Update</button><button type="button" data-update-cancel hidden>Cancel download</button><button type="button" data-update-notes>What’s new</button><button type="button" data-update-dismiss>Later</button></div>';
      notice.querySelector('[data-update-download]').onclick = () => bridge.downloadUpdate();
      notice.querySelector('[data-update-cancel]').onclick = () => bridge.cancelUpdate();
      notice.querySelector('[data-update-notes]').onclick = () => {
        const dialog = document.createElement('dialog'); dialog.className = 'lambda-update-details';
        const title = document.createElement('h3'); title.textContent = 'What’s new · ' + (bridge.update.tag || '');
        const content = document.createElement('pre'); content.textContent = bridge.update.notes || 'Release notes are not available for this update.';
        const close = document.createElement('button'); close.textContent = 'Close'; close.onclick = () => dialog.close();
        dialog.append(title, content, close); document.body.appendChild(dialog);
        dialog.onclose = () => dialog.remove(); dialog.showModal();
      };
      notice.querySelector('[data-update-dismiss]').onclick = () => bridge.dismissUpdate();
      document.body.appendChild(notice);
    }
    if (notice) {
      notice.hidden = !update.showNotification;
      notice.querySelector('strong').textContent = 'Update available · ' + (update.tag || '');
      notice.querySelector('span').textContent = message;
      notice.querySelector('[data-update-download]').disabled = !!update.busy;
      notice.querySelector('[data-update-dismiss]').disabled = !!update.busy;
      notice.querySelector('[data-update-cancel]').hidden = update.status !== 'downloading';
      const progress = notice.querySelector('progress'); progress.hidden = !updating; progress.value = update.progress || 0;
    }
    const check = document.querySelector('[data-pop="updates"]');
    if (check) { check.disabled = !!update.busy; check.textContent = update.status === 'checking' ? 'Checking…' : 'Check for updates'; }
    const download = document.querySelector('[data-pop="update-download"]');
    if (download) { download.hidden = !update.available; download.disabled = !!update.busy; download.textContent = 'Update and restart'; }
    const tests = document.querySelector('[data-test-updates]');
    if (tests) { tests.checked = !!update.includeTests; tests.disabled = !!update.busy; }
    const status = document.querySelector('[data-update-status]');
    if (status) status.textContent = updating ? message : update.available ? 'Available: ' + update.tag : update.status === 'checking' ? 'Checking updates…' : update.status === 'ready' ? 'You’re up to date.' : update.status === 'error' ? 'Could not check updates. Try again later.' : 'Installed: ' + (update.currentTag || '');
    scheduleRegions();
  }

  const visibleRect = el => {
    const r = el.getBoundingClientRect();
    if (r.width <= 0 || r.height <= 0) return null;
    const style = getComputedStyle(el);
    if (style.visibility === 'hidden' || style.display === 'none' || Number(style.opacity) === 0) return null;
    return [r.left, r.top, r.width, r.height];
  };

  function reportRegions() {
    pending = false;
    if (!bridge) return;
    const drags = [];
    const holes = [];
    const dragSelector = root.classList.contains('is-mini') ? '[data-drag], [data-drag-mini]' : '[data-drag]';
    document.querySelectorAll(dragSelector).forEach(el => { const r = visibleRect(el); if (r) drags.push(r); });
    document.querySelectorAll(HOLE_SELECTOR).forEach(el => { const r = visibleRect(el); if (r) holes.push(r); });
    const regions = JSON.stringify([drags, holes]);
    if (regions !== lastRegions) {
      lastRegions = regions;
      bridge.setDragRegions(drags, holes);
    }
  }

  function scheduleRegions() {
    if (pending) return;
    pending = true;
    requestAnimationFrame(() => requestAnimationFrame(reportRegions));
  }

  window.LambdaWindow = {
    attach(windowBridge) {
      bridge = windowBridge;
      buildControls();
      applyState();
      bridge.stateChanged.connect(applyState);
      bridge.updateChanged.connect(applyUpdate);
      applyUpdate();
      new ResizeObserver(scheduleRegions).observe(document.body);
      window.addEventListener('resize', scheduleRegions);
      window.addEventListener('scroll', scheduleRegions, {passive: true});
      new MutationObserver(scheduleRegions).observe(root, {attributes: true, subtree: true, attributeFilter: ['class', 'hidden']});
      document.addEventListener('transitionend', scheduleRegions, true);
      scheduleRegions();
    },
    refresh: scheduleRegions,
    refreshUpdates: applyUpdate,
    checkUpdates: () => bridge && bridge.checkUpdates(),
    downloadUpdate: () => bridge && bridge.downloadUpdate(),
    setTestUpdates: enabled => bridge && bridge.setTestUpdates(enabled),

    toast(message, kind) {
      let host = document.querySelector('.lambda-toasts');
      if (!host) {
        host = document.createElement('div');
        host.className = 'lambda-toasts';
        host.setAttribute('role', 'status');
        host.setAttribute('aria-live', 'polite');
        document.body.appendChild(host);
      }
      const toast = document.createElement('div');
      toast.className = 'lambda-toast' + (kind === 'warn' ? ' warn' : '');
      toast.textContent = message;
      host.appendChild(toast);
      while (host.children.length > 3) host.firstElementChild.remove();
      requestAnimationFrame(() => requestAnimationFrame(() => toast.classList.add('show')));
      const life = Math.min(9000, 3200 + message.length * 35);
      setTimeout(() => {
        toast.classList.remove('show');
        setTimeout(() => toast.remove(), 350);
      }, life);
    },

    // Page transitions driven by MainWindow (opacity only, so position:fixed
    // content never jumps).
    leave() { root.classList.remove('lp-entering'); root.classList.add('lp-leaving'); },
    enter() {
      root.classList.remove('lp-leaving');
      root.classList.add('lp-entering');
      requestAnimationFrame(() => requestAnimationFrame(() => root.classList.remove('lp-entering')));
      scheduleRegions();
    }
  };

  document.addEventListener('DOMContentLoaded', buildControls);
})();
