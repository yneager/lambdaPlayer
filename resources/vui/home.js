// LAMBDA Player Home page behaviour.
(() => {
  let home = null;
  let recents = [];
  let session = {available: false, name: ''};
  const q = (s, root=document) => root.querySelector(s);
  const qa = (s, root=document) => Array.from(root.querySelectorAll(s));
  const call = (name, ...args) => { if (home && typeof home[name] === 'function') home[name](...args); };
  const THEMES = ['theme-cyan', 'theme-violet', 'theme-blue'];

  const formatLeft = seconds => {
    seconds = Math.max(0, Math.round(Number(seconds) || 0));
    const h = Math.floor(seconds / 3600);
    const m = Math.max(1, Math.round((seconds % 3600) / 60));
    return h > 0 ? h + 'h ' + String(m).padStart(2, '0') + 'm left' : m + ' min left';
  };

  function makeCard({title, meta, badge, progress, theme, onOpen, tooltip}) {
    const card = document.createElement('a');
    card.className = 'wide-card ' + theme;
    card.href = '#';
    card.title = tooltip || title;
    card.innerHTML =
      '<div class="card-art"><span class="sphere"></span><span class="slash"></span></div>' +
      '<div class="card-shade"></div>' +
      '<span class="card-badge"></span>' +
      '<div class="wide-info"><div><strong></strong><span></span></div><span class="mini-play"></span></div>' +
      '<div class="progress"><i></i></div>';
    q('.card-badge', card).textContent = badge;
    q('.wide-info strong', card).textContent = title;
    q('.wide-info div span', card).textContent = meta;
    q('.progress i', card).style.width = Math.round(Math.max(0, Math.min(1, progress || 0)) * 100) + '%';
    card.addEventListener('click', e => { e.preventDefault(); onOpen(); });
    return card;
  }

  function renderRecents() {
    const rail = q('.continue-rail');
    if (!rail) return;
    rail.innerHTML = '';
    if (!recents.length) {
      rail.appendChild(makeCard({
        title: 'Open your first video', meta: 'Recently played videos appear here',
        badge: 'START', progress: 0, theme: THEMES[0], onOpen: () => call('openVideo'),
        tooltip: 'Open a video from this PC'
      }));
      rail.appendChild(makeCard({
        title: 'Play a whole folder', meta: 'Next plays the following video automatically',
        badge: 'FOLDER', progress: 0, theme: THEMES[1], onOpen: () => call('openFolder'),
        tooltip: 'Choose a folder of videos'
      }));
    } else {
      recents.forEach((item, index) => {
        const current = session.available && item.path === session.path;
        const meta = current ? 'Resume current session'
          : item.watched ? 'Watched · play again'
          : item.remaining > 0 ? formatLeft(item.remaining) : 'Not started';
        rail.appendChild(makeCard({
          title: item.name, meta, badge: current ? 'NOW' : (item.ext || 'VIDEO'),
          progress: item.watched ? 1 : item.progress, theme: THEMES[index % THEMES.length],
          onOpen: () => current ? call('resume') : call('openRecent', item.path),
          tooltip: item.path
        }));
      });
    }
    rail.classList.remove('rail-enter');
    void rail.offsetWidth;
    rail.classList.add('rail-enter');
    if (window.LambdaWindow) window.LambdaWindow.refresh();
  }

  // The hero is shared with the add-on views (stremio.js), which feature an
  // add-on title when catalogs are installed and the local session otherwise.
  function renderHeroCta() {
    if (window.lambdaStremio) window.lambdaStremio.setLocalState(session, recents);
  }

  // ---- About popover --------------------------------------------------------
  function ensureAbout() {
    let pop = q('.lambda-popover');
    if (pop) return pop;
    pop = document.createElement('div');
    pop.className = 'lambda-popover glass';
    pop.setAttribute('role', 'dialog');
    pop.setAttribute('aria-label', 'About LAMBDA Player');
    pop.innerHTML =
      '<div class="pop-head"><span class="brand-mark"><i></i><i></i></span><div><strong>LAMBDA Player</strong><small class="pop-version"></small></div></div>' +
      '<p>Local video player built on libmpv, with optional real-time RIFE frame interpolation on any Vulkan GPU.</p>' +
      '<div class="pop-keys"><span><kbd>Space</kbd> Play / pause</span><span><kbd>←</kbd><kbd>→</kbd> Seek 5 s</span><span><kbd>F</kbd> Fullscreen</span><span><kbd>M</kbd> Mute</span><span><kbd>Ctrl</kbd><kbd>O</kbd> Open</span></div>' +
      '<div class="pop-actions"><button type="button" class="pop-btn primary" data-pop="open">Open video</button><button type="button" class="pop-btn" data-pop="licenses">Third-party licenses</button></div>';
    document.body.appendChild(pop);
    q('.pop-version', pop).textContent = 'Version ' + ((home && home.version) || '');
    q('[data-pop="open"]', pop).addEventListener('click', () => { closeAbout(); call('openVideo'); });
    q('[data-pop="licenses"]', pop).addEventListener('click', () => { closeAbout(); call('openLicenses'); });
    return pop;
  }
  function openAbout() { ensureAbout().classList.add('open'); q('.profile').classList.add('open'); if (window.LambdaWindow) window.LambdaWindow.refresh(); }
  function closeAbout() {
    const pop = q('.lambda-popover');
    if (pop) pop.classList.remove('open');
    const profile = q('.profile');
    if (profile) profile.classList.remove('open');
    if (window.LambdaWindow) window.LambdaWindow.refresh();
  }
  const aboutOpen = () => !!q('.lambda-popover.open');

  // ---- Bindings -------------------------------------------------------------
  function bind() {
    const hero = q('.hero');
    if (hero) new IntersectionObserver(entries => {
      hero.classList.toggle('is-offscreen', !entries[0].isIntersecting);
    }).observe(hero);
    qa('[data-action]').forEach(el => el.addEventListener('click', e => {
      e.preventDefault();
      const action = el.dataset.action;
      if (action === 'open') call('openVideo');
      else if (action === 'folder') call('openFolder');
      else if (action === 'resume') call('resume');
      else if (action === 'addons-view' && window.lambdaStremio) window.lambdaStremio.show('addons');
      else if (action === 'about') aboutOpen() ? closeAbout() : openAbout();
    }));

    // Nav shell gets a stronger glass once the page scrolls.
    const nav = q('.nav-shell');
    let navScrolled;
    const updateNav = () => {
      const scrolled = window.scrollY > 24;
      if (navScrolled !== scrolled) { nav.classList.toggle('scrolled', scrolled); navScrolled = scrolled; }
    };
    window.addEventListener('scroll', updateNav, {passive: true});
    updateNav();

    // Rail arrow buttons scroll their rail by most of a page.
    qa('.rail-controls').forEach(group => {
      const rail = q(group.dataset.rail || '.poster-rail');
      if (!rail) return;
      const buttons = qa('button', group);
      const sync = () => {
        const max = rail.scrollWidth - rail.clientWidth - 2;
        buttons.forEach(b => { b.disabled = Number(b.dataset.dir) < 0 ? rail.scrollLeft <= 2 : rail.scrollLeft >= max; });
      };
      buttons.forEach(b => b.addEventListener('click', () => rail.scrollBy({left: Number(b.dataset.dir) * rail.clientWidth * 0.8, behavior: 'smooth'})));
      rail.addEventListener('scroll', sync, {passive: true});
      window.addEventListener('resize', sync);
      sync();
    });

    document.addEventListener('pointerdown', e => {
      if (aboutOpen() && !e.target.closest('.lambda-popover,.profile')) closeAbout();
    });
    document.addEventListener('keydown', e => {
      if (e.key === 'Escape' && aboutOpen()) closeAbout();
    });
  }

  // Animated mouse-wheel scrolling. Qt WebEngine hands Chromium each Windows
  // wheel notch as a precise pixel delta, so Chromium's scroll animator never
  // runs and the page jumps ~100px per notch. Ease toward a target instead.
  // Touchpads, ctrl+wheel zoom, reduced motion and inner scrollers stay native.
  function smoothWheel() {
    if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) return;
    const root = document.scrollingElement || document.documentElement;
    let target = 0, current = 0, frame = 0, last = 0;

    const innerScroller = (el, dy) => {
      for (; el && el !== document.body && el !== root; el = el.parentElement) {
        const oy = getComputedStyle(el).overflowY;
        if ((oy === 'auto' || oy === 'scroll') &&
            (dy > 0 ? el.scrollTop < el.scrollHeight - el.clientHeight - 1 : el.scrollTop > 0)) return true;
      }
      return false;
    };

    const step = now => {
      // Someone else moved the page (scrollbar drag, keys, nav links): yield.
      if (Math.abs(window.scrollY - current) > 2) { frame = 0; return; }
      const dt = Math.min(64, now - (last || now - 16.7));
      last = now;
      target = Math.max(0, Math.min(root.scrollHeight - window.innerHeight, target));
      current += (target - current) * (1 - Math.pow(0.82, dt / 16.7));
      if (Math.abs(target - current) < 0.5) current = target;
      window.scrollTo({top: current, behavior: 'instant'});
      // Chromium rounds/clamps scroll positions; use the actual position so
      // rounding at the bottom cannot leave a permanent animation running.
      current = window.scrollY;
      frame = Math.abs(current - target) < 1 ? 0 : requestAnimationFrame(step);
    };

    window.addEventListener('wheel', e => {
      if (e.ctrlKey || e.defaultPrevented || !e.deltaY || Math.abs(e.deltaX) > Math.abs(e.deltaY)) return;
      const notch = e.deltaMode !== 0 || (e.wheelDeltaY && e.wheelDeltaY % 120 === 0);
      if (!notch || innerScroller(e.target, e.deltaY)) return;
      e.preventDefault();
      if (!frame) { target = current = window.scrollY; last = 0; }
      const unit = e.deltaMode === 1 ? 40 : e.deltaMode === 2 ? window.innerHeight : 1;
      target = Math.max(0, Math.min(root.scrollHeight - window.innerHeight, target + e.deltaY * unit));
      if (!frame) frame = requestAnimationFrame(step);
    }, {passive: false});
  }

  window.lambdaHome = {
    setRecents(list) { recents = Array.isArray(list) ? list : []; renderRecents(); renderHeroCta(); },
    setSession(next) { session = Object.assign({available: false, name: '', path: ''}, next || {}); renderRecents(); renderHeroCta(); },
    toast: (message, kind) => window.LambdaWindow && window.LambdaWindow.toast(message, kind)
  };

  const start = () => {
    bind();
    smoothWheel();
    renderRecents();
    renderHeroCta();
    new QWebChannel(qt.webChannelTransport, channel => {
      home = channel.objects.homeBridge;
      if (window.LambdaWindow && channel.objects.windowBridge) window.LambdaWindow.attach(channel.objects.windowBridge);
      if (window.lambdaStremio && channel.objects.stremio) window.lambdaStremio.attach(channel.objects.stremio, home);
      call('ready');
    });
  };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
  else start();
})();
