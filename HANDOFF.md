# AI Agent Handoff

## AMD FRC post-render frame generation (2026-09-25, not committed)

- Code: `src/frc/` — `AmfFrcInterpolator` (AMF/FRC, SimpleFRC loop), `WglDxInterop` (GL<->D3D11 textures), `ScreenInterpolationController` (worker thread, display ring, presentation timer, audio-delay compensation, stats, watchdog). Hooks in `MpvVideoWidget::requestFrame/paintGL`; menu entry index 3 in `MainWindow` (`kFrcModeIndex`). Design, measurements and why Windows.Graphics.Capture was not used: `docs/frc/RESEARCH.md`.
- Diagnostics: `LAMBDA_FRC_LOG=<file>` (JSON line per second incl. GUI latency probe), `LAMBDA_DEBUG_GRAB` JSON has an `frc` object. Matrix switches: `LAMBDA_FRC_PROFILE` (1 HIGH, 2 SUPER), `LAMBDA_FRC_SEARCH` (0 native, 1 performance), `LAMBDA_FRC_FUTURE`, `LAMBDA_FRC_FALLBACK`; `LAMBDA_FRC_INPUT=source` runs FRC at the decoded resolution. Test scripts (not committed): `_local/tools/frctest.ps1`, `_local/tools/frclifecycle.ps1`.
- `main.cpp` now uses swap interval 0 (see CHANGELOG); check fullscreen for tearing on other machines.
- 60/120/200 fps: menu entries 4-6 (`kFrcTargets`), cascade in `AmfFrcInterpolator` (kMaxStages 4), tick presenter in `ScreenInterpolationController::onOutput/presentDue`. Player controls: Speed / Audio / Subtitles / Smoothness buttons (player.html/js), no Settings button; Fit/Fill under "...". Fullscreen DWM frame: `MainWindow::applyFullscreenFrame`.
- Not done: real Comet film content / subjective artifact review, HDR output, non-AMD machines (the option is disabled with the probe's reason).

## Local work awaiting owner review (2026-09-24)

Not committed or pushed. Built app: `_local/app/LambdaPlayer.exe`.

- Player now fills the native window; removed the inset rounded card and 200vmax masking shadow. DWM still owns outer window corners (preference 2/ROUND verified). Fullscreen assigns the native video the root rectangle immediately and rejects stale browser rectangles; CSS padding no longer animates window-state changes. Browser geometry refreshes on window class changes as well as resize.
- mpv render callbacks are coalesced and checked with `mpv_render_context_update`. GUI-thread painting does not wait for the target timestamp; paired `video-timing-offset=0` avoids introducing the offset described in the bundled `mpv/render.h` documentation. Do not enable advanced render control without moving synchronous mpv commands off the rendering thread.
- Player state pushes are coalesced/deduplicated; timeline-only updates skip the rest of the DOM. Geometry reads are deferred until after writes. Home removes expensive backdrop blur, static backgrounds replace continuously repainted decorative effects, offscreen hero animations pause, wheel targets are clamped as page height changes.
- RIFE uses 4 concurrent requests / 4 buffered frames and up to 8 CPU workers, leaving scheduling headroom. GPU selection and the plugin's default GPU concurrency remain unchanged. Source colour metadata travels with each frame instead of shared mutable state, and R80 `_Range` semantics are handled correctly. Interpolation selection before media/render readiness is rejected (this previously hung the UI).
- Added `tests/rife/color_runtime.py`, run using `_local/app/vapoursynth/python.exe`. It checks real plugin output with concurrent requests, limited/full range, BT.709/BT.601, luma preservation and output timing.
- `LAMBDA_DEBUG_GRAB` now writes diagnostic JSON with each request. Use `name-stats.request` for JSON only: GL screenshots stall presentation and invalidate frame-drop measurements. Diagnostics contain geometry, corner preference, fps, hwdec and drop counts, never stream URLs/headers.

Validation: Release build and all 8 protocol suites passed. Home scroll measurement (180 rAF frames, same local page/hardware): blur enabled median 33.3 ms / p95 50.1 ms; final build median 16.7 ms / p95 16.7 ms / max 16.8 ms. This is a local measurement, not a universal performance guarantee. HTTP add-on test through `AddonsBridge::play` with a local 640x360/30 fps clip: fullscreen video/root/window all 2560x1440; restoration to 1360x840 and maximize/fullscreen/restore checked. In-process libmpv RIFE60 produced 60.0006 fps with zero decoder/presentation drops at the sampled checkpoints before and after seeking. Original restored to 30 fps. Both colour runtime cases passed. No-media interpolation guard remained responsive.

Limits: actual user Comet URLs and 1080p/4K RIFE throughput were not exercised; the local clip has no audio, so A/V sync requires an audible clip check. Streaming-server sources/pins were not changed or rebuilt. Its existing executable is included in the local deployed app.

Scheduling reference: https://mpv.io/manual/stable/#video-filters-vapoursynth (auto concurrency is logical CPU count; larger buffers alone do not improve speed).

## Last Updated
- Date: 2026-09-24
- Model/Agent: Claude (claude-opus-5-5)
- Branch: `main` (v0.2.5 released)
- CI: the Windows workflow now builds and runs the `tests/stremio` suites (`ctest`).

## Project Summary
LAMBDA Player is a small Windows desktop video player implemented in C++20 with Qt 6 Widgets and libmpv. Qt owns the window; the Home and Player UIs are Vui HTML/CSS pages in Qt WebEngine. libmpv renders through its render API into `MpvVideoWidget` (a `QOpenGLWidget`, since v0.2.2) and does decoding, timing, audio and subtitles. Since v0.2.5 it is also a Stremio add-on client.

The repository targets a portable Windows x64 build produced by the `Build Windows Portable` GitHub Actions workflow (artifact `LAMBDA-Player-Windows-x64`).

## Latest Change: v0.2.5 Stremio Add-on Client

Read `docs/stremio/SOURCE_MAP.md` first: every protocol behaviour is a port of a named Stremio (or proven client) implementation. `docs/stremio/COMPATIBILITY.md` records every deviation and finding (C-001…C-018).

Layers:
- `src/stremio/` (`lambda_stremio`, no GUI): `manifest`, `resources`, `capabilities`, `transport`, `legacytransport`, `addonurl` (pure protocol), `addonclient` (network), `addonmanager` (installed add-ons, `addons.json`), `contentservice` (request planning + JSON for the UI), `streamresolver` (+ `StreamingServer`), `videoparams` (subtitle context), `lzstring`, `language`.
- `src/stremiobackend.*` owns them; `src/addonsbridge.*` is the Home page's QWebChannel object `stremio`; `src/addonconfigurewindow.*` shows add-on configure pages in an off-the-record profile.
- `resources/vui/stremio.js` + `addons.css`: Home rows/hero, Discover, Search, Details, Sources, Add-ons. `home.js` keeps local recents/About and hands the hero to `stremio.js`.
- `MainWindow::openStream` plays resolved streams in the same libmpv pipeline; `fetchAddonSubtitles` asks subtitle add-ons after FILE_LOADED; add-on subtitles are combo entries with data `"addon:<n>"` loaded by `sub-add` on selection.

Testing:
- `build.ps1 -Test` (local) / CI `ctest`: 8 Qt Test suites with `tests/stremio/mockaddonserver.h`.
- Runtime harness used for v0.2.5 (not committed, lives in `_local/tools`): `run-test.ps1` (isolated `LAMBDA_DATA_DIR`, `QTWEBENGINE_REMOTE_DEBUGGING=127.0.0.1:9223`, `LAMBDA_DEBUG_GRAB`, `LAMBDA_MPV_LOG`), `cdp.ps1` (evaluate JS in `index.html`/`player.html`), `grab.ps1` (window capture) and `testaddon.py` (deterministic local add-on with catalogs, a series with canonical ids, direct/header-protected streams, inline and add-on subtitles, a configure page).
- Built-in streaming engine: `tools/stream-server/` (pins in `versions.json`, host, triplet, patches, `build.ps1`); `src/stremio/serverprocess.*` runs it; `StremioBackend` owns cache location/size/clear. Local build: `_local/tools/build-streamserver.ps1` (Rust + vcpkg + libclang in `_local`), `_local/tools/enginetest.ps1` tests the exe directly.
- YouTube is intentionally not played in LAMBDA (owner decision); YouTube streams open in the browser.

## Previous Change: v0.2.4 Animated Wheel Scrolling

`resources/vui/home.js` → `smoothWheel()` eases the Home page toward the accumulated wheel target.
- Qt WebEngine forwards Windows wheel notches to Chromium as precise pixel deltas, so `QWebEngineSettings::ScrollAnimatorEnabled` (set in `homepage.cpp`) never animates them.
- Only notch input (`wheelDeltaY` a multiple of 120, or line/page `deltaMode`) is intercepted. Touchpads, ctrl+wheel, inner vertical scrollers and `prefers-reduced-motion` stay native.
- Any outside scroll (scrollbar drag, keys, nav `scrollTo`) cancels the animation. Per-frame `scrollTo` uses `behavior: 'instant'` to bypass the CSS `scroll-behavior: smooth`.

## Previous Change: v0.2.3 Smoother Scrolling

`main.cpp` adds `--enable-gpu-rasterization` to `QTWEBENGINE_CHROMIUM_FLAGS` before `QApplication`.
- On the desktop-OpenGL Qt Quick/WebEngine path (required since v0.2.2), Chromium otherwise rasterizes the Vui pages on the CPU, which made scrolling janky.
- Measured frame stalls fell from ~480 ms to 16.8 ms; see CHANGELOG.
- Check Chromium's GPU feature status with DevTools `SystemInfo.getInfo` (`QTWEBENGINE_REMOTE_DEBUGGING=127.0.0.1:9222`). `rasterization` should read `enabled_force`.
- If a GPU driver misbehaves, users can set `QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu-rasterization`.

## Earlier Change: v0.2.2 Working Video + Frameless Polished UI

**Video rendering architecture changed. Read this first.**
- `video_` is now `MpvVideoWidget` (`QOpenGLWidget`) using libmpv's render API (`vo=libmpv`). There is no `wid` and no native child HWND.
  - The native-HWND approach cannot work under the transparent WebEngine chrome on Windows.
  - Do not add `WA_NativeWindow` to any widget in the main window: it breaks Qt's compositing of the GL video and the web layers.
- `PlayerChrome`'s internal `QQuickWidget` has `WA_AlwaysStackOnTop`, so the chrome is blended over the video.
- `main.cpp` must keep `AA_ShareOpenGLContexts` and `QQuickWindow::setGraphicsApi(OpenGL)` before `QApplication`.
- The render context must be freed (`video_->shutdown()`) before `mpv_terminate_destroy()`.
- The first `loadfile` is deferred (`pendingPath_`) until `renderReady`.
- The `mpvWakeup` connection must stay **before** `initMpv()`, otherwise the first wakeup is lost and no mpv event is ever processed. That was the "opening a video doesn't work" bug.
- The window is frameless. `MainWindow::nativeEvent` handles `WM_NCCALCSIZE`, `WM_NCHITTEST` and `WM_NCACTIVATE`, and `applyNativeFrame()` re-adds the frame styles and DWM attributes.
  - Drag regions come from each page's `[data-drag]` elements via `windowBridge.setDragRegions`.
  - Use `fullscreenMode_` and `isWindowMaximized()`, not Qt's `isFullScreen()` or `isMaximized()`.
  - Maximize goes through Win32 `ShowWindow`.
- Resume and recent files use mpv watch-later files (`watch-later-options=start,aid,sid`) plus `recent/items` in the QSettings INI.
- Testing aids:
  - `LAMBDA_DEBUG_GRAB=<dir>` with `<name>.request` files saves an in-app composited capture. Desktop capture cannot read GL content on some drivers.
  - `QTWEBENGINE_REMOTE_DEBUGGING=127.0.0.1:9222` exposes the pages to DevTools.
  - JS console errors are logged via `qWarning`.
- Verified locally (see CHANGELOG). The user still needs to confirm the build on their machine, especially the mini player and RIFE.

## Previous Change: v0.2.1 LAMBDA Player / Complete Vui CSS

The active project line is reset to **v0.2.1** and the product is now **LAMBDA Player**.

### Application architecture
- `MainWindow` keeps a `QStackedWidget`: Home and Player share one persistent libmpv instance.
- Home is the real current Vui `index.html` + `home.css` rendered from qrc through Qt WebEngine.
- Player is the real current Vui `player.html` + `styles.css` rendered from qrc through a transparent Qt WebEngine chrome.
- `PlayerChrome` / `player.js` use Qt WebChannel to route CSS controls to the existing C++ backend.
- `video_` remains a native Qt widget and the libmpv `wid` target. The WebEngine player reports the responsive CSS player rectangle; Qt moves/masks the native video widget to that rectangle and keeps the CSS chrome above it.
- The legacy native Qt player controls remain hidden state containers for now. Do not make them visible again unless the CSS player is deliberately removed.
- Home/Player navigation does not recreate mpv. Returning Home pauses; Resume returns to the current loaded file.
- The local resume URL is `lambda://resume`.

### Real player mappings
- Vui brand chip → Home.
- Top upload/open icon → local Open Video.
- More / Audio / Captions / Settings → CSS settings panel backed by real track/interpolation state.
- Player-mode, center play and bottom Play → play/pause.
- Next → `playlist-next weak`.
- Timeline → existing exact seek path using one `absolute+exact` flag.
- Volume/mute, speed, audio tracks, subtitle tracks, external subtitle loading, chapters and fullscreen remain live.
- Interpolation settings use the existing `InterpolationController`; RIFE modes and fallback behavior are unchanged.
- PiP remains disabled because there is still no real PiP backend.

### Packaging / identity
- CMake project/target: `LambdaPlayer`.
- Version: **0.2.1**.
- Executable: `LambdaPlayer.exe`.
- CI artifact: `LAMBDA-Player-Windows-x64`.
- WebEngine dependencies: Widgets, WebEngineWidgets, WebChannel; CI installs `qtwebengine qtwebchannel qtpositioning`.
- Inter stays pinned and bundled locally; Google Fonts are not used at runtime.

### Verification requirement
- CI must compile, deploy WebEngine resources, assemble RIFE and pass the packaged-app startup smoke test.
- CI success is not visual/runtime proof. Windows testing must specifically verify that the transparent WebEngine player chrome stacks correctly above the native mpv HWND, video remains visible through the media area, controls receive input, Home ↔ Player works, seeking/tracks/fullscreen work, and RIFE still activates.

## Previous Change: README Version History + Frame-Rate Labels / 60 fps Mode
The user confirmed the current RIFE integration and interpolation modes are working correctly in `LambdaPlayer.exe` after the activation fix below.

The selector now has three entries, labelled with the video's real rates:
- index 0: "Original (N fps)", which is Off;
- index 1: "2N fps (RIFE)", which doubles the frame rate;
- index 2: "60 fps (RIFE)", which targets 60 fps.

The user asked not to use the name "2×". Labels are refreshed on `MPV_EVENT_FILE_LOADED` from mpv's `container-fps`, using `InterpolationController::normalizedFps()`. The 60 fps entry is disabled for sources at 59 fps or above, and an active 60 fps mode turns off with a message on such files.

`rife.vpy` receives `user-data = "<double|60>|<runtime dir>"`. In 60 mode it uses `factor = 60 / snapped container_fps`, because mpv's `video_in` carries no fps and the plugin's `fps_num` option would throw. Verified with the pinned `mpv.exe`: 24, 25 and 30 fps each became 60 fps, and 60 fps sources were refused cleanly. The C++ UI is build-verified by CI only.

## Earlier Fix: RIFE Activation (after user report)
The first RIFE build (PR #1) was built by CI. When the user selected RIFE 2× it failed with "mpv could not create the VapourSynth filter: error running command".

Root cause: mpv on Windows caches the environment on its first `getenv()` call (`osdep/io.c`, `init_getenv`, run once). LAMBDA Player set `VSSCRIPT_PATH` only when RIFE was selected, long after `mpv_create()`, so mpv never saw it and `dlopen("VSScript.dll")` failed. The earlier local tests used `mpv.exe` with the variable set before launch, which is why they did not catch it.

Fix and verification: `InterpolationController::configureProcessEnvironment()` is now called in `MainWindow::initMpv()` before `mpv_create()`. `setMode()` additionally preloads `vsscript.dll` by full path with `LoadLibraryExW`. A ctypes harness driving the pinned `libmpv-2.dll` in-process confirmed three results:
- the old order reproduces the user's error;
- setting the variable early gives 47.9996 fps;
- preloading only gives 47.9996 fps.

This must still be confirmed in `LambdaPlayer.exe` by the user.

## Runtime Verification Of The Base Player (user-confirmed)
The user manually tested the build of commit `3314ef13` (Actions run #10) and confirmed these work correctly:

- video playback
- MKV seeking (progress-bar click, progress-bar drag, Left/Right ±5 s)
- embedded subtitles (selection and visible rendering)
- external subtitles
- multiple audio tracks
- fullscreen overlay controls (slide-up animation, auto-hide)

These are the "do not break" baseline for all further work.

## Current State
Everything from the base player above, plus:

- **Frame Interpolation** combo box in the track row of the control bar: Original (default, Off), double frame rate (RIFE), and 60 fps (RIFE).
- RIFE modes = mpv's built-in `vapoursynth` video filter running `rife/rife.vpy`, which calls the existing VapourSynth-RIFE-ncnn-Vulkan plugin (RIFE v4.6, ncnn/Vulkan). LAMBDA Player contains no decoding, rendering or inference code.
- Portable runtime (embedded Python + VapourSynth + plugin + model) is assembled by CI from pinned, SHA-256-verified downloads.

### Pipeline
`media file → libmpv decode → mpv vapoursynth filter (@novarife) → VapourSynth R80 → misc.SCDetect → YUV→RGBS → rife.RIFE(factor 2/1, sc=True, v4.6) → RGBS→source YUV format → libmpv rendering (subtitles/OSD drawn afterwards by mpv) → LAMBDA Player video widget`

### How the filter is added/removed
`InterpolationController` (`src/interpolationcontroller.{h,cpp}`):
1. Checks that `rife/rife.vpy`, `rife/librife_windows_x86-64.dll`, `rife/MiscFilters.dll`, `rife/models/rife-v4.6_ensembleFalse/flownet.{bin,param}`, `vapoursynth/python.exe`, `vapoursynth/python3.dll` and `vapoursynth/Lib/site-packages/vapoursynth/vsscript.dll` exist next to `LambdaPlayer.exe`.
2. `VSSCRIPT_PATH` is set to the bundled `vsscript.dll` at startup, **before `mpv_create()`**, because mpv caches the environment. When RIFE is selected, `vsscript.dll` is also preloaded by full path.
3. Runs `vf add @novarife:vapoursynth=file=%N%<rife.vpy>:user-data=%N%<rife dir>` (mpv `%len%` quoting because Windows paths contain `:`).
4. Off runs `vf remove @novarife` — only that label; unrelated filters are never cleared.

Failure handling:
- Missing files → error dialog, combo reverts to Off, no filter added.
- `vf add` fails synchronously (e.g. VSScript cannot load) → mpv does not add the filter; error dialog, Off.
- Script/plugin fails when video reaches it (missing model, no Vulkan GPU, Python exception) → mpv logs `Disabling filter novarife because it has failed.` and passes video through unfiltered. LAMBDA Player requests error-level mpv log messages, detects that line, removes `@novarife`, reverts the combo to Off and shows the Python exception text.

## Verification Status

### Verified: libmpv VapourSynth support (dependency audit)
- The workflow previously downloaded `shinchiro/mpv-winbuild-cmake` **latest**. It is now pinned to release `20260923`, asset `mpv-dev-x86_64-20260923-git-6fd80b2003.7z`, SHA-256 `372F29C292D0C8B4CE916225739E5872E35B8E11F3F4590C285BAED8BA551100`.
- `libmpv-2.dll` from that exact archive was inspected: it contains the build configuration string `-Dvapoursynth=enabled`, the feature list includes `vapoursynth`, and it contains the filter strings `VapourSynth bridge`, `VSScript.dll`, `getVSScriptAPI`, `buffered-frames`, `video_in`, `container_fps`. No dependency change was needed.
- The same release's `mpv.exe` (`mpv-x86_64-20260923-git-6fd80b2003.7z`, SHA-256 `A2FC7178EE5D49869B7719E907ED402D6191E27167D4631FF9AA2592910632AA`, mpv v0.41.0-1055-g6fd80b200) is built from the same source and lists `vapoursynth  VapourSynth bridge` in `--vf=help`. It was used as the runtime test vehicle below.
- mpv source (`video/filter/vf_vapoursynth.c`): on Windows it `dlopen`s `$VSSCRIPT_PATH`, else `VSScript.dll`; the script is reloaded on every seek; output timing comes from `_DurationNum/_DurationDen`, which the RIFE plugin halves for factor 2.

### Runtime-verified locally (mpv.exe from the pinned build + the exact pinned runtime, Windows 11, AMD Radeon RX 9070 XT, driver 32.0.31041.1004)
Driven over mpv's JSON IPC with the same `vf add/remove @novarife` commands LAMBDA Player issues, and the runtime laid out exactly as the CI script assembles it:

| Check | Result |
|---|---|
| 24 fps H.264 720p, RIFE 2× | `estimated-vf-fps` 24.02 → **47.9996** |
| 30 fps H.264 720p, RIFE 2× | 30.03 → **60.0006** |
| Generated frames | screenshot shows the on-frame counter blended between two source frames (true synthesized frame, not duplication / display interpolation) |
| `hwdec=auto-safe` (d3d11va) with RIFE | works: mpv's autoconvert inserts `HW-downloading from d3d11` + `nv12 -> yuv420p` automatically; `hwdec-current` stays `d3d11va` → **LAMBDA Player keeps `auto-safe`, no hwdec change needed** |
| `hwdec=no` with RIFE | works |
| Seek (absolute+exact) while active | works; script reloads on seek as documented, stays at 48 fps |
| Pause / resume while active | works |
| `vf remove @novarife` | back to 24 fps and `d3d11` direct HW output (normal path restored) |
| Remove then re-add | works |
| External SRT subtitle while active | rendered crisply on top of the interpolated video (subtitles are not passed through RIFE) |
| VSScript not loadable | `vf add` returns error, filter not added, playback continues |
| Script error (plugin path) / missing model | mpv logs Python exception, disables `novarife`, playback continues unfiltered |
| `resources/rife/rife.vpy` | compiles with the bundled Python 3.12.10 |
| `.github/scripts/assemble-rife-runtime.ps1` | ran locally: all 6 downloads SHA-256 verified, layout correct, runtime ≈ 51 MB |

### Build verification
- Pushed with this commit; the `Build Windows Portable` run for it must be checked. LAMBDA Player's C++ changes were **not** compiled locally (no Qt/MSVC on the agent machine). If CI fails, fix it before anything else.

### User runtime verification
- The user reported the current LAMBDA Player build, including the RIFE integration and current interpolation modes, is working perfectly on their test system.
- Earlier user verification already covered MKV seeking, embedded/external subtitles, multiple audio tracks and fullscreen overlay behavior.

### Still not broadly verified
- NVIDIA and Intel GPUs; low-end GPUs / broader 1080p–4K real-time performance (the documented local interpolation harness test was on an AMD RX 9070 XT).
- Running on a clean PC without the VC++ redistributable, although the workflow bundles the MSVC runtime DLLs app-locally.
- HDR / 10-bit content: the script converts using the source matrix and returns the source pixel format, but no transfer-function handling is done in the RGB step; HDR (PQ/HLG) through RIFE is **unverified and not claimed as supported**. Selecting Original gives normal HDR playback.

## Pinned Third-Party Versions
| Component | Pin | SHA-256 |
|---|---|---|
| libmpv (shinchiro) | `20260923` / `mpv-dev-x86_64-20260923-git-6fd80b2003.7z` | `372F29C292D0C8B4CE916225739E5872E35B8E11F3F4590C285BAED8BA551100` |
| Python embeddable | 3.12.10 | `4ACBED6DD1C744B0376E3B1CF57CE906F9DC9E95E68824584C8099A63025A3C3` |
| VapourSynth | R80 `VapourSynth64-Portable-R80.zip` | `5D927152D9DB29D104C8960BF44D0DF7777835F7741D310184BFAE6FDA2F4F22` |
| VapourSynth-RIFE-ncnn-Vulkan | `r9_mod_v33` (commit `c3ec6aabc07c8fa37a4f58d7fed9e2ad1fc1b13f`) `librife_windows_x86-64.dll` | `36A25B471BE88E6F915320C818022DC8657DD9BEAC22A8C3158BD7F4260CC410` |
| RIFE v4.6 model `rife-v4.6_ensembleFalse/flownet.bin` | plugin commit above | `03393CF14FE6D0AF015D24D93BAE1E5925F2828CB3B6555300A132643C617AA5` |
| `…/flownet.param` | plugin commit above | `AE9B08AF43FBA97E27AA2C40C04C35B1A75F637B2D2B01A941FA94361AB25CBD` |
| vs-miscfilters-obsolete | R2 `miscfilters-r2.7z` | `54CF54C4D66151C01C1C663ED0D47CA99C7F1B0A94927BD99B35362C02172BD2` |

Model identifier: the plugin's model index 23 = `rife-v4.6 (ensemble=False)` maps to folder `models/rife-v4.6_ensembleFalse` in this plugin version (verified in the pinned README, the repository tree and the strings in the pinned DLL). The script passes `model_path` explicitly, so no index mapping is relied on.

GPU selection: not set; the plugin uses `ncnn::get_default_gpu_index()`.

## Important Files
- `src/interpolationcontroller.{h,cpp}` — RIFE glue (paths, file checks, `VSSCRIPT_PATH`, `vf add/remove @novarife`, failure detection).
- `src/homepage.{h,cpp}` — native Vui homepage, generated artwork, home rails and local-file/session-resume actions.
- `src/mainwindow.{h,cpp}` — application stack + native Vui player UI, playback controls, interpolation combo, mpv log-message forwarding and error dialog; `openStream` / add-on subtitles.
- `src/stremio/*`, `src/stremiobackend.*`, `src/addonsbridge.*`, `src/addonconfigurewindow.*` — Stremio add-on client (see the v0.2.5 section).
- `resources/vui/stremio.js`, `resources/vui/addons.css` — add-on views in the Home page.
- `tests/stremio/*` — protocol/client tests and the mock add-on server; `tests/stremio/data/lz-string` is binary test data (`.gitattributes` `-text`).
- `docs/stremio/SOURCE_MAP.md`, `docs/stremio/COMPATIBILITY.md` — references and compatibility log.
- `resources/rife/rife.vpy` — VapourSynth script (adapted from MIT `Lafourkad/mpv-RIFE`).
- `.github/scripts/assemble-rife-runtime.ps1` — pinned, hash-verified runtime assembly.
- `.github/workflows/windows-portable.yml` — build + package (pinned libmpv, MSVC runtime, RIFE runtime).
- `.github/scripts/make-mpv-lib.ps1` — MSVC import library for libmpv.
- `THIRD_PARTY_NOTICES.md`, `licenses/` — license documentation (copied into the package).

Portable package layout additions:
```
LambdaPlayer.exe, libmpv-2.dll, Qt DLLs, msvcp140*.dll/vcruntime140*.dll
THIRD_PARTY_NOTICES.md, licenses\
rife\rife.vpy, rife\librife_windows_x86-64.dll, rife\MiscFilters.dll, rife\models\rife-v4.6_ensembleFalse\
vapoursynth\python.exe, python3.dll, python312.dll, python312.zip, python312._pth (+ "Lib\site-packages")
vapoursynth\Lib\site-packages\vapoursynth\ (vsscript.dll, libvapoursynth.dll, vapoursynth.pyd, ...)
```

## Architecture / Important Decisions
- Qt + libmpv split and native `wid` embedding are unchanged.
- Interpolation is only an mpv video filter; no custom decoder/renderer/inference. The filter is labelled so it can be removed without touching other filters.
- `hwdec=auto-safe` is kept: mpv downloads HW frames automatically when the VapourSynth filter is active, and returns to direct HW output when it is removed (verified). `mpv-RIFE` uses `--hwdec=no`, which was tested and is not needed.
- mpv's `vapoursynth` filter only accepts planar YUV; the script refuses other input and mpv falls back to unfiltered playback.
- License choice: only MIT/BSD/LGPL code/binaries are used; `president-not-sure/mpv-interpolation` (GPLv2) was read for ideas only, nothing copied. The bundled libmpv is a GPL build (as before this milestone) — see `THIRD_PARTY_NOTICES.md`.
- mpv event handling threading model is unchanged (wakeup → queued `processMpvEvents()`); log messages arrive through the same event loop.

## Do Not Break
- Stremio protocol behaviour must stay traceable to `docs/stremio/SOURCE_MAP.md`; change it only with a source reference and a test, and log deviations in `COMPATIBILITY.md`.
- Never re-encode or decode a transport URL: request URLs are built by string replacement of the `/manifest.json` path suffix (configured paths and query tokens depend on it).
- Keep one result slot per planned add-on request; one failing/slow add-on must never block or clear another.
- `http-header-fields` is set per add-on stream and cleared by `clearAddonSession()` for every other file; keep `pause=no` before `loadfile`.
- Add-on configure pages must stay in their own off-the-record profile without a WebChannel.
- Keep `Q_OBJECT` + `CMAKE_AUTOMOC` (both `MainWindow` and `InterpolationController` use `Q_OBJECT`).
- Keep seek flags as one argument (`absolute+exact`).
- Keep the Vui Home/Player page architecture and the native-widget setup. `video_` remains the libmpv target; player chrome and the transition curtain must stay compatible with the Windows video HWND.
- Home must pause the current session without destroying/recreating mpv; drag/drop and Open Video must continue to use `openPath()`.
- Never clear the whole mpv `vf` chain; only remove `@novarife`.
- Do not change dependency pins without updating hashes, `THIRD_PARTY_NOTICES.md` and this file.
- RIFE must stay optional: any failure must leave normal playback working and the combo on Off.
- Any environment variable meant for mpv or its filters must be set **before `mpv_create()`**; mpv on Windows never re-reads the environment.
- Test mpv integration changes in-process against `libmpv-2.dll` (for example with a ctypes harness), not only with `mpv.exe` launched with a prepared environment.

## Next Recommended Tasks
0. v0.2.5: confirm the CI run (the first `stream-server` job builds libtorrent/Boost/OpenSSL with vcpkg and takes long; later runs use the cache). Possible follow-ups: engine buffering/peer status in the player, next-episode/binge (bingeGroup), a Continue watching for add-on titles, trailers through the streaming server.
1. Confirm the CI run for this commit passed; download the artifact and runtime-test in `LambdaPlayer.exe`: RIFE 2× on a 24 fps and a 30 fps file (use mpv stats / visual smoothness), seek (click, drag, arrows), pause, embedded + external subtitles, audio-track switch, mute/volume, fullscreen overlay, Off → normal playback.
2. Test the failure path by renaming `rife\models` in the extracted artifact → selecting RIFE 2× should show an error and stay on Off.
3. Test on NVIDIA and Intel GPUs and on 1080p/4K content; record performance.
4. Later milestones (not started): target-FPS modes, display-Hz matching, GPU selector, model selector, presets, upscaling, settings persistence.
