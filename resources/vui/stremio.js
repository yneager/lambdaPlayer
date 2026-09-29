// LAMBDA Player — Stremio add-on views: Home rows, Discover, Search, Details,
// Sources and Add-ons. Protocol decisions (which add-on gets which request,
// parsing, capability matching) are made in C++ (src/stremio, AddonsBridge);
// this file renders the per-add-on result slots it receives, in add-on order,
// the way stremio-web renders stremio-core's models.
(() => {
  'use strict';

  const q = (s, root = document) => root.querySelector(s);
  const qa = (s, root = document) => Array.from(root.querySelectorAll(s));

  let bridge = null;
  let tokenSeq = 0;
  const newToken = prefix => prefix + ':' + (++tokenSeq);

  const state = {
    addons: [],
    server: {},
    defaults: [],
    view: 'home',
    history: [],
    local: {session: {available: false}, recents: []},
  };

  // token -> callback, and per-view token groups for cancellation
  const handlers = {
    catalog: new Map(),
    metaPlan: new Map(),
    metaResult: new Map(),
    streamsPlan: new Map(),
    streamsResult: new Map(),
    install: new Map(),
  };
  const groups = {home: new Set(), discover: new Set(), search: new Set(), details: new Set(), addons: new Set()};

  // ---- Small helpers ----------------------------------------------------------
  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = text;
    return node;
  }

  function img(url, className) {
    const image = el('img', className);
    image.alt = '';
    image.loading = 'lazy';
    image.decoding = 'async';
    image.referrerPolicy = 'no-referrer';
    image.addEventListener('error', () => {
      const parent = image.parentElement;
      image.remove();
      if (parent) parent.classList.add('no-image');
    });
    image.src = url;
    return image;
  }

  function toast(message, warning) {
    if (window.LambdaWindow) window.LambdaWindow.toast(message, warning ? 'warn' : 'info');
  }

  function refreshWindow() {
    if (window.LambdaWindow) window.LambdaWindow.refresh();
  }

  const TYPE_LABELS = {movie: 'Movies', series: 'Series', channel: 'Channels', tv: 'TV', other: 'Other', anime: 'Anime', all: 'All'};
  const typeLabel = type => TYPE_LABELS[type] || (type ? type.charAt(0).toUpperCase() + type.slice(1) : '');
  const TYPE_SINGULAR = {movie: 'Movie', series: 'Series', channel: 'Channel', tv: 'TV', other: 'Other', anime: 'Anime'};
  const typeSingular = type => TYPE_SINGULAR[type] || typeLabel(type);

  function formatBytes(bytes) {
    const n = Number(bytes);
    if (!isFinite(n) || n <= 0) return '';
    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
    let value = n;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) { value /= 1024; unit++; }
    return value.toFixed(value >= 100 || unit === 0 ? 0 : 1) + ' ' + units[unit];
  }

  function formatDate(iso) {
    if (!iso) return '';
    const date = new Date(iso);
    if (isNaN(date.getTime())) return '';
    return date.toLocaleDateString(undefined, {year: 'numeric', month: 'short', day: 'numeric'});
  }

  function initials(name) {
    return String(name || '?').split(/\s+/).filter(Boolean).slice(0, 2).map(w => w[0]).join('').toUpperCase() || '?';
  }

  function hashHue(text) {
    let h = 0;
    for (const ch of String(text || '')) h = (h * 31 + ch.charCodeAt(0)) >>> 0;
    return h % 360;
  }

  // ---- Bridge requests ----------------------------------------------------------
  function loadCatalog(group, request, force, done) {
    const token = newToken('cat');
    groups[group].add(token);
    handlers.catalog.set(token, result => {
      handlers.catalog.delete(token);
      groups[group].delete(token);
      done(result);
    });
    bridge.loadCatalog(token, request, !!force);
    return token;
  }

  function cancelToken(token) {
    if (!token) return;
    bridge.cancel(token);
    for (const map of Object.values(handlers)) map.delete(token);
    for (const set of Object.values(groups)) set.delete(token);
  }

  function cancelGroup(group) {
    for (const token of Array.from(groups[group])) cancelToken(token);
  }

  // ---- Views ----------------------------------------------------------------------
  function setActiveLink(view) {
    const linkView = view === 'details' || view === 'search' ? (state.history.length ? state.history[state.history.length - 1].view : 'home') : view;
    qa('.nav-links a').forEach(a => a.classList.toggle('active', a.dataset.viewLink === linkView));
  }

  function show(view, options = {}) {
    if (!q('main.view[data-view="' + view + '"]')) return;
    const leaving = state.view;
    if (!options.back && leaving !== view) {
      if (view === 'details' || view === 'search') {
        state.history.push({view: leaving, scroll: window.scrollY});
      } else {
        state.history = [];
      }
    }
    if (leaving === 'details' && view !== 'details') {
      finishCardFlight();
      q('.details-view').classList.remove('shared-entry');
      cancelDetails();
    }
    state.view = view;
    qa('main.view').forEach(m => { m.hidden = m.dataset.view !== view; });
    setActiveLink(view);
    document.body.dataset.view = view;
    window.scrollTo({top: options.scroll || 0, behavior: 'instant'});

    if (view === 'discover' && !discover.initialized) openDiscover(null);
    if (view === 'addons') { renderAddons(); loadAddonCatalogs(false); }
    if (view === 'home') observeRows();
    refreshWindow();
  }

  function back() {
    const previous = state.history.pop();
    show(previous ? previous.view : 'home', {back: true, scroll: previous ? previous.scroll : 0});
  }

  // ---- Meta cards -------------------------------------------------------------------
  function metaCard(meta, context) {
    const shape = meta.posterShape === 'landscape' || meta.posterShape === 'square' ? meta.posterShape : 'poster';
    const card = el('a', 'meta-card shape-' + shape);
    card.href = '#';
    card.title = meta.name || meta.id;
    const art = el('div', 'meta-art');
    art.style.setProperty('--hue', hashHue(meta.name || meta.id));
    if (meta.poster) art.appendChild(img(meta.poster));
    else art.classList.add('no-image');
    art.appendChild(el('span', 'meta-initials', initials(meta.name || meta.id)));
    card.appendChild(art);
    card.appendChild(el('div', 'meta-shade'));
    if (meta.imdbRating) card.appendChild(el('span', 'meta-rating', '★ ' + meta.imdbRating));
    const info = el('div', 'meta-info');
    info.appendChild(el('strong', null, meta.name || meta.id));
    const sub = [meta.releaseInfo, typeSingular(meta.type)].filter(Boolean).join(' · ');
    info.appendChild(el('small', null, sub));
    card.appendChild(info);
    card.addEventListener('click', e => { e.preventDefault(); openDetails(meta, context, card); });
    return card;
  }

  function skeletonCards(count, shape) {
    const fragment = document.createDocumentFragment();
    for (let i = 0; i < count; i++) {
      const s = el('div', 'meta-card skeleton shape-' + (shape || 'poster'));
      s.appendChild(el('div', 'meta-art'));
      fragment.appendChild(s);
    }
    return fragment;
  }

  // ---- Catalog rows (Board and Search) -------------------------------------------------
  // catalogs_with_extra.rs: one row per planned catalog request, loaded when
  // it comes into view (LoadRange), paged with "skip" only when declared.
  const rowStates = new Map(); // key -> row state (home rows)
  let rowObserver = null;

  function createRow(row, group, onReady) {
    const section = el('section', 'content-stage addon-row');
    const head = el('div', 'section-head');
    const titles = el('div');
    titles.appendChild(el('span', 'eyebrow', (row.addon && row.addon.name ? row.addon.name.toUpperCase() : '') + ' · ' + typeLabel(row.type).toUpperCase()));
    titles.appendChild(el('h2', null, row.name));
    head.appendChild(titles);
    const controls = el('div', 'rail-controls');
    const left = el('button', null, '←');
    left.type = 'button'; left.title = 'Scroll left'; left.setAttribute('aria-label', 'Scroll left');
    const right = el('button', null, '→');
    right.type = 'button'; right.title = 'Scroll right'; right.setAttribute('aria-label', 'Scroll right');
    controls.append(left, right);
    head.appendChild(controls);
    section.appendChild(head);
    const rail = el('div', 'rail meta-rail');
    rail.appendChild(skeletonCards(8));
    section.appendChild(rail);

    const rs = {row, group, section, rail, requested: false, loading: false, done: false, count: 0, lastRequest: null, lastCount: 0, onReady, status: 'idle'};
    const sync = () => {
      const max = rail.scrollWidth - rail.clientWidth - 2;
      left.disabled = rail.scrollLeft <= 2;
      right.disabled = rail.scrollLeft >= max && (rs.done || !row.supportsSkip);
      if (rail.scrollLeft > max - rail.clientWidth * 1.5) loadNextRowPage(rs);
    };
    left.addEventListener('click', () => rail.scrollBy({left: -rail.clientWidth * 0.8, behavior: 'smooth'}));
    right.addEventListener('click', () => rail.scrollBy({left: rail.clientWidth * 0.8, behavior: 'smooth'}));
    rail.addEventListener('scroll', sync, {passive: true});
    rs.sync = sync;
    section._row = rs;
    return rs;
  }

  function loadRowPage(rs, request, first) {
    rs.loading = true;
    rs.status = first ? 'loading' : rs.status;
    loadCatalog(rs.group, request, false, result => {
      rs.loading = false;
      if (first) rs.rail.innerHTML = '';
      if (result.status === 'ready') {
        const metas = result.metas || [];
        const shape = metas[0] && metas[0].posterShape;
        if (first && shape) rs.section.dataset.shape = shape;
        metas.forEach(meta => rs.rail.appendChild(metaCard(meta, {addon: rs.row.addon})));
        rs.count += metas.length;
        rs.lastRequest = request;
        rs.lastCount = metas.length;
        rs.status = 'ready';
        if (first) { rs.rail.classList.remove('rail-enter'); void rs.rail.offsetWidth; rs.rail.classList.add('rail-enter'); }
        if (rs.onReady) rs.onReady(rs, metas);
      } else if (result.status === 'empty') {
        rs.done = true;
        if (first) {
          // stremio-web hides EmptyContent rows.
          rs.status = 'empty';
          rs.section.hidden = true;
          if (rs.onReady) rs.onReady(rs, []);
        }
      } else {
        rs.done = true;
        if (first) {
          rs.status = 'error';
          const error = el('div', 'row-error');
          error.appendChild(el('span', null, result.error || 'The add-on did not answer.'));
          const retry = el('button', 'pill-btn', 'Retry');
          retry.type = 'button';
          retry.addEventListener('click', () => { rs.done = false; rs.rail.innerHTML = ''; rs.rail.appendChild(skeletonCards(6)); loadRowPage(rs, rs.row.request, true); });
          error.appendChild(retry);
          rs.rail.appendChild(error);
          if (rs.onReady) rs.onReady(rs, []);
        }
      }
      if (rs.sync) requestAnimationFrame(rs.sync);
      refreshWindow();
    });
  }

  function loadNextRowPage(rs) {
    if (rs.loading || rs.done || !rs.row.supportsSkip || !rs.lastRequest || rs.status !== 'ready') return;
    rs.loading = true;
    bridge.nextPage(rs.lastRequest, rs.lastCount, next => {
      rs.loading = false;
      if (!next) { rs.done = true; return; }
      loadRowPage(rs, next, false);
    });
  }

  function ensureObserver() {
    if (rowObserver) return rowObserver;
    rowObserver = new IntersectionObserver(entries => {
      entries.forEach(entry => {
        const rs = entry.target._row;
        if (entry.isIntersecting && rs && !rs.requested) {
          rs.requested = true;
          loadRowPage(rs, rs.row.request, true);
          rowObserver.unobserve(entry.target);
        }
      });
    }, {rootMargin: '700px 0px'});
    return rowObserver;
  }

  function observeRows() {
    const observer = ensureObserver();
    rowStates.forEach(rs => { if (!rs.requested) observer.observe(rs.section); });
  }

  // ---- Home ----------------------------------------------------------------------------
  const hero = {rowIndex: Infinity, meta: null, row: null};
  let boardOrder = [];

  function planBoard() {
    bridge.boardRows(rows => {
      const container = q('.addon-rows');
      const keys = new Set(rows.map(r => r.key));
      // Rows that are no longer planned (addon removed/disabled) go away;
      // rows that stay keep what they already loaded.
      Array.from(rowStates.keys()).forEach(key => {
        if (!keys.has(key)) { rowStates.get(key).section.remove(); rowStates.delete(key); }
      });
      boardOrder = rows.map(r => r.key);
      rows.forEach((row, index) => {
        let rs = rowStates.get(row.key);
        if (!rs) {
          rs = createRow(row, 'home', (state, metas) => considerHero(index, state, metas));
          rowStates.set(row.key, rs);
        }
        rs.index = index;
        container.appendChild(rs.section);
      });
      if (hero.row && !keys.has(hero.row.key)) resetHero();
      observeRows();
      updateHomeEmpty(rows.length);
      refreshWindow();
    });
  }

  function updateHomeEmpty(rowCount) {
    const empty = q('.addons-empty');
    empty.hidden = state.addons.length > 0;
    const hasDefaults = state.defaults.every(url => state.addons.some(a => a.transportUrl === url));
    qa('[data-install-defaults]').forEach(b => { b.hidden = hasDefaults; });
    const notice = q('.no-catalogs');
    if (state.addons.length > 0 && rowCount === 0) {
      if (!notice) {
        const box = el('section', 'content-stage no-catalogs');
        const card = el('div', 'empty-card glass compact');
        const copy = el('div', 'empty-copy');
        copy.appendChild(el('span', 'eyebrow', 'NO CATALOGS'));
        copy.appendChild(el('h2', null, 'Your add-ons don’t provide catalogs'));
        copy.appendChild(el('p', null, 'Stream and subtitle add-ons work from Details pages. Install a catalog add-on such as Cinemeta to fill Home and Discover.'));
        card.appendChild(copy);
        box.appendChild(card);
        q('.addon-rows').after(box);
      }
    } else if (notice) {
      notice.remove();
    }
  }

  function considerHero(index, rs, metas) {
    if (!metas.length || index >= hero.rowIndex) return;
    const pick = metas.find(m => m.background) || metas.find(m => m.poster) || metas[0];
    hero.rowIndex = index;
    hero.meta = pick;
    hero.row = rs.row;
    renderHero();
  }

  function resetHero() {
    hero.rowIndex = Infinity;
    hero.meta = null;
    hero.row = null;
    renderHero();
    rowStates.forEach(rs => { if (rs.status === 'ready') { /* re-offer the hero */ } });
  }

  function setLocalState(session, recents) {
    state.local = {session: session || {available: false}, recents: recents || []};
    renderHero();
  }

  function renderHero() {
    const heroEl = q('.hero');
    if (!heroEl) return;
    const backdrop = q('.hero-backdrop', heroEl);
    const kicker = q('.hero-kicker-text', heroEl);
    const title = q('.hero-title', heroEl);
    const logo = q('.hero-logo', heroEl);
    const description = q('.hero-description', heroEl);
    const meta = q('.hero-meta', heroEl);
    const cta = q('.primary-cta', heroEl);
    const ctaLabel = q('.primary-cta-label', heroEl);
    const secondary = q('[data-hero-secondary]', heroEl);
    const secondaryLabel = q('.secondary-cta-label', heroEl);
    const index = q('.hero-index', heroEl);
    const session = state.local.session || {};
    const recents = state.local.recents || [];
    meta.innerHTML = '';

    if (hero.meta) {
      const m = hero.meta;
      heroEl.classList.add('featured');
      backdrop.style.backgroundImage = m.background || m.poster ? 'url("' + String(m.background || m.poster).replace(/"/g, '%22') + '")' : '';
      kicker.textContent = 'FEATURED · ' + ((hero.row.addon && hero.row.addon.name) || '').toUpperCase();
      if (m.logo) {
        logo.hidden = false;
        logo.src = m.logo;
        logo.alt = m.name || '';
        logo.onerror = () => { logo.hidden = true; title.hidden = false; };
        title.hidden = true;
      } else {
        logo.hidden = true;
        title.hidden = false;
      }
      title.textContent = m.name || m.id;
      const text = m.description || '';
      description.textContent = text.length > 240 ? text.slice(0, 237).trimEnd() + '…' : text;
      description.hidden = !text;
      [m.releaseInfo, m.runtime, m.imdbRating ? '★ ' + m.imdbRating : '', typeSingular(m.type)].filter(Boolean)
        .forEach((value, i) => meta.appendChild(el('span', i === 2 && m.imdbRating ? 'match' : null, value)));
      (m.genres || []).slice(0, 2).forEach(g => meta.appendChild(el('span', 'quality', g.toUpperCase())));
      ctaLabel.textContent = 'Watch';
      cta.dataset.action = 'featured';
      cta.title = 'Show details and sources for ' + (m.name || '');
      secondaryLabel.textContent = session.available ? 'Resume ' + (session.name || 'video') : 'Discover';
      secondary.dataset.heroSecondary = session.available ? 'resume' : 'discover';
      setSecondaryIcon(secondary, session.available ? 'play' : 'discover');
      index.hidden = false;
      q('.hero-index-kicker', index).textContent = 'FEATURED · ' + typeLabel(hero.row.type).toUpperCase();
      q('.hero-index-title', index).textContent = (hero.row.name || '').toUpperCase();
    } else {
      heroEl.classList.remove('featured');
      backdrop.style.backgroundImage = '';
      kicker.textContent = 'LAMBDA PLAYER';
      logo.hidden = true;
      title.hidden = false;
      title.innerHTML = 'Your films,<br><em>beautifully played</em>';
      description.hidden = false;
      description.textContent = state.addons.length
        ? 'Play videos from this PC with libmpv and optional real-time RIFE frame interpolation, and browse what your add-ons offer below.'
        : 'Play videos from this PC with libmpv and optional real-time RIFE frame interpolation. Add Stremio add-ons to browse catalogs and find sources.';
      ctaLabel.textContent = session.available ? 'Resume' : (recents.length ? 'Play recent' : 'Open video');
      cta.dataset.action = 'resume';
      cta.title = session.available ? 'Resume ' + (session.name || 'current video')
        : recents.length ? 'Play ' + recents[0].name : 'Open a video from this PC';
      secondaryLabel.textContent = state.addons.length ? 'Discover' : 'Add add-ons';
      secondary.dataset.heroSecondary = state.addons.length ? 'discover' : 'addons';
      setSecondaryIcon(secondary, state.addons.length ? 'discover' : 'add');
      index.hidden = true;
    }
  }

  const HERO_ICONS = {
    add: '<path d="M12 5v14M5 12h14"/>',
    discover: '<circle cx="12" cy="12" r="8.5"/><path d="m15.2 8.8-2 4.4-4.4 2 2-4.4z"/>',
    play: '<path d="m9 7 8 5-8 5z"/>',
  };
  function setSecondaryIcon(button, name) {
    const svg = q('svg', button);
    if (svg && svg.dataset.icon !== name) { svg.innerHTML = HERO_ICONS[name]; svg.dataset.icon = name; }
  }

  // ---- Discover -------------------------------------------------------------------------
  // catalog_with_filters.rs: type -> catalog -> extra filters, paged with skip.
  const discover = {initialized: false, model: null, selected: null, pageSizes: [], loading: false};
  let discoverObserver = null;

  function openDiscover(request) {
    discover.initialized = true;
    cancelGroup('discover');
    discover.pageSizes = [];
    discover.loading = false;
    const grid = q('.discover-grid');
    grid.innerHTML = '';
    setDiscoverStatus('');
    if (request && state.view === 'discover') window.scrollTo({top: 0, behavior: 'instant'});
    bridge.discover(request, [], model => {
      discover.model = model;
      discover.selected = model.selected;
      renderDiscoverFilters(model);
      if (!model.selected) {
        setDiscoverStatus(state.addons.length ? 'None of your add-ons has a catalog to browse.' : 'Install add-ons to browse catalogs here.', true);
        return;
      }
      grid.appendChild(skeletonCards(12));
      loadDiscoverPage(model.selected, true);
    });
  }

  function setDiscoverStatus(text, withAction) {
    const status = q('.discover-status');
    status.innerHTML = '';
    if (!text) return;
    status.appendChild(el('span', null, text));
    if (withAction && !state.addons.length) {
      const b = el('button', 'pill-btn primary', 'Go to Add-ons');
      b.type = 'button';
      b.addEventListener('click', () => show('addons'));
      status.appendChild(b);
    }
  }

  function loadDiscoverPage(request, first) {
    discover.loading = true;
    const grid = q('.discover-grid');
    loadCatalog('discover', request, false, result => {
      discover.loading = false;
      if (first) grid.innerHTML = '';
      if (result.status === 'ready') {
        (result.metas || []).forEach(meta => grid.appendChild(metaCard(meta, {addon: result.addon})));
        discover.pageSizes.push((result.metas || []).length);
        setDiscoverStatus('');
      } else {
        discover.pageSizes.push(0);
        if (first) setDiscoverStatus(result.status === 'empty' ? 'This catalog is empty.' : 'The add-on did not answer: ' + (result.error || 'unknown error'));
      }
      bridge.discover(discover.selected, discover.pageSizes, model => {
        discover.model = model;
        observeDiscoverSentinel();
      });
      refreshWindow();
    });
  }

  function observeDiscoverSentinel() {
    if (!discoverObserver) {
      discoverObserver = new IntersectionObserver(entries => {
        if (entries.some(e => e.isIntersecting) && discover.model && discover.model.nextPage && !discover.loading && state.view === 'discover') {
          loadDiscoverPage(discover.model.nextPage, false);
        }
      }, {rootMargin: '900px 0px'});
      discoverObserver.observe(q('.discover-sentinel'));
    } else {
      // Re-check: the sentinel may already be in view.
      discoverObserver.unobserve(q('.discover-sentinel'));
      discoverObserver.observe(q('.discover-sentinel'));
    }
  }

  function chip(label, selected, onClick, extraClass) {
    const b = el('button', 'chip' + (selected ? ' selected' : '') + (extraClass ? ' ' + extraClass : ''), label);
    b.type = 'button';
    b.addEventListener('click', onClick);
    return b;
  }

  function renderDiscoverFilters(model) {
    const types = q('.discover-types');
    const catalogs = q('.discover-catalogs');
    const extra = q('.discover-extra');
    types.innerHTML = '';
    catalogs.innerHTML = '';
    extra.innerHTML = '';
    (model.types || []).forEach(t => types.appendChild(chip(typeLabel(t.type), t.selected, () => openDiscover(t.request))));
    (model.catalogs || []).forEach(c => {
      const b = chip(c.name, c.selected, () => openDiscover(c.request), 'catalog-chip');
      b.appendChild(el('small', null, c.addonName));
      catalogs.appendChild(b);
    });
    (model.extra || []).forEach(x => {
      const label = el('label', 'extra-select');
      label.appendChild(el('span', null, x.name.charAt(0).toUpperCase() + x.name.slice(1)));
      const select = el('select');
      x.options.forEach((option, index) => {
        const o = el('option', null, option.value === null ? 'Any' : option.value);
        o.value = String(index);
        o.selected = option.selected;
        select.appendChild(o);
      });
      select.addEventListener('change', () => openDiscover(x.options[Number(select.value)].request));
      label.appendChild(select);
      extra.appendChild(label);
    });
    refreshWindow();
  }

  // ---- Search ---------------------------------------------------------------------------
  // CatalogsWithExtra { extra: [search=query] }: only catalogs that declare
  // the "search" extra are asked.
  let searchState = null;

  function runSearch(query) {
    query = String(query || '').trim();
    if (!query) return;
    cancelGroup('search');
    const rowsEl = q('.search-rows');
    rowsEl.innerHTML = '';
    q('.search-title').textContent = 'Results for “' + query + '”';
    const status = q('.search-status');
    status.textContent = '';
    if (state.view !== 'search') show('search');
    else window.scrollTo({top: 0, behavior: 'instant'});
    bridge.searchRows(query, rows => {
      searchState = {query, pending: rows.length, found: 0};
      if (!rows.length) {
        status.textContent = state.addons.length ? 'None of your add-ons can search.' : 'Install add-ons to search their catalogs.';
        return;
      }
      rows.forEach(row => {
        const rs = createRow(row, 'search', (_, metas) => {
          searchState.pending--;
          if (metas.length) searchState.found++;
          if (searchState.pending <= 0 && !searchState.found) status.textContent = 'No results.';
        });
        rowsEl.appendChild(rs.section);
        rs.requested = true;
        loadRowPage(rs, row.request, true);
      });
      refreshWindow();
    });
  }

  // ---- Details --------------------------------------------------------------------------
  // meta_details.rs: meta from every add-on that supports it; the first Ready
  // in add-on order is shown (serialize_meta_details). Streams are asked for
  // stream/{meta type}/{video id}.
  let details = null;
  let cardFlight = null;

  function finishCardFlight() {
    if (!cardFlight) return;
    const flight = cardFlight;
    cardFlight = null;
    clearTimeout(flight.guard);
    clearTimeout(flight.settleTimer);
    flight.target.classList.remove('flight-target');
    flight.clone.remove();
  }

  function settleCardFlight() {
    if (!cardFlight || cardFlight.settling) return;
    cardFlight.settling = true;
    cardFlight.target.classList.remove('flight-target');
    cardFlight.clone.classList.add('is-settling');
    cardFlight.settleTimer = setTimeout(finishCardFlight, 300);
  }

  function flyCardToPoster(clone, source) {
    const target = q('.details-poster');
    const destination = target.getBoundingClientRect();
    if (source.width < 20 || destination.width < 20 || destination.height < 20) return;
    clone.classList.add('card-flight');
    clone.removeAttribute('href');
    clone.setAttribute('aria-hidden', 'true');
    clone.tabIndex = -1;
    Object.assign(clone.style, {
      left: destination.left + 'px', top: destination.top + 'px',
      width: destination.width + 'px', height: destination.height + 'px'
    });
    clone.style.setProperty('--flight-x', (source.left - destination.left) + 'px');
    clone.style.setProperty('--flight-y', (source.top - destination.top) + 'px');
    clone.style.setProperty('--flight-sx', source.width / destination.width);
    clone.style.setProperty('--flight-sy', source.height / destination.height);
    document.body.appendChild(clone);
    target.classList.add('flight-target');
    cardFlight = {clone, target, guard: 0, settleTimer: 0, settling: false};
    // Commit the first position before starting the compositor transition.
    void clone.offsetWidth;
    requestAnimationFrame(() => {
      if (cardFlight && cardFlight.clone === clone) clone.classList.add('is-flying');
    });
    clone.addEventListener('transitionend', e => {
      if (e.propertyName === 'transform') settleCardFlight();
    });
    cardFlight.guard = setTimeout(settleCardFlight, 1230);
  }

  function cancelDetails() {
    if (!details) return;
    cancelGroup('details');
    details = null;
  }

  function openDetails(preview, context, originCard = null) {
    finishCardFlight();
    const flightEnabled = !!originCard && !window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    const source = flightEnabled ? originCard.getBoundingClientRect() : null;
    const clone = flightEnabled ? originCard.cloneNode(true) : null;
    cancelDetails();
    const token = newToken('meta');
    details = {token, type: preview.type, id: preview.id, preview, slots: [], results: [], meta: null, metaSlot: -1,
               streamsToken: null, video: null, season: null, context: context || {}};
    groups.details.add(token);
    q('.details-view').classList.toggle('shared-entry', flightEnabled);
    show('details');
    renderDetailsHeader(preview, null);
    if (clone) flyCardToPoster(clone, source);
    q('.episodes-panel').hidden = true;
    q('.sources-panel').hidden = true;
    q('.details-actions').innerHTML = '';
    setDetailsState('loading', 'Loading details…');

    handlers.metaPlan.set(token, slots => {
      if (!details || details.token !== token) return;
      details.slots = slots;
      details.results = slots.map(() => null);
      if (!slots.length) {
        setDetailsState('error', 'None of your add-ons provides details for this ' + typeSingular(details.type).toLowerCase() + '.');
      }
    });
    handlers.metaResult.set(token, (index, result) => {
      if (!details || details.token !== token) return;
      details.results[index] = result;
      pickMeta();
    });
    bridge.loadMeta(token, preview.type, preview.id, false);
  }

  function pickMeta() {
    const results = details.results;
    const ready = results.findIndex(r => r && r.status === 'ready' && r.meta);
    if (ready >= 0) {
      if (ready !== details.metaSlot) {
        details.metaSlot = ready;
        details.meta = results[ready].meta;
        renderDetails();
      }
      return;
    }
    if (results.length && results.every(r => r && r.status !== 'ready')) {
      const first = results[0];
      setDetailsState('error', 'Details could not be loaded' + (first && first.error ? ': ' + first.error : '.'));
    }
  }

  function setDetailsState(kind, text) {
    const box = q('.details-state');
    box.className = 'details-state ' + (kind || '');
    box.textContent = text || '';
  }

  function renderDetailsHeader(meta, addon) {
    const backdrop = q('.details-backdrop');
    const image = meta.background || meta.poster;
    backdrop.style.backgroundImage = image ? 'url("' + String(image).replace(/"/g, '%22') + '")' : '';
    const poster = q('.details-poster');
    poster.innerHTML = '';
    poster.classList.remove('no-image');
    poster.style.setProperty('--hue', hashHue(meta.name || meta.id));
    if (meta.poster) poster.appendChild(img(meta.poster)); else poster.classList.add('no-image');
    q('.details-kicker').textContent = [typeSingular(meta.type).toUpperCase(), addon ? 'DETAILS FROM ' + addon.name.toUpperCase() : ''].filter(Boolean).join(' · ');
    const logo = q('.details-logo');
    const title = q('.details-title');
    title.textContent = meta.name || meta.id;
    if (meta.logo) {
      logo.hidden = false;
      logo.src = meta.logo;
      logo.alt = meta.name || '';
      logo.onerror = () => { logo.hidden = true; title.hidden = false; };
      title.hidden = true;
    } else {
      logo.hidden = true;
      title.hidden = false;
    }
    const metaLine = q('.details-meta');
    metaLine.innerHTML = '';
    [meta.releaseInfo, meta.runtime, meta.imdbRating ? '★ ' + meta.imdbRating : ''].filter(Boolean)
      .forEach(value => metaLine.appendChild(el('span', value.startsWith('★') ? 'match' : null, value)));
    q('.details-description').textContent = meta.description || '';
    const genres = q('.details-genres');
    genres.innerHTML = '';
    (meta.genres || []).forEach(g => genres.appendChild(el('span', 'genre-pill', g)));
  }

  // meta_details.rs selected_guess_stream_update
  function guessVideoId(meta) {
    if (meta.defaultVideoId) return meta.defaultVideoId;
    if (!meta.videos || !meta.videos.length || meta.isLive) return meta.id;
    return null;
  }

  function renderDetails() {
    const meta = details.meta;
    const slot = details.slots[details.metaSlot] || {};
    details.metaBase = slot.request ? slot.request.base : '';
    renderDetailsHeader(meta, slot.addon);
    setDetailsState('', '');
    const actions = q('.details-actions');
    actions.innerHTML = '';

    const guessed = guessVideoId(meta);
    if (guessed) {
      const play = el('button', 'primary-cta', null);
      play.type = 'button';
      play.appendChild(el('span', 'play-glyph'));
      play.appendChild(el('span', null, 'Sources'));
      play.addEventListener('click', () => loadStreams({id: guessed, title: meta.name}));
      actions.appendChild(play);
    }
    (meta.trailerStreams || []).slice(0, 1).forEach(trailer => {
      if (!trailer.ytId) return;
      const b = el('button', 'secondary-cta glass', 'Trailer');
      b.type = 'button';
      b.title = 'Open the trailer on YouTube';
      b.addEventListener('click', () => bridge.openExternal('https://www.youtube.com/watch?v=' + encodeURIComponent(trailer.ytId)));
      actions.appendChild(b);
    });
    (meta.links || []).filter(l => l.category === 'imdb' && /^https?:/.test(l.url)).slice(0, 1).forEach(link => {
      const b = el('button', 'secondary-cta glass', 'IMDb');
      b.type = 'button';
      b.addEventListener('click', () => bridge.openExternal(link.url));
      actions.appendChild(b);
    });

    renderEpisodes(meta);
    if (guessed && !(meta.videos || []).some(v => v.season !== undefined)) {
      // Movies and live channels: sources right away (Stremio's guessed stream path).
      loadStreams({id: guessed, title: meta.name});
    }
    refreshWindow();
  }

  function renderEpisodes(meta) {
    const panel = q('.episodes-panel');
    const videos = meta.videos || [];
    if (!videos.length || meta.isLive) { panel.hidden = true; return; }
    panel.hidden = false;
    const hasSeasons = videos.some(v => v.season !== undefined);
    q('.panel-head strong', panel).textContent = hasSeasons ? 'Episodes' : 'Videos';
    const seasons = hasSeasons ? Array.from(new Set(videos.filter(v => v.season !== undefined).map(v => v.season))) : [];
    // stremio-core sorts season 0 (specials) last.
    seasons.sort((a, b) => (a === 0 ? Infinity : a) - (b === 0 ? Infinity : b));
    const select = q('.season-select', panel);
    select.innerHTML = '';
    if (hasSeasons) {
      if (details.season === null || !seasons.includes(details.season)) {
        const now = Date.now();
        details.season = seasons.find(s => s !== 0 && videos.some(v => v.season === s && (!v.released || Date.parse(v.released) <= now))) ?? seasons[0];
      }
      seasons.forEach(season => select.appendChild(chip(season === 0 ? 'Specials' : 'Season ' + season, season === details.season, () => {
        details.season = season;
        renderEpisodes(meta);
      })));
    }
    const list = q('.episode-list', panel);
    list.innerHTML = '';
    const shown = hasSeasons ? videos.filter(v => v.season === details.season) : videos;
    const now = Date.now();
    shown.forEach(video => {
      const row = el('button', 'episode' + (details.video && details.video.id === video.id ? ' selected' : ''));
      row.type = 'button';
      const thumb = el('div', 'episode-thumb');
      thumb.style.setProperty('--hue', hashHue(video.title || video.id));
      if (video.thumbnail) thumb.appendChild(img(video.thumbnail)); else thumb.classList.add('no-image');
      if (video.episode !== undefined) thumb.appendChild(el('span', 'episode-number', 'E' + video.episode));
      row.appendChild(thumb);
      const copy = el('div', 'episode-copy');
      const heading = video.episode !== undefined ? video.episode + '. ' + (video.title || 'Episode ' + video.episode) : (video.title || video.id);
      copy.appendChild(el('strong', null, heading));
      const upcoming = video.released && Date.parse(video.released) > now;
      const date = formatDate(video.released);
      if (date) copy.appendChild(el('small', upcoming ? 'upcoming' : null, upcoming ? 'Upcoming · ' + date : date));
      if (video.overview) copy.appendChild(el('p', null, video.overview));
      row.appendChild(copy);
      row.addEventListener('click', () => {
        qa('.episode.selected', list).forEach(e => e.classList.remove('selected'));
        row.classList.add('selected');
        loadStreams(video);
      });
      list.appendChild(row);
    });
  }

  // ---- Sources --------------------------------------------------------------------------
  let sources = null;
  const SERVER_KINDS = new Set(['torrent', 'magnet', 'rar', 'zip', '7zip', 'tgz', 'tar', 'nzb']);
  const KIND_LABELS = {url: 'Direct', magnet: 'Torrent', torrent: 'Torrent', youtube: 'YouTube', rar: 'RAR', zip: 'ZIP',
                       '7zip': '7-Zip', tgz: 'TGZ', tar: 'TAR', nzb: 'Usenet', external: 'External', playerFrame: 'Web player'};

  function loadStreams(video, force) {
    if (!details || !details.meta) return;
    if (sources && sources.token) cancelToken(sources.token);
    const token = newToken('streams');
    groups.details.add(token);
    details.video = video;
    const meta = details.meta;
    const episodeLabel = video.season !== undefined && video.episode !== undefined
      ? 'S' + video.season + ' · E' + video.episode + (video.title ? ' — ' + video.title : '')
      : '';
    sources = {token, video, episodeLabel, slots: [], results: [], filter: null};
    const panel = q('.sources-panel');
    panel.hidden = false;
    q('.sources-title', panel).textContent = episodeLabel || meta.name || 'Streams';
    q('.sources-list', panel).innerHTML = '';
    q('.sources-filter', panel).innerHTML = '';
    q('.sources-list', panel).appendChild(el('div', 'sources-empty', 'Asking your add-ons…'));

    handlers.streamsPlan.set(token, plan => {
      if (!sources || sources.token !== token) return;
      sources.slots = plan.slots || [];
      sources.embedded = !!plan.embedded;
      sources.results = sources.slots.map(() => null);
      renderSources();
    });
    handlers.streamsResult.set(token, (index, result) => {
      if (!sources || sources.token !== token) return;
      sources.results[index] = result;
      renderSources();
    });
    bridge.loadStreams(token, details.type, video.id, details.token, !!force);
    if (window.matchMedia('(max-width: 1100px)').matches) panel.scrollIntoView({behavior: 'smooth', block: 'start'});
    refreshWindow();
  }

  function streamNeedsServer(stream) {
    return SERVER_KINDS.has(stream.kind) || (stream.kind === 'url' && /^ftps?:/i.test(stream.url || ''));
  }

  function streamCard(stream, slot) {
    const card = el('button', 'stream-card');
    card.type = 'button';
    const name = el('div', 'stream-name', stream.name || (slot.addon && slot.addon.name) || 'Stream');
    card.appendChild(name);
    const body = el('div', 'stream-body');
    if (stream.description) body.appendChild(el('div', 'stream-desc', stream.description));
    const badges = el('div', 'stream-badges');
    badges.appendChild(el('span', 'badge kind-' + stream.kind, KIND_LABELS[stream.kind] || stream.kind));
    const hints = stream.behaviorHints || {};
    if (hints.videoSize) badges.appendChild(el('span', 'badge', formatBytes(hints.videoSize)));
    if (hints.filename) badges.appendChild(el('span', 'badge filename', hints.filename));
    if (stream.subtitles && stream.subtitles.length) badges.appendChild(el('span', 'badge', stream.subtitles.length + ' subtitle' + (stream.subtitles.length > 1 ? 's' : '')));
    const needsServer = streamNeedsServer(stream);
    if (needsServer) {
      const server = state.server || {};
      if (server.mode === 'external') badges.appendChild(el('span', 'badge ' + (server.available ? 'server-ok' : 'server-missing'),
        server.available ? 'External server' : 'External server offline'));
      else if (server.engineMissing) badges.appendChild(el('span', 'badge server-missing', 'Engine missing'));
    }
    if (stream.kind === 'external' || stream.kind === 'playerFrame') badges.appendChild(el('span', 'badge', 'Opens in browser'));
    body.appendChild(badges);
    card.appendChild(body);
    card.addEventListener('click', () => playStream(stream, slot, card));
    return card;
  }

  function renderSources() {
    const panel = q('.sources-panel');
    const list = q('.sources-list', panel);
    const filterRow = q('.sources-filter', panel);
    list.innerHTML = '';
    filterRow.innerHTML = '';
    if (!sources.slots.length) {
      list.appendChild(el('div', 'sources-empty', 'None of your add-ons provides streams for this ' + typeSingular(details.type).toLowerCase() + '.'));
      return;
    }
    const readySlots = sources.slots.map((slot, i) => ({slot, result: sources.results[i], i}))
      .filter(x => x.result && x.result.status === 'ready' && (x.result.streams || []).length);
    if (readySlots.length > 1) {
      filterRow.appendChild(chip('All', sources.filter === null, () => { sources.filter = null; renderSources(); }));
      readySlots.forEach(x => filterRow.appendChild(chip((x.slot.addon && x.slot.addon.name) || 'Add-on', sources.filter === x.i, () => { sources.filter = x.i; renderSources(); })));
    }
    let total = 0;
    sources.slots.forEach((slot, i) => {
      if (sources.filter !== null && sources.filter !== i) return;
      const result = sources.results[i];
      const group = el('section', 'source-group');
      const head = el('div', 'source-head');
      const addon = slot.addon || {};
      head.appendChild(el('strong', null, addon.name || 'Add-on'));
      if (!result) {
        head.appendChild(el('span', 'source-state loading', 'Loading'));
      } else if (result.status === 'ready') {
        head.appendChild(el('span', 'source-state', (result.streams || []).length + ' stream' + ((result.streams || []).length === 1 ? '' : 's')));
      } else if (result.status === 'empty') {
        head.appendChild(el('span', 'source-state muted', 'No streams'));
      } else {
        head.appendChild(el('span', 'source-state error', result.error || 'Failed'));
        const retry = el('button', 'icon-btn', '↻');
        retry.type = 'button';
        retry.title = 'Retry';
        retry.addEventListener('click', () => loadStreams(sources.video, true));
        head.appendChild(retry);
      }
      group.appendChild(head);
      if (result && result.status === 'ready') {
        (result.streams || []).forEach(stream => { group.appendChild(streamCard(stream, slot)); total++; });
      }
      list.appendChild(group);
    });
    const allDone = sources.results.every(Boolean);
    if (allDone && total === 0 && sources.filter === null) {
      list.prepend(el('div', 'sources-empty', 'No streams found.'));
    }
    refreshWindow();
  }

  function playStream(stream, slot, card) {
    if (!details || !details.meta) return;
    const meta = details.meta;
    const video = sources.video || {};
    qa('.stream-card.busy').forEach(c => c.classList.remove('busy'));
    card.classList.add('busy');
    sources.busyCard = card;
    const context = {
      type: details.type,
      metaId: meta.id,
      videoId: video.id,
      streamTransportUrl: slot.request ? slot.request.base : '',
      metaTransportUrl: details.metaBase || '',
      title: meta.name || '',
      episodeLabel: sources.episodeLabel || '',
      addonName: (slot.addon && slot.addon.name) || '',
      poster: meta.poster || '',
      background: meta.background || '',
    };
    if (video.season !== undefined && video.episode !== undefined) {
      context.season = video.season;
      context.episode = video.episode;
    }
    bridge.play(stream, context);
  }

  function onPlayStatus(status) {
    const card = sources && sources.busyCard;
    if (status.state === 'resolving') {
      if (card) card.dataset.status = status.message || 'Opening…';
      return;
    }
    if (card) { card.classList.remove('busy'); delete card.dataset.status; }
    if (status.state === 'error') toast(status.message || 'This stream cannot be played.', true);
    else if (status.state === 'external') toast(status.message || 'Opened in your browser.');
  }

  // ---- Add-ons page -----------------------------------------------------------------------
  let installSeq = 0;

  function installRequest(callBridge, done) {
    const id = 'install:' + (++installSeq);
    handlers.install.set(id, result => { handlers.install.delete(id); done(result); });
    callBridge(id);
  }

  function describeOutcome(result) {
    return result.message || (result.status === 'installed' ? 'Installed.' : result.status);
  }

  function setInstallFeedback(result) {
    const box = q('.install-feedback');
    box.innerHTML = '';
    if (!result) return;
    const ok = ['installed', 'updated', 'alreadyInstalled', 'collectionImported'].includes(result.status);
    box.className = 'install-feedback ' + (ok ? 'ok' : result.status === 'configurationRequired' ? 'config' : 'error');
    box.appendChild(el('span', null, describeOutcome(result)));
    if (result.status === 'configurationRequired') {
      const b = el('button', 'pill-btn primary', 'Configure');
      b.type = 'button';
      b.addEventListener('click', () => bridge.configure(result.transportUrl));
      box.appendChild(b);
    }
  }

  function onInstallResult(id, result) {
    const handler = handlers.install.get(id);
    if (handler) { handler(result); return; }
    // Installs finished from a configure window.
    if (id === 'configure') {
      toast(describeOutcome(result), !['installed', 'updated', 'alreadyInstalled'].includes(result.status));
      setInstallFeedback(result);
    }
  }

  function installFromInput(url) {
    const input = q('.install-form input');
    const button = q('.install-form button');
    button.disabled = true;
    setInstallFeedback({status: 'working', message: 'Installing…'});
    q('.install-feedback').className = 'install-feedback';
    installRequest(id => bridge.install(url, id), result => {
      button.disabled = false;
      setInstallFeedback(result);
      if (['installed', 'updated', 'collectionImported'].includes(result.status)) {
        input.value = '';
        toast(describeOutcome(result));
      }
    });
  }

  function installDefaults(button) {
    if (button) button.disabled = true;
    installRequest(id => bridge.installDefaults(id), result => {
      if (button) button.disabled = false;
      toast(describeOutcome(result), result.status === 'failed');
      setInstallFeedback(result);
    });
  }

  const RESOURCE_LABELS = {catalog: 'Catalogs', meta: 'Details', stream: 'Streams', subtitles: 'Subtitles', addon_catalog: 'Add-on catalogs'};

  function addonCard(addon, index, count) {
    const card = el('article', 'addon-card glass' + (addon.enabled ? '' : ' disabled'));
    card.draggable = true;
    card.dataset.url = addon.transportUrl;
    const logo = el('div', 'addon-logo');
    logo.style.setProperty('--hue', hashHue(addon.name));
    if (addon.logo) logo.appendChild(img(addon.logo)); else logo.classList.add('no-image');
    logo.appendChild(el('span', null, initials(addon.name)));
    card.appendChild(logo);

    const body = el('div', 'addon-body');
    const title = el('div', 'addon-title');
    title.appendChild(el('strong', null, addon.name));
    title.appendChild(el('small', null, 'v' + addon.version + ' · ' + (addon.host || '')));
    body.appendChild(title);
    if (addon.description) body.appendChild(el('p', 'addon-desc', addon.description));

    const caps = el('div', 'addon-caps');
    const names = new Set((addon.resources || []).map(r => r.name));
    if ((addon.catalogs || []).length) names.add('catalog');
    if ((addon.addonCatalogs || []).length) names.add('addon_catalog');
    Array.from(names).forEach(name => {
      let label = RESOURCE_LABELS[name] || name;
      if (name === 'catalog') label += ' (' + (addon.catalogs || []).length + ')';
      caps.appendChild(el('span', 'cap cap-' + name, label));
    });
    (addon.types || []).forEach(t => caps.appendChild(el('span', 'cap type', typeLabel(t))));
    if (addon.idPrefixes && addon.idPrefixes.length) caps.appendChild(el('span', 'cap ids', 'IDs: ' + addon.idPrefixes.slice(0, 4).join(', ') + (addon.idPrefixes.length > 4 ? '…' : '')));
    const hints = addon.behaviorHints || {};
    if (hints.p2p) caps.appendChild(el('span', 'cap flag', 'P2P'));
    if (hints.adult) caps.appendChild(el('span', 'cap flag', 'Adult'));
    if (addon.official) caps.appendChild(el('span', 'cap flag', 'Official'));
    body.appendChild(caps);
    if (addon.lastError) body.appendChild(el('div', 'addon-error', 'Last refresh failed: ' + addon.lastError));
    card.appendChild(body);

    const controls = el('div', 'addon-controls');
    const toggle = el('label', 'switch');
    toggle.title = addon.enabled ? 'Disable this add-on' : 'Enable this add-on';
    const box = el('input');
    box.type = 'checkbox';
    box.checked = addon.enabled;
    box.addEventListener('change', () => bridge.setEnabled(addon.transportUrl, box.checked));
    toggle.append(box, el('i'));
    controls.appendChild(toggle);

    const order = el('div', 'order-btns');
    const up = el('button', 'icon-btn', '↑');
    up.type = 'button'; up.title = 'Move up (asked earlier)'; up.disabled = index === 0;
    up.addEventListener('click', () => bridge.move(addon.transportUrl, index - 1));
    const down = el('button', 'icon-btn', '↓');
    down.type = 'button'; down.title = 'Move down'; down.disabled = index === count - 1;
    down.addEventListener('click', () => bridge.move(addon.transportUrl, index + 1));
    order.append(up, down);
    controls.appendChild(order);

    const actions = el('div', 'addon-actions');
    if (addon.configureUrl) {
      const configure = el('button', 'pill-btn', 'Configure');
      configure.type = 'button';
      configure.addEventListener('click', () => bridge.configure(addon.transportUrl));
      actions.appendChild(configure);
    }
    const refresh = el('button', 'pill-btn', 'Refresh');
    refresh.type = 'button';
    refresh.title = 'Fetch the manifest again';
    refresh.addEventListener('click', () => bridge.refresh(addon.transportUrl));
    actions.appendChild(refresh);
    const copy = el('button', 'pill-btn', 'Copy URL');
    copy.type = 'button';
    copy.addEventListener('click', () => copyText(addon.transportUrl));
    actions.appendChild(copy);
    const remove = el('button', 'pill-btn danger', 'Remove');
    remove.type = 'button';
    remove.addEventListener('click', () => {
      if (remove.dataset.confirm) { bridge.remove(addon.transportUrl); return; }
      remove.dataset.confirm = '1';
      remove.textContent = 'Confirm remove';
      setTimeout(() => { remove.dataset.confirm = ''; remove.textContent = 'Remove'; }, 3500);
    });
    actions.appendChild(remove);
    controls.appendChild(actions);
    card.appendChild(controls);

    // Drag and drop reordering.
    card.addEventListener('dragstart', e => { e.dataTransfer.setData('text/plain', addon.transportUrl); card.classList.add('dragging'); });
    card.addEventListener('dragend', () => card.classList.remove('dragging'));
    card.addEventListener('dragover', e => { e.preventDefault(); card.classList.add('drop-target'); });
    card.addEventListener('dragleave', () => card.classList.remove('drop-target'));
    card.addEventListener('drop', e => {
      e.preventDefault();
      card.classList.remove('drop-target');
      const url = e.dataTransfer.getData('text/plain');
      if (url && url !== addon.transportUrl) bridge.move(url, index);
    });
    return card;
  }

  function copyText(text) {
    const done = () => toast('Copied to the clipboard.');
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(done, () => fallbackCopy(text, done));
    } else {
      fallbackCopy(text, done);
    }
  }

  function fallbackCopy(text, done) {
    const area = el('textarea');
    area.value = text;
    area.style.position = 'fixed';
    area.style.opacity = '0';
    document.body.appendChild(area);
    area.select();
    try { document.execCommand('copy'); done(); } catch (e) { toast('Could not copy.', true); }
    area.remove();
  }

  function renderAddons() {
    const list = q('.addon-list');
    list.innerHTML = '';
    if (!state.addons.length) {
      list.appendChild(el('div', 'list-empty', 'No add-ons installed yet.'));
    }
    state.addons.forEach((addon, index) => list.appendChild(addonCard(addon, index, state.addons.length)));
    renderServer();
    const hasDefaults = state.defaults.every(url => state.addons.some(a => a.transportUrl === url));
    qa('[data-install-defaults]').forEach(b => { b.hidden = hasDefaults; });
    refreshWindow();
  }

  function renderServer() {
    const card = q('.server-card');
    if (!card) return;
    const server = state.server || {};
    const external = server.mode === 'external';
    card.classList.toggle('online', !!server.available || (!external && !!server.running));
    card.classList.toggle('problem', external ? (server.checked && !server.available) : !!server.engineMissing);
    let title, detail;
    if (external) {
      title = server.available ? 'External streaming server connected' : 'External streaming server';
      detail = server.available ? 'Torrent and archive streams play through ' + server.url
        : 'Not reachable at ' + server.externalUrl + '. Start it, or clear the address below to use the built-in engine.';
    } else if (server.engineMissing) {
      title = 'Streaming engine missing';
      detail = 'lambda-stream-server.exe is not next to LAMBDA Player, so torrents cannot play. Re-extract the portable package.';
    } else {
      title = server.running ? 'Streaming engine running' : 'Streaming engine ready';
      detail = server.running ? 'Built-in engine on 127.0.0.1:' + server.port + '. Torrent, magnet and archive streams play here.'
        : 'Built in. It starts when you play a torrent, magnet or archive stream.';
      if (server.error) detail += ' Last problem: ' + server.error;
    }
    q('.server-title', card).textContent = title;
    q('.server-detail', card).textContent = detail;
    const cacheRow = q('.cache-row', card);
    cacheRow.hidden = external;
    q('.cache-detail', card).textContent = 'Cache: ' + (server.cacheDirectory || '') + ' · ' + (server.cacheUsage ? formatBytes(server.cacheUsage) + ' used' : 'empty');
    const select = q('.cache-size', card);
    if (server.cacheSize && document.activeElement !== select) {
      const value = String(server.cacheSize);
      if (![...select.options].some(o => o.value === value)) select.appendChild(new Option(formatBytes(server.cacheSize), value));
      select.value = value;
    }
    q('[data-cache-reset]', card).hidden = !server.customCacheLocation;
    const input = q('.server-form input', card);
    if (document.activeElement !== input) input.value = server.externalUrl || '';
    if (external) q('.server-external', card).open = true;
  }

  // Addon discovery: addon_catalog resources of the installed add-ons
  // (Cinemeta provides Stremio's "official" and "community" lists).
  const addonCatalogs = {rows: [], selected: 0, loaded: false};

  function loadAddonCatalogs(force) {
    if (addonCatalogs.loaded && !force) return;
    addonCatalogs.loaded = true;
    bridge.addonCatalogRows(rows => {
      addonCatalogs.rows = rows;
      const section = q('.addon-catalogs-section');
      section.hidden = !rows.length;
      if (!rows.length) return;
      if (addonCatalogs.selected >= rows.length) addonCatalogs.selected = 0;
      renderAddonCatalogTabs();
      loadAddonCatalog();
    });
  }

  function renderAddonCatalogTabs() {
    const tabs = q('.addon-catalog-tabs');
    tabs.innerHTML = '';
    addonCatalogs.rows.forEach((row, i) => tabs.appendChild(chip(row.name + (row.type && row.type !== 'all' ? ' · ' + typeLabel(row.type) : ''),
      i === addonCatalogs.selected, () => { addonCatalogs.selected = i; renderAddonCatalogTabs(); loadAddonCatalog(); })));
  }

  function loadAddonCatalog() {
    cancelGroup('addons');
    const row = addonCatalogs.rows[addonCatalogs.selected];
    const list = q('.addon-catalog-list');
    list.innerHTML = '';
    list.appendChild(el('div', 'list-empty', 'Loading…'));
    loadCatalog('addons', row.request, false, result => {
      list.innerHTML = '';
      if (result.status !== 'ready') {
        list.appendChild(el('div', 'list-empty', result.status === 'empty' ? 'This list is empty.' : 'Could not load: ' + (result.error || '')));
        return;
      }
      (result.addons || []).forEach(addon => list.appendChild(catalogAddonCard(addon)));
      refreshWindow();
    });
  }

  function catalogAddonCard(addon) {
    const card = el('article', 'addon-card compact glass');
    const logo = el('div', 'addon-logo');
    logo.style.setProperty('--hue', hashHue(addon.name));
    if (addon.logo) logo.appendChild(img(addon.logo)); else logo.classList.add('no-image');
    logo.appendChild(el('span', null, initials(addon.name)));
    card.appendChild(logo);
    const body = el('div', 'addon-body');
    const title = el('div', 'addon-title');
    title.appendChild(el('strong', null, addon.name));
    title.appendChild(el('small', null, 'v' + addon.version));
    body.appendChild(title);
    if (addon.description) body.appendChild(el('p', 'addon-desc', addon.description));
    const caps = el('div', 'addon-caps');
    (addon.types || []).slice(0, 6).forEach(t => caps.appendChild(el('span', 'cap type', typeLabel(t))));
    body.appendChild(caps);
    card.appendChild(body);
    const controls = el('div', 'addon-controls');
    const installed = state.addons.some(a => a.transportUrl === addon.transportUrl);
    const hints = addon.behaviorHints || {};
    if (installed) {
      controls.appendChild(el('span', 'installed-label', 'Installed'));
    } else if (hints.configurationRequired) {
      const b = el('button', 'pill-btn primary', 'Configure');
      b.type = 'button';
      b.addEventListener('click', () => bridge.configure(addon.transportUrl));
      controls.appendChild(b);
    } else {
      const b = el('button', 'pill-btn primary', 'Install');
      b.type = 'button';
      b.addEventListener('click', () => {
        b.disabled = true;
        installRequest(id => bridge.install(addon.transportUrl, id), result => {
          b.disabled = false;
          toast(describeOutcome(result), !['installed', 'updated', 'alreadyInstalled'].includes(result.status));
        });
      });
      controls.appendChild(b);
      if (hints.configurable && addon.configureUrl) {
        const c = el('button', 'pill-btn', 'Configure');
        c.type = 'button';
        c.addEventListener('click', () => bridge.configure(addon.transportUrl));
        controls.appendChild(c);
      }
    }
    card.appendChild(controls);
    return card;
  }

  // ---- State from C++ -----------------------------------------------------------------------
  function applyState(next, changed) {
    state.addons = next.addons || [];
    state.server = next.streamingServer || {};
    state.defaults = next.defaults || [];
    if (changed) {
      addonCatalogs.loaded = false;
      discover.initialized = false;
      if (state.view === 'discover') openDiscover(null);
    }
    planBoard();
    renderHero();
    if (state.view === 'addons') { renderAddons(); loadAddonCatalogs(false); }
  }

  function bindUi() {
    qa('[data-view-link]').forEach(a => a.addEventListener('click', e => {
      e.preventDefault();
      const view = a.dataset.viewLink;
      if (view === 'discover' && discover.initialized) { show('discover'); return; }
      show(view);
    }));
    qa('[data-back]').forEach(b => b.addEventListener('click', back));

    const searchForm = q('.nav-search');
    searchForm.addEventListener('submit', e => {
      e.preventDefault();
      const input = q('input', searchForm);
      runSearch(input.value);
      input.blur();
    });

    q('.primary-cta').addEventListener('click', e => {
      if (e.currentTarget.dataset.action !== 'featured' || !hero.meta) return;
      e.preventDefault();
      openDetails(hero.meta, {addon: hero.row && hero.row.addon});
    });
    q('[data-hero-secondary]').addEventListener('click', e => {
      e.preventDefault();
      const action = e.currentTarget.dataset.heroSecondary;
      if (action === 'resume') { const btn = q('.primary-cta'); btn.dataset.action = 'resume'; btn.click(); renderHero(); }
      else if (action === 'discover') show('discover');
      else show('addons');
    });

    qa('[data-install-defaults]').forEach(b => b.addEventListener('click', () => installDefaults(b)));
    q('.install-form').addEventListener('submit', e => {
      e.preventDefault();
      const url = q('.install-form input').value.trim();
      if (url) installFromInput(url);
    });
    q('[data-import-file]').addEventListener('click', () => installRequest(id => bridge.importCollectionFile(id), result => {
      if (result.status !== 'cancelled') { setInstallFeedback(result); toast(describeOutcome(result), result.status === 'failed'); }
    }));
    q('[data-import-paste]').addEventListener('click', () => { const box = q('.paste-box'); box.hidden = !box.hidden; if (!box.hidden) q('textarea', box).focus(); refreshWindow(); });
    q('[data-import-cancel]').addEventListener('click', () => { q('.paste-box').hidden = true; refreshWindow(); });
    q('[data-import-text]').addEventListener('click', () => {
      const text = q('.paste-box textarea').value;
      if (!text.trim()) return;
      installRequest(id => bridge.importCollectionText(text, id), result => {
        setInstallFeedback(result);
        toast(describeOutcome(result), result.status === 'failed');
        if (result.status !== 'failed') { q('.paste-box textarea').value = ''; q('.paste-box').hidden = true; }
      });
    });
    q('[data-export]').addEventListener('click', () => bridge.exportCollection());
    q('[data-refresh-all]').addEventListener('click', () => { bridge.refresh(''); toast('Refreshing all add-on manifests…'); });
    q('.server-form').addEventListener('submit', e => {
      e.preventDefault();
      bridge.setStreamingServerUrl(q('.server-form input').value.trim());
    });
    q('.cache-size').addEventListener('change', e => bridge.setCacheSize(Number(e.target.value)));
    q('[data-cache-folder]').addEventListener('click', () => bridge.chooseCacheLocation());
    q('[data-cache-reset]').addEventListener('click', () => bridge.resetCacheLocation());
    q('[data-cache-clear]').addEventListener('click', () => bridge.clearCache());
    q('.sources-refresh').addEventListener('click', () => { if (sources && sources.video) loadStreams(sources.video, true); });

    document.addEventListener('keydown', e => {
      if (e.key === 'Escape' && state.view === 'details' && !document.querySelector('.lambda-popover.open')) { back(); return; }
      if ((e.key === 'f' && (e.ctrlKey || e.metaKey)) || (e.key === '/' && !/INPUT|TEXTAREA|SELECT/.test(document.activeElement.tagName))) {
        e.preventDefault();
        q('.nav-search input').focus();
      }
    });
  }

  function attach(nextBridge) {
    bridge = nextBridge;
    bridge.catalogResult.connect((token, result) => { const f = handlers.catalog.get(token); if (f) f(result); });
    bridge.metaPlan.connect((token, slots) => { const f = handlers.metaPlan.get(token); if (f) f(slots); });
    bridge.metaResult.connect((token, index, result) => { const f = handlers.metaResult.get(token); if (f) f(index, result); });
    bridge.streamsPlan.connect((token, plan) => { const f = handlers.streamsPlan.get(token); if (f) f(plan); });
    bridge.streamsResult.connect((token, index, result) => { const f = handlers.streamsResult.get(token); if (f) f(index, result); });
    bridge.installResult.connect(onInstallResult);
    bridge.addonsChanged.connect(next => applyState(next, true));
    bridge.playStatus.connect(onPlayStatus);
    bridge.streamingServerChanged.connect(server => { state.server = server; renderServer(); if (sources) renderSources(); });
    bridge.notify.connect((message, warning) => toast(message, warning));
    bindUi();
    bridge.state(initial => {
      applyState(initial, false);
      bridge.checkStreamingServer();
    });
  }

  window.lambdaStremio = {attach, setLocalState, show};
})();
