// LAMBDA Player Home page behaviour.
(() => {
  let home = null;
  let library = null;
  let localItems = [];
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
        badge: 'FOLDER', progress: 0, theme: THEMES[1], onOpen: () => library && library.chooseFolder(),
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
          onOpen: () => current ? call('resume') : item.online ? window.lambdaStremio.openSaved(item) : call('openRecent', item.path),
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

  function renderLibrary(items) {
    localItems = Array.isArray(items) ? items : [];
    const rail = q('.local-library-rail');
    rail.replaceChildren();
    localItems.forEach(item => {
      const card = document.createElement('button');
      card.type = 'button'; card.className = 'local-card';
      if (item.poster) { const image = document.createElement('img'); image.src = item.poster; image.alt = ''; image.loading = 'lazy'; card.appendChild(image); }
      const title = document.createElement('strong'); title.textContent = item.title; card.appendChild(title);
      const count = document.createElement('small'); count.textContent = item.type === 'movie' ? 'Movie' : item.episodes.length + ' episodes'; card.appendChild(count);
      card.addEventListener('click', () => library.state(items => { const fresh = items.find(i => i.folder === item.folder); if (fresh) openLocal(fresh); })); rail.appendChild(card);
    });
    q('.local-library-empty').hidden = localItems.length > 0;
  }
  function openLocal(item) {
    const dialog = q('#local-details');
    q('h2', dialog).textContent = item.title;
    const poster = q('img', dialog); poster.hidden = !item.poster; poster.src = item.poster || '';
    q('[data-local-art]', dialog).onclick = () => library.matchArtwork(item.folder);
    q('[data-local-refresh]', dialog).onclick = () => { library.refresh(item.folder); dialog.close(); };
    q('[data-local-remove]', dialog).onclick = () => { library.remove(item.folder); dialog.close(); };
    const list = q('.local-episodes', dialog); list.replaceChildren();
    item.episodes.forEach(e => {
      const row = document.createElement('button'); row.type = 'button'; row.className = 'local-episode';
      const title = document.createElement('strong'); title.textContent = e.season !== undefined ? 'S' + e.season + ' · E' + e.episode + ' — ' + e.name : e.name;
      const info = document.createElement('small'); info.textContent = e.watched ? 'Watched · Play again' : e.position > 0 ? 'Resume at ' + Math.floor(e.position / 60) + ':' + String(Math.floor(e.position % 60)).padStart(2, '0') : e.relative;
      row.append(title, info);
      row.onclick = () => { dialog.close(); library.play(item.folder, e.path); };
      list.appendChild(row);
    });
    if (!dialog.open) dialog.showModal();
    dialog.scrollTop = 0;
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
      '<div class="pop-head"><span class="brand-mark" aria-hidden="true">λ</span><div><strong>LAMBDA Player</strong><small class="pop-version"></small></div></div>' +
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
      else if (action === 'folder') library && library.chooseFolder();
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

  // Qt WebEngine can turn a physical wheel notch into an instant pixel scroll,
  // even with ScrollAnimatorEnabled. Animate discrete notches explicitly; the
  // page's expensive full-screen effects are removed in home.css so this is
  // cheap to paint. Touchpads, zoom and inner scrollers retain native input.
  function smoothWheel() {
    if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) return;
    const root = document.scrollingElement || document.documentElement;
    let target = 0, current = 0, velocity = 0, frame = 0, last = 0;

    const innerScroller = (el, dy) => {
      for (; el && el !== document.body && el !== root; el = el.parentElement) {
        const oy = getComputedStyle(el).overflowY;
        if ((oy === 'auto' || oy === 'scroll') &&
            (dy > 0 ? el.scrollTop < el.scrollHeight - el.clientHeight - 1 : el.scrollTop > 0)) return true;
      }
      return false;
    };

    const step = now => {
      if (Math.abs(window.scrollY - current) > 3) {
        // A scrollbar drag, keyboard command or navigation took over.
        frame = 0;
        return;
      }
      const dt = Math.min(.04, Math.max(.001, (now - (last || now - 16.7)) / 1000));
      last = now;
      target = Math.max(0, Math.min(root.scrollHeight - window.innerHeight, target));
      const omega = 18;
      const distance = current - target;
      const impulse = (velocity + omega * distance) * dt;
      const decay = Math.exp(-omega * dt);
      current = target + (distance + impulse) * decay;
      velocity = (velocity - omega * impulse) * decay;
      if (Math.abs(target - current) < .5 && Math.abs(velocity) < 4) {
        current = target;
        velocity = 0;
      }
      window.scrollTo({top: current, behavior: 'instant'});
      current = window.scrollY;
      frame = Math.abs(target - current) < 1 && Math.abs(velocity) < 4
        ? 0 : requestAnimationFrame(step);
    };

    window.addEventListener('wheel', e => {
      if (e.ctrlKey || e.defaultPrevented || !e.deltaY || Math.abs(e.deltaX) > Math.abs(e.deltaY)) return;
      const wheelDelta = e.wheelDeltaY || e.wheelDelta || 0;
      const notch = e.deltaMode !== 0 || Math.abs(wheelDelta) >= 120
          || Math.abs(e.deltaY) >= 24;
      if (!notch || innerScroller(e.target, e.deltaY)) return;
      e.preventDefault();
      if (!frame) { target = current = window.scrollY; velocity = 0; last = 0; }
      const unit = e.deltaMode === 1 ? 40 : e.deltaMode === 2 ? window.innerHeight : 1;
      target = Math.max(0, Math.min(root.scrollHeight - window.innerHeight, target + e.deltaY * unit * 1.6));
      if (!frame) frame = requestAnimationFrame(step);
    }, {passive: false});
  }

  window.lambdaHome = {
    openOnline(item) { if (window.lambdaStremio) window.lambdaStremio.openSaved(item); },
    setRecents(list) { recents = Array.isArray(list) ? list : []; renderRecents(); renderHeroCta(); },
    setSession(next) { session = Object.assign({available: false, name: '', path: ''}, next || {}); renderRecents(); renderHeroCta(); },
    toast: (message, kind) => window.LambdaWindow && window.LambdaWindow.toast(message, kind)
  };

  const start = () => {
    bind();
    q('[data-library-add]').onclick = () => library && library.chooseFolder();
    q('[data-local-close]').onclick = () => q('#local-details').close();
    q('#local-details').addEventListener('click', e => { if (e.target === e.currentTarget) e.currentTarget.close(); });
    smoothWheel();
    renderRecents();
    renderHeroCta();
    new QWebChannel(qt.webChannelTransport, channel => {
      home = channel.objects.homeBridge;
      library = channel.objects.library;
      if (library) { library.changed.connect(renderLibrary); library.state(renderLibrary); }
      if (window.LambdaWindow && channel.objects.windowBridge) window.LambdaWindow.attach(channel.objects.windowBridge);
      if (window.lambdaStremio && channel.objects.stremio) window.lambdaStremio.attach(channel.objects.stremio, home);
      call('ready');
    });
  };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
  else start();
})();
