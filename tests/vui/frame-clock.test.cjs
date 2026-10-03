const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const {test} = require('node:test');
const source = fs.readFileSync(path.join(__dirname, '../../resources/vui/frame-clock.js'), 'utf8');

function fixture(cssAnimations = []) {
  let nativeCallback, frameCallback, now = 0, requests = 0;
  const listeners = {};
  const document = {hidden: false, documentElement: {}, getAnimations: () => cssAnimations, addEventListener(name, handler) { listeners[name] = handler; }};
  const window = {requestAnimationFrame(fn) { nativeCallback = fn; return 17; }, cancelAnimationFrame() { nativeCallback = null; }, addEventListener() {}};
  let mutate, observation;
  class MutationObserver { constructor(handler) { mutate = handler; } observe(target, options) { observation = options; } }
  class CSSAnimation {}
  class CSSTransition {}
  for (const animation of cssAnimations) Object.setPrototypeOf(animation, animation.kind === 'transition' ? CSSTransition.prototype : CSSAnimation.prototype);
  vm.runInNewContext(source, {window, document, performance: {now: () => now}, setTimeout, MutationObserver, CSSAnimation, CSSTransition,
    getComputedStyle(target) { return {animationName: target.name, animationPlayState: target.paused ? 'paused' : 'running'}; }});
  return {window, document,
    attach(enabled = true) { window.LambdaFrameClock.attach({uiFramePacing: enabled, uiFrame: {connect(fn) { frameCallback = fn; }}, requestUiFrame() { requests++; }}); },
    get requests() { return requests; },
    get observation() { return observation; },
    frame(timestamp) { now = timestamp; (frameCallback || nativeCallback)(); },
    visibility(hidden) { document.hidden = hidden; listeners.visibilitychange(); }
    , mutate() { mutate(); }
  };
}

test('batch callbacks, cancellation during dispatch and recursive next-frame requests', () => {
  const f = fixture(); f.attach();
  const results = [];
  let cancelled;
  f.window.requestAnimationFrame(function(now) {
    assert.equal(this, f.window); results.push(now);
    f.window.cancelAnimationFrame(cancelled);
    f.window.requestAnimationFrame(next => results.push(next));
  });
  cancelled = f.window.requestAnimationFrame(() => assert.fail('Cancelled callback ran'));
  assert.equal(f.requests, 1);
  f.frame(8.333); assert.deepEqual(results, [8.333]); assert.equal(f.requests, 2);
  f.frame(16.666); assert.deepEqual(results, [8.333, 16.666]); assert.equal(f.requests, 2);
});

test('CSS timelines retain elapsed time, pause offscreen and finish finite transitions', () => {
  function animation(kind, end) {
    return {kind, animationName: 'motion', effect: {target: {name: 'motion'}, getComputedTiming: () => ({endTime: end})},
      currentTime: 0, playbackRate: 1, playState: 'running', pause() { this.playState = 'paused'; }, finish() { this.currentTime = end; this.playState = 'finished'; }};
  }
  const motion = animation('animation', Infinity), transition = animation('transition', 100);
  const f = fixture([motion, transition]); f.attach(); f.frame(0); f.frame(40);
  assert.equal(motion.currentTime, 40); assert.equal(transition.currentTime, 40);
  motion.effect.target.paused = true; f.mutate(); f.frame(60); f.frame(100);
  assert.equal(motion.currentTime, 40); assert.equal(transition.playState, 'finished');
  const idleRequests = f.requests;
  motion.effect.target.paused = false; f.mutate(); f.frame(200); f.frame(220);
  assert.equal(motion.currentTime, 60); assert.ok(f.requests > idleRequests);
});

test('hidden pages retain callbacks and resume without an idle clock', () => {
  const f = fixture(); f.attach(); let calls = 0;
  f.frame(0); // Initial CSS scan.
  f.visibility(true); f.window.requestAnimationFrame(() => calls++);
  assert.equal(f.requests, 1);
  f.visibility(false); assert.equal(f.requests, 2);
  f.visibility(true); f.frame(8); assert.equal(calls, 0);
  f.visibility(false); f.frame(16); assert.equal(calls, 1);
  assert.equal(f.requests, 3);
});

test('native fallback when disabled and attachment preserves pending callbacks', () => {
  const f = fixture(); let calls = 0;
  f.window.requestAnimationFrame(() => calls++); f.attach(false); f.frame(16);
  assert.equal(calls, 1); assert.equal(f.requests, 0);
  f.window.requestAnimationFrame(() => calls++); f.attach(); assert.equal(f.requests, 1);
  f.frame(24); assert.equal(calls, 2);
});

test('inline progress and scroll styles do not trigger global animation scans', () => {
  const f = fixture(); f.attach();
  assert.deepEqual(Array.from(f.observation.attributeFilter), ['class', 'hidden']);
  assert.equal(f.observation.childList, true);
});
