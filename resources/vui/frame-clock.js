// Qt WebEngine 6.8 has a separate 60 Hz offscreen compositor clock. The native
// bridge paces local animation callbacks to the window's current monitor.
// No timer runs when there are no pending callbacks or active CSS animations.
(() => {
  'use strict';
  const nativeRequest = window.requestAnimationFrame.bind(window);
  const nativeCancel = window.cancelAnimationFrame.bind(window);
  let bridge = null, scheduled = false, nativeHandle = 0, sequence = 0;
  const callbacks = new Map();
  const animations = new Map();
  let scanNeeded = false;

  function scanAnimations(now) {
    if (!scanNeeded) return;
    scanNeeded = false;
    if (!document.getAnimations) return;
    const current = new Set(document.getAnimations());
    for (const animation of animations.keys()) {
      if (!current.has(animation) || animation.playState === 'idle' || animation.playState === 'finished') animations.delete(animation);
    }
    for (const animation of current) {
      // Only CSS animations/transitions owned by these local pages are paced.
      // Script-created Web Animations keep their original API lifecycle.
      if (!(animation instanceof CSSAnimation || animation instanceof CSSTransition)) continue;
      if (animation.playState === 'idle' || animation.playState === 'finished') continue;
      let stylePaused = false;
      if (animation instanceof CSSAnimation) {
        const style = getComputedStyle(animation.effect.target);
        const names = style.animationName.split(',').map(name => name.trim());
        const states = style.animationPlayState.split(',').map(state => state.trim());
        const index = Math.max(0, names.indexOf(animation.animationName));
        stylePaused = states[index % states.length] === 'paused';
      }
      const existing = animations.get(animation);
      if (stylePaused) {
        if (existing) existing.running = false;
        continue;
      }
      if (existing) {
        if (!existing.running) { existing.running = true; existing.last = now; }
        // CSS state changes can resume an animation paused through the API.
        if (animation.playState === 'running') animation.pause();
      } else if (animation.playState === 'running') {
        animations.set(animation, {time: Number(animation.currentTime) || 0, last: now, rate: animation.playbackRate, running: true});
        animation.pause();
      }
    }
  }
  function advanceAnimations(now) {
    for (const [animation, state] of animations) {
      if (animation.playState === 'idle' || animation.playState === 'finished') { animations.delete(animation); continue; }
      if (!state.running) continue;
      state.time += (now - state.last) * state.rate;
      state.last = now;
      const end = animation.effect.getComputedTiming().endTime;
      if (state.rate > 0 && Number.isFinite(end) && state.time >= end) {
        animation.finish();
        animations.delete(animation);
      } else animation.currentTime = state.time;
    }
  }

  function schedule() {
    const motion = Array.from(animations.values()).some(state => state.running);
    if (scheduled || (!callbacks.size && !motion && !scanNeeded) || document.hidden) return;
    scheduled = true;
    if (bridge) bridge.requestUiFrame();
    else nativeHandle = nativeRequest(flush);
  }
  function flush() {
    scheduled = false;
    nativeHandle = 0;
    if (document.hidden) return;
    // A callback requested inside this batch belongs to the next frame.
    const batch = Array.from(callbacks.keys());
    const now = performance.now();
    scanAnimations(now);
    advanceAnimations(now);
    for (const id of batch) {
      const callback = callbacks.get(id);
      if (!callback) continue;
      callbacks.delete(id);
      try { callback.call(window, now); }
      catch (error) { setTimeout(() => { throw error; }, 0); }
    }
    schedule();
  }
  window.requestAnimationFrame = callback => {
    if (typeof callback !== 'function') throw new TypeError('Animation callback must be a function');
    const id = ++sequence;
    callbacks.set(id, callback);
    schedule();
    return id;
  };
  window.cancelAnimationFrame = id => { callbacks.delete(id); };
  document.addEventListener('visibilitychange', () => {
    if (document.hidden && nativeHandle) { nativeCancel(nativeHandle); nativeHandle = 0; scheduled = false; }
    if (!document.hidden) { scheduled = false; schedule(); }
  });
  window.LambdaFrameClock = {
    attach(next) {
      if (bridge || !next?.uiFramePacing || !next?.uiFrame || !next?.requestUiFrame) return;
      bridge = next;
      bridge.uiFrame.connect(flush);
      if (nativeHandle) nativeCancel(nativeHandle);
      nativeHandle = 0;
      scheduled = false;
      const rescan = () => { scanNeeded = true; schedule(); };
      // Preserve CSS easing, durations and keyframes while advancing their
      // timelines on the same bounded clock as scrolling and JS transitions.
      // Inline style updates from scrolling/progress must not trigger a full
      // animation/layout scan on every frame. CSS lifecycle events cover them.
      new MutationObserver(rescan).observe(document.documentElement, {subtree: true, childList: true, attributes: true, attributeFilter: ['class', 'hidden']});
      for (const event of ['animationstart', 'animationcancel', 'transitionrun', 'transitioncancel']) document.addEventListener(event, rescan, true);
      window.addEventListener('resize', rescan);
      scanNeeded = true;
      schedule();
    }
  };
})();
