(() => {
  'use strict';
  let bridge;
  let state = {palette: 'mint', fps: 120, smoothScroll: true, reduceMotion: false, torrentEncryption: true};
  const palettes = ['mint', 'ocean', 'violet', 'amber'];
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
        '<h3>Downloader+</h3><label class="preference-row">Encrypted torrent connections<input type="checkbox" data-preference="torrentEncryption"></label>' +
        '<p>Restart LAMBDA to apply. Turn off if a torrent cannot connect to peers. Downloads have no speed cap.</p><button type="button" class="pop-btn" data-preferences-folder>Choose download folder…</button>';
      pop.querySelector('.pop-head').after(panel);
      panel.querySelectorAll('[data-palette-choice]').forEach(button => button.onclick = () => set('palette', button.dataset.paletteChoice));
      panel.querySelectorAll('[data-preference]').forEach(input => input.onchange = () => set(input.dataset.preference,
        input.type === 'checkbox' ? input.checked : Number(input.value)));
      panel.querySelector('[data-preferences-folder]').onclick = () => window.lambdaDownloads?.chooseFolder();
      apply(state);
    }
  };
  apply(state);
})();
