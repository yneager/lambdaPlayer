(() => {
  'use strict';
  let bridge;
  let state = {palette: 'mint', fps: 120, smoothScroll: true, reduceMotion: false, torrentEncryption: true};
  const palettes = ['mint', 'ocean', 'violet', 'amber'];
  const labels = {open:'Open file',play:'Play / pause',fullscreen:'Fullscreen',mute:'Mute',seekBack:'Seek backward',seekForward:'Seek forward',volumeUp:'Volume up',volumeDown:'Volume down',next:'Next file',home:'Go to Home',interpolation:'Toggle interpolation',subtitleEarlier:'Subtitles earlier',subtitleLater:'Subtitles later',loadSubtitle:'Load subtitle file'};
  let shortcutBridge, bindings = {}, editor, capturing;
  function keyboardFocus() {
    const active = document.activeElement;
    shortcutBridge?.setKeyboardInputActive(!!document.querySelector('dialog[open]') || !!active?.matches('input,textarea,select,[contenteditable="true"]'));
  }
  function renderBindings() {
    document.querySelectorAll('[data-shortcut-hint]').forEach(hint => { hint.textContent = bindings[hint.dataset.shortcutHint] || 'Disabled'; });
    editor?.querySelectorAll('[data-shortcut]').forEach(button => {
      if (button.dataset.shortcut !== capturing) button.textContent = bindings[button.dataset.shortcut] || 'Disabled';
    });
  }
  function saveBinding(action, sequence) {
    shortcutBridge.setShortcut(action, sequence, result => {
      if (!editor) return;
      editor.querySelector('.shortcut-error').textContent = result.ok ? '' : result.error;
      if (result.ok) { bindings[action] = result.sequence; capturing = null; renderBindings(); }
    });
  }
  window.LambdaShortcuts = {
    hint(action) { return bindings[action] || ''; },
    attach(next) {
      if (shortcutBridge === next) return;
      shortcutBridge = next; bindings = {...next.shortcuts};
      next.shortcutsChanged.connect(() => { bindings = {...next.shortcuts}; renderBindings(); });
      document.addEventListener('focusin', keyboardFocus);
      document.addEventListener('focusout', () => queueMicrotask(keyboardFocus));
      keyboardFocus();
      renderBindings();
    },
    open(focusAction) {
      if (!shortcutBridge) return;
      if (editor) { editor.focus(); return; }
      editor = document.createElement('dialog'); editor.className = 'shortcut-dialog';
      editor.innerHTML = '<h2>Keyboard shortcuts</h2><p>Click a shortcut, then press your new key combination. Clear disables it. Escape cancels recording.</p><div class="shortcut-list"></div><p class="shortcut-error" role="status"></p><div class="shortcut-actions"><button type="button" data-reset>Reset defaults</button><button type="button" data-done>Done</button></div>';
      const list = editor.querySelector('.shortcut-list');
      Object.entries(labels).forEach(([action, label]) => {
        const row = document.createElement('div'); row.className = 'shortcut-row';
        const name = document.createElement('span'); name.textContent = label;
        const key = document.createElement('button'); key.type = 'button'; key.dataset.shortcut = action; key.setAttribute('aria-label', 'Change ' + label);
        key.onclick = () => { capturing = action; renderBindings(); key.textContent = 'Press keys…'; editor.querySelector('.shortcut-error').textContent = ''; };
        const clear = document.createElement('button'); clear.type = 'button'; clear.textContent = 'Clear'; clear.setAttribute('aria-label', 'Disable ' + label);
        clear.onclick = () => saveBinding(action, ''); row.append(name,key,clear); list.append(row);
      });
      editor.querySelector('[data-reset]').onclick = () => { capturing = null; shortcutBridge.resetShortcuts(); };
      editor.querySelector('[data-done]').onclick = () => editor.close();
      editor.addEventListener('keydown', event => {
        if (!capturing) return;
        event.preventDefault(); event.stopPropagation();
        if (event.key === 'Escape') { capturing = null; renderBindings(); return; }
        if (event.repeat || ['Control','Alt','Shift','Meta','Dead','Unidentified'].includes(event.key)) return;
        const keys = {' ':'Space',ArrowLeft:'Left',ArrowRight:'Right',ArrowUp:'Up',ArrowDown:'Down',PageUp:'PgUp',PageDown:'PgDown',Delete:'Del',Insert:'Ins'};
        const modifiers = [event.ctrlKey && 'Ctrl',event.altKey && 'Alt',event.shiftKey && 'Shift',event.metaKey && 'Meta'].filter(Boolean);
        saveBinding(capturing, [...modifiers, keys[event.key] || (event.key.length === 1 ? event.key.toUpperCase() : event.key)].join('+'));
      });
      editor.addEventListener('close', () => { editor.remove(); editor = null; capturing = null; keyboardFocus(); });
      document.body.append(editor); renderBindings(); editor.showModal(); keyboardFocus();
      if (focusAction) editor.querySelector(`[data-shortcut="${focusAction}"]`)?.focus();
    }
  };
  function apply(next) {
    state = {...state, ...next};
    document.documentElement.dataset.palette = palettes.includes(state.palette) ? state.palette : 'mint';
    document.documentElement.classList.toggle('reduce-motion', state.reduceMotion);
    document.querySelectorAll('[data-preference]').forEach(input => {
      const value = state[input.dataset.preference];
      if (input.type === 'checkbox') input.checked = !!value;
      else input.value = value;
    });
    document.querySelectorAll('[data-palette-choice]').forEach(button => {
      button.setAttribute('aria-pressed', String(button.dataset.paletteChoice === state.palette));
    });
  }
  function set(key, value) {
    apply({[key]: value});
    bridge?.setPreference(key, value);
  }
  window.LambdaPreferences = {
    get state() { return state; },
    attach(next) {
      bridge = next;
      window.LambdaShortcuts.attach(next);
      apply(bridge.preferences);
      bridge.preferencesChanged.connect(() => apply(bridge.preferences));
    },
    populate(pop) {
      const panel = document.createElement('section');
      panel.className = 'preference-panel';
      panel.innerHTML = '<h3>Appearance</h3><div class="palette-choices" role="group" aria-label="Color palette">' +
        palettes.map(name => `<button type="button" class="palette-choice" data-palette-choice="${name}" aria-pressed="false"><i></i>${name[0].toUpperCase() + name.slice(1)}</button>`).join('') +
        '</div><label class="preference-row">UI frame rate<select data-preference="fps"><option value="60">60 fps</option><option value="120">120 fps · balanced</option><option value="240">240 fps · high refresh</option></select></label>' +
        '<p>Limited to your monitor’s refresh rate. Higher rates use more CPU; video frame rate is unchanged.</p>' +
        '<label class="preference-row">Smooth wheel scrolling<input type="checkbox" data-preference="smoothScroll"></label>' +
        '<label class="preference-row">Reduce motion<input type="checkbox" data-preference="reduceMotion"></label>' +
        '<h3>Downloader+</h3><label class="preference-row">Prefer encrypted torrent connections<input type="checkbox" data-preference="torrentEncryption"></label>' +
        '<p>Restart LAMBDA to apply. Encryption is preferred; other peers can still connect. Downloads have no speed cap.</p><button type="button" class="pop-btn" data-preferences-folder>Choose download folder…</button>';
      pop.querySelector('.pop-head').after(panel);
      panel.querySelectorAll('[data-palette-choice]').forEach(button => button.onclick = () => set('palette', button.dataset.paletteChoice));
      panel.querySelectorAll('[data-preference]').forEach(input => input.onchange = () => set(input.dataset.preference,
        input.type === 'checkbox' ? input.checked : Number(input.value)));
      panel.querySelector('[data-preferences-folder]').onclick = () => window.lambdaDownloads?.chooseFolder();
      const shortcuts = document.createElement('button'); shortcuts.className = 'pop-btn'; shortcuts.textContent = 'Edit keyboard shortcuts…';
      shortcuts.onclick = () => window.LambdaShortcuts.open(); panel.append(shortcuts); renderBindings();
      apply(state);
    }
  };
  apply(state);
})();
