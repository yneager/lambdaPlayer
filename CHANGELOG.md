# Changelog

This file records notable completed work visible in the repository history.

## Unreleased — AMD FRC post-render frame generation

- AMD FRC now also runs at 60, 120 and 200 fps: FRC only doubles, so 2-4 components are cascaded (up to a 384 fps frame grid for 24 fps video) and a presenter picks, per output tick, the frame whose content time fits best (phase-locked to the video). Measured at native 3840x2160: 60.0 / 119.8 / 201.0 presented frames/s (GPU 3D load 50 / 74 / 75 %); audio delay follows the measured 115-130 ms latency.
- Player controls: separate Speed, Audio, Subtitles and Smoothness buttons replace the Settings button; Fit/Fill for fullscreen moved to the "..." button.
- Fullscreen: no Windows 11 rounded corners or 1 px window border (DWM corner preference / border colour switched while fullscreen, restored afterwards).

- New interpolation mode "Double frame rate (AMD FRC)": mpv renders each new frame into a D3D11 texture (WGL_NV_DX_interop2), AMD AMF Frame Rate Conversion (FRC_x2_PRESENT, SUPER profile, native motion search, indicator off) generates the intermediate frame on a worker thread, and the video widget presents source + generated frames at 2x the source rate under the unchanged player chrome. No window capture, no CPU copies, no VapourSynth. RIFE stays as the legacy mode. Details and upstream sources: `docs/frc/RESEARCH.md`.
- Measured on RX 9070 XT: 24 fps 4K source -> 48.0 presented frames/s with FRC input 3840x2160 (`LAMBDA_FRC_INPUT=source`) and at 2560x1440 / 1360x840 on-screen input; 0 late, 2 ms worker time; audio delayed by the measured 66 ms lag and restored when turned off; flushed on seek, pause/resume and file change.
- Fixed a pre-existing stall: the window's vsync-blocking SwapBuffers held the GUI thread ~31 ms per repaint during playback (32 ms event latency). Swap interval 0 (DWM paces composition); `LAMBDA_SWAP_INTERVAL=1` restores the old behaviour.
- AMF SDK subset vendored in `third_party/amf` (MIT); standalone proof in `tools/frc-proof`.

## Unreleased — window, scrolling and playback performance

- Fill the native rounded window instead of drawing a rounded card inside a square backing; remove the oversized masking shadow.
- Size fullscreen video directly, ignore delayed windowed geometry, and refresh browser geometry on window-state changes.
- Reduce Home scroll repaint costs, pause offscreen hero animations, and clamp animated wheel scrolling when content height changes.
- Coalesce mpv render notifications and player UI updates; avoid target-time waits on Qt's UI thread and repeated whole-player DOM updates for the timeline.
- Bound RIFE CPU/frame concurrency, preserve per-frame colour metadata and R80 colour range, and prevent interpolation activation before a video is ready.
- Release build, eight protocol suites, local HTTP/fullscreen/seek/RIFE runtime checks and concurrent colour/timing regression checks passed. See HANDOFF.md for measurements and test limits.

## Version Map

- **v0.2.5** — Stremio add-on client: install/configure/import add-ons, Home/Discover/Search/Details from add-on catalogs and metadata, source picker, add-on streams in the libmpv player, subtitle add-ons in the subtitle menu, built-in torrent/archive streaming engine.
- **v0.2.4** — animated mouse-wheel scrolling on the Home page (eased instead of jumping per notch).
- **v0.2.3** — smoother scrolling (GPU rasterization enabled for the Vui WebEngine pages).
- **v0.2.2** — video visible again (libmpv render API under the Vui web chrome, lost-wakeup fix), frameless rounded window with custom controls, mini player, recent files/resume, polished and fully functional UI.
- **v0.2.1** — LAMBDA Player identity reset; real Vui HTML/CSS on both Home and Player while preserving the native libmpv/RIFE engine.
- **v0.2.0** — RIFE interpolation milestone: initial RIFE integration, Windows activation fix, dynamic frame-rate labels and 60 fps mode.
- **v0.1.0** — base-player milestone: libmpv playback, controls, fullscreen overlay, audio/subtitle selection, external subtitles and MKV seek/subtitle fixes.

The README keeps the user-facing permanent version history. These chronological entries retain the more detailed implementation record.

## 2026-09-24 — v0.2.5 built-in streaming engine

### Added
- `lambda-stream-server.exe`: the open-source Stremio-compatible streaming server stremio-native/stream-server (libtorrent backend, pinned commit `f585ab6`) with LAMBDA's host `tools/stream-server/lambda_stream_host.rs` (127.0.0.1 only, chosen port, ready line, exits when LAMBDA's stdin pipe closes; no tray, SSDP, HTTPS or self-update). Built by `tools/stream-server/build.ps1` (vcpkg libtorrent/OpenSSL, x86-64 baseline instead of upstream's AVX2) in a cached CI job and bundled in the portable package.
- Two upstream fixes carried as patches (`tools/stream-server/patches/`): archives downloaded over HTTP keep their extension (format detection failed for every remote archive), and the ZIP/TAR readers advance the read buffer (they returned no data).
- `StreamingServerProcess` (start on demand, remembered port with fallback, readiness, restart after a crash, stop on exit, engine temp files inside the cache) and `StreamingServer::ensure`; an external Stremio server can still be configured.
- Torrents: `/create` is always called and checked, so dead torrents, missing files and engine errors are reported in the source list with progress messages; the torrent file name and size go to subtitle add-ons.
- Add-ons → Streaming engine: state, cache folder, size limit, Clear cache (stops engine playback first).
- Links that are web pages open in the browser; YouTube streams open the YouTube page.

### Fixed
- `/opensubHash` answers with `"error": null` were treated as failures.

### Verification
- Unit tests: 8 suites pass (new: engine missing, ready line, torrent create errors, web-page links, YouTube links).
- Runtime with real Torrentio results for Night of the Living Dead (1968, public domain): single-file torrent (metadata 14 s, playback started while downloading), seek forward to 60 % (6.3 s) and back to 20 % (0.07 s), a 75-file pack with `fileIdx` 4 (correct file), a torrent without `fileIdx` (main video chosen), a magnet with trackers, OpenSubtitles results during torrent playback with the hash verified independently and an add-on subtitle loaded; test add-on: ZIP archive stream, dead torrent (error after 46 s, another source can be chosen), Clear cache during torrent playback; local file playback unchanged, engine not started for local files.

## 2026-09-24 — v0.2.5 Stremio add-on client

### Added
- `lambda_stremio` library (`src/stremio/`), ported from stremio-core (`development` @ 88be65b) and checked against stremio-web, stremio-video, the add-on SDK docs, stremio-addon-client, Debrify, Nuvio, Raffi and Cove (see `docs/stremio/SOURCE_MAP.md`):
  - manifest/descriptor parsing incl. short/full resources, full and legacy catalog extras, `UniqueVec` catalogs, `skip` normalisation, optional `idPrefixes`;
  - capability matching (`is_resource_supported`, `is_extra_supported`, `AggrRequest::plan`) and the Discover selection rules;
  - HTTP transport URL building (encodeURIComponent set, `&`-joined extras, configured paths and query tokens kept byte-for-byte) and the legacy `/stremio/v1` JSON-RPC transport;
  - response parsing (exactly one key, `null` → empty, invalid items skipped individually, all stream source forms);
  - `AddonClient` (Qt networking, 200/201 only, redirects followed, 15 s per request, HTTP disk cache + short in-memory cache, never caching failures, cancellation);
  - `AddonManager` (install from manifest/configured/`stremio://`/legacy URLs, `X-Stremio-Addon` detection, collections, identity by transport URL, update in place, `configurationRequired` refusal, enable/disable, reorder, refresh, import/export, persistence in `addons.json`);
  - `StreamResolver` + `StreamingServer` (direct URLs to mpv with `proxyHeaders` as mpv `http-header-fields`; torrent/magnet/YouTube/archives/NZB/FTP through the Stremio streaming server exactly like stremio-video; external/web-player links opened in the browser);
  - `VideoParamsFetcher` (subtitle `videoHash`/`videoSize`/`filename` like stremio-video, with a local OpenSubtitles hash over HTTP ranges when the streaming server is not running) and an lz-string port for server payloads;
  - `ContentService` (Board, Search, next page, Discover, meta/stream/subtitle planning).
- `StremioBackend`, `AddonsBridge` (QWebChannel "stremio" object for the Home page) and `AddonConfigureWindow` (add-on configuration pages in an isolated off-the-record WebEngine profile; install links become installs).
- Vui views in the Home page (`resources/vui/stremio.js`, `addons.css`): Home catalog rows and featured hero, Discover, Search, Details with seasons/episodes and a per-add-on source picker, Add-ons management and add-on discovery. The decorative placeholder sections of the Home page were removed; with no add-ons installed an empty state offers Stremio's official Cinemeta and OpenSubtitles v3.
- `MainWindow::openStream`: add-on streams use the same libmpv pipeline as local files (RIFE, tracks, fullscreen, keys). Subtitle add-ons are asked after the file loads; their subtitles and `stream.subtitles` are listed in the existing subtitle menu (grouped Embedded / External / Add-ons) and loaded with mpv `sub-add` only when chosen.
- 8 Qt Test suites (`tests/stremio/`) with a local mock add-on server; CI runs them.
- Developer aids: `LAMBDA_DATA_DIR` (separate add-on profile) and `LAMBDA_MPV_LOG` (mpv log file).

### Fixed
- A video opened after returning Home stayed paused while the controls showed it playing (`pause` survives `loadfile`; present since v0.2.2).

### Verification
- Unit tests: 8 suites pass locally (MSVC 14.51, Qt 6.8.3), including stremio-core's own URL vectors, Debrify's configured-URL cases and lz-string's test data.
- Runtime, in `LambdaPlayer.exe` with a separate test profile: installed Cinemeta, OpenSubtitles v3, Public Domain Movies, WatchHub (from Cinemeta's addon catalog), the legacy OpenSubtitles add-on and a local test add-on; Home rows, featured hero, Discover (types, catalogs, required `genre`, paging), Search, movie and series Details, episode streams with canonical ids, direct playback, `proxyHeaders` playback, subtitle add-on requests with the correct extras (hash verified independently), add-on subtitles loaded through mpv, configure-page install and persistence across restarts.
- Local playback re-checked after the change: local file, embedded/external subtitles, RIFE Original → double → 60 fps → Original (only `@novarife` added/removed).
- Not runtime-tested: torrent/YouTube/archive playback through the Stremio streaming server (not installed on the test PC; covered by mock-server tests).

## 2026-09-24 — v0.2.4 animated wheel scrolling

### Fixed
- Home-page mouse-wheel scrolling jumped ~100 px per notch instead of animating. Qt WebEngine forwards Windows wheel notches to Chromium as precise pixel deltas, so `ScrollAnimatorEnabled` never takes effect. `home.js` now eases the page toward the accumulated wheel target (frame-rate independent). Touchpads, ctrl+wheel zoom, inner scrollers and `prefers-reduced-motion` keep native behaviour; scrollbar drags, keys and nav links interrupt the animation.

## 2026-09-24 — v0.2.3 smoother scrolling

### Fixed
- Janky Home-page scrolling. Since v0.2.2, Qt Quick and Qt WebEngine run on the desktop-OpenGL path, which the OpenGL video surface needs. On that path Chromium reports `rasterization: unavailable_off`, so the Vui pages' large blurs, gradients and glows were rasterized on the CPU while scrolling.
- `main.cpp` now appends `--enable-gpu-rasterization` to `QTWEBENGINE_CHROMIUM_FLAGS` before `QApplication`. A user-supplied flags value containing `gpu-rasterization` (for example `--disable-gpu-rasterization`) is left untouched. The GPU blocklist is **not** overridden.

### Verification
- Measured on the Home page with wheel input sent through the WebEngine DevTools protocol (24 notches down, then up), recording `requestAnimationFrame` intervals. Test machine: Windows 11, RX 9070 XT, 60 Hz.

  | Build | Rasterization | Late frames | 95th percentile | Worst frame |
  |---|---|---|---|---|
  | v0.2.2 | CPU | 35–43% | 183–300 ms | ~480 ms |
  | v0.2.3 | GPU (`enabled_force`) | 0% | 16.8 ms | 16.8 ms |

- Video playback under the chrome was re-checked after the change.

## 2026-09-23 — v0.2.2 working video + frameless polished UI

### Fixed
- **Opening a video did nothing visible.** There were two causes.
  - `mpv_set_wakeup_callback()` fires immediately. libmpv only fires again after `mpv_wait_event()` drains the queue. The `mpvWakeup` → `processMpvEvents` connection was made after `initMpv()`, so the first wakeup was lost and no mpv event was ever processed. The connection now happens before `initMpv()`.
  - Video rendered into a native child HWND (`wid`), which Windows cannot composite under the transparent WebEngine chrome.
- Video now uses libmpv's render API (`vo=libmpv`, OpenGL) in `src/mpvvideowidget.{h,cpp}` (`QOpenGLWidget`).
  - The WebEngine chrome's internal `QQuickWidget` gets `WA_AlwaysStackOnTop`, so Qt blends it over the video.
  - `main.cpp` sets `AA_ShareOpenGLContexts` and the OpenGL Qt Quick API.
  - The first `loadfile` waits until the renderer exists.
  - mpv still decodes, renders subtitles and runs the RIFE `@novarife` filter unchanged.

### Added
- Frameless window: `FramelessWindowHint`, with the native frame styles re-applied.
  - `WM_NCCALCSIZE` makes the whole window client area.
  - `WM_NCHITTEST` handles resize edges and page-reported drag regions (`src/windowbridge.{h,cpp}`, `resources/vui/window.js`).
  - DWM rounded corners, dark mode and shadow.
  - Maximize goes through Win32 `ShowWindow`.
  - The app owns its fullscreen flag, because Qt reports "full screen" for a maximized frameless window on a monitor without a taskbar.
- Custom window controls, page fades, toasts and themed scrollbars (`resources/vui/window.{js,css}`).
- Home behaviour in `resources/vui/home.js`, backed by a Home WebChannel bridge:
  - real recent files in Continue watching;
  - open video, open folder and About popover;
  - scrollspy nav and rail arrows.
- Player behaviour:
  - Next opens the next video in the same folder (natural sort);
  - an always-on-top mini player;
  - a finished/replay state, a loading state, buffered range and a timeline hover time;
  - subtitles lifted above the visible deck (`sub-margin-y`);
  - a Ctrl+O shortcut.
- Resume support through mpv watch-later files (`start,aid,sid` only, so the RIFE `vf` is never restored). Recent files are stored in `%APPDATA%\LAMBDA\LAMBDA Player.ini`.
- A developer capture hook, active only when `LAMBDA_DEBUG_GRAB=<dir>` is set.

### Verification
- Built locally with MSVC 14.51 and Qt 6.8.3 and run on Windows 11 with an RX 9070 XT.
- Checked with in-app captures:
  - video visible under the chrome;
  - Home and Player layouts;
  - subtitles above the deck.
- Checked with real mouse input:
  - maximize, restore, title-bar drag and corner resize;
  - fullscreen, with Esc restoring the previous geometry;
  - opening and closing the settings panel;
  - Home showing the recent file with Resume.
- The mini player and RIFE were not re-tested by automation after this change.

## 2026-09-23 — v0.2.1 LAMBDA Player / complete Vui CSS

### Changed
- Product identity changed to **LAMBDA Player**; CMake project/target, application metadata, window titles, portable executable and CI artifact/release names now use the new identity.
- Active version reset to **0.2.1**. Current-branch v0.3.x version labels were removed from the maintained docs/version map.
- Home uses the real local Vui `index.html` + `home.css`.
- Player uses the real local Vui `player.html` + `styles.css` with only integration overrides needed for a real native video surface and working settings.
- Qt WebChannel connects the CSS player controls to the existing C++ actions and state.
- CSS settings expose real audio/subtitle tracks, external subtitle loading, playback speed and RIFE interpolation modes.

### Preserved
- libmpv native `wid` rendering into `video_`.
- Existing mpv playback, `hwdec=auto-safe`, chapter metadata, playlist-next, volume/mute, `absolute+exact` seeking, fullscreen keyboard behavior and track selection.
- Existing RIFE/VapourSynth runtime, pins and failure fallback. Only `@novarife` is managed.

### Packaging
- Qt WebEngineWidgets and WebChannel are deployed with the portable package.
- Inter remains locally bundled from its pinned upstream commit; the Vui surfaces do not require network resources.
- Portable output is `LambdaPlayer.exe`; CI artifact is `LAMBDA-Player-Windows-x64`.

## 2026-09-23 — Frame-rate labels and 60 fps mode

### Added
- **60 fps (RIFE)** mode. `rife.vpy` computes the RIFE factor as `60 / source fps`, for example 5/2 for 24 fps, 12/5 for 25 fps, 2/1 for 30 fps and 1001/400 for 23.976 fps. It does not use the plugin's `fps_num` option, because mpv's `video_in` clip has no frame rate. The source rate is mpv's `container_fps`, snapped to the nearest standard rate within 0.5%. Output timing still comes from mpv's per-frame durations. The mode is refused for sources at 59 fps or above and greyed out in the UI.

### Changed
- The selector no longer says "RIFE 2×". Entries are labelled with the current video's real frame rates, for example "Original (24 fps)", "48 fps (RIFE)" and "60 fps (RIFE)". Before a file is loaded they read "Original", "Double frame rate (RIFE)" and "60 fps (RIFE)".
- The mode is passed to `rife.vpy` through mpv's `user-data` sub-option as `<mode>|<runtime dir>`, where mode is `double` or `60`. Switching between RIFE modes removes and re-adds only `@novarife`.

### Verification
- The pinned `mpv.exe` and runtime were tested on an RX 9070 XT. `estimated-vf-fps` after enabling each mode:

  | Source | Mode | Output |
  |---|---|---|
  | 24 fps | 60 fps | 59.9995 |
  | 25 fps | 60 fps | 60.0000 |
  | 30 fps | 60 fps | 60.0006 |
  | 24 fps | double | 47.9996 |
  | 60 fps | 60 fps | refused with a clear message; playback continues |

- The C++ changes are build-verified by CI only.

## 2026-09-23 — RIFE 2× activation fix

### Fixed
- Selecting RIFE 2× failed with "mpv could not create the VapourSynth filter: error running command" (user report on the first RIFE build). Cause: on Windows mpv snapshots the process environment once, on its first `getenv()` call (`osdep/io.c`), so the `VSSCRIPT_PATH` LAMBDA Player set when RIFE was selected was never seen and mpv could not find `VSScript.dll`. Reproduced with the pinned `libmpv-2.dll`.
- Fix: `VSSCRIPT_PATH` is now set before `mpv_create()`, and `vsscript.dll` is additionally preloaded by full path before the filter is added, so mpv's by-name fallback finds it. Both variants were verified against the pinned `libmpv-2.dll` (24 fps → 48 fps); the old order still reproduces the error.

## 2026-09-23 — RIFE 2× frame interpolation (first milestone)

### Added
- `Frame Interpolation: Off | RIFE 2×` selector in the control bar (default Off). RIFE 2× adds the labelled mpv video filter `@novarife:vapoursynth=file=<rife.vpy>:user-data=<runtime dir>`; Off removes only that filter and leaves the rest of the video-filter chain untouched.
- `src/interpolationcontroller.{h,cpp}`: minimal glue that locates the bundled runtime, checks the required files, sets `VSSCRIPT_PATH`/`PATH` for libmpv's VSScript lookup, adds/removes the filter and detects mpv's "Disabling filter novarife because it has failed" log message to fall back to Off with a user-visible error. No inference code lives in LAMBDA Player.
- `resources/rife/rife.vpy`: VapourSynth script adapted from the MIT-licensed `Lafourkad/mpv-RIFE` structure. Uses `misc.SCDetect` for scene changes, converts YUV→RGBS with the matrix taken from mpv's frame properties, runs `rife.RIFE(..., model_path=<rife-v4.6_ensembleFalse>, factor_num=2, factor_den=1, sc=True)` and converts back to the source pixel format. All paths are relative to the runtime directory; no hardcoded machine paths.
- `.github/scripts/assemble-rife-runtime.ps1`: assembles the portable runtime from pinned, SHA-256-verified downloads: Python 3.12.10 embeddable, VapourSynth R80 (wheel extracted into `vapoursynth/Lib/site-packages`), VapourSynth-RIFE-ncnn-Vulkan `r9_mod_v33` (`librife_windows_x86-64.dll`), RIFE v4.6 model files from plugin commit `c3ec6aab`, and vs-miscfilters-obsolete R2 (`MiscFilters.dll`).
- `THIRD_PARTY_NOTICES.md` and `licenses/` with verbatim license texts for mpv, Python, VapourSynth, the RIFE plugin, rife-ncnn-vulkan, ncnn, the RIFE model, vs-miscfilters-obsolete and mpv-RIFE; both are copied into the portable package.

### Changed
- `Build Windows Portable` now pins libmpv to shinchiro/mpv-winbuild-cmake release `20260923` (`mpv-dev-x86_64-20260923-git-6fd80b2003.7z`, SHA-256 verified) instead of downloading `latest`; this exact build was verified to contain mpv's `vapoursynth` filter and to load `VSScript.dll` at runtime.
- The workflow additionally copies the MSVC 2022 runtime DLLs app-locally (required by Qt, VapourSynth and the RIFE plugin) and runs the RIFE runtime assembly before uploading `LAMBDA-Player-Windows-x64`.
- mpv error-level log messages are now requested (`mpv_request_log_messages(mpv, "error")`) and forwarded to the interpolation controller.
- Project version bumped to 0.2.0; README rewritten for the current feature set.

### Verification
- User runtime-verified the MKV seeking / subtitle fixes from commit `3314ef13` (progress-bar seeking, embedded subtitles, external subtitles, multiple audio tracks, fullscreen overlay) before this milestone started.
- Local runtime verification of the interpolation stack was done with the exact pinned `mpv.exe` build (same code as `libmpv-2.dll`) plus the exact pinned runtime on Windows 11 / AMD Radeon RX 9070 XT: 24 fps → 47.99 fps and 30 fps → 60.00 fps (`estimated-vf-fps`), seek, pause/resume, filter remove/re-add, `hwdec=auto-safe` (d3d11va with mpv's automatic HW download), and the failure paths (missing VSScript, script error, missing model → playback continues, filter disabled). See `HANDOFF.md` for details and for what is not yet verified inside `LambdaPlayer.exe` itself.

## 2026-09-23 — base player

### Added
- Created the initial C++20 / Qt 6 / libmpv Windows player application and CMake build configuration.
- Added local file opening, drag/drop loading, play/pause, seeking, time display, volume/mute, playback speed, fullscreen, hardware-decoding configuration, and keyboard shortcuts.
- Added the `Build Windows Portable` GitHub Actions workflow for Windows 2022 / MSVC 2022 x64.
- Added automatic libmpv development-package download and `.github/scripts/make-mpv-lib.ps1` to generate the MSVC import library from the libmpv DLL exports.
- Added audio-track and subtitle-track selectors populated from mpv's track list.
- Added external subtitle loading for common subtitle formats.

### Changed
- Reworked fullscreen controls so they overlay the video rather than reducing the video area's size.
- Added animated fullscreen control slide-in/slide-out, automatic hiding, and cursor hiding.

### Fixed
- Corrected MKV/progress-bar seeking by passing mpv seek flags as a single `absolute+exact` argument; added direct click-to-seek and made playback keyboard shortcuts work even when child controls have focus.
- Strengthened audio/subtitle switching by setting mpv `aid`/`sid` runtime properties directly, explicitly enabling subtitle visibility, enabling embedded fonts, and enabling Matroska subtitle preroll.
- Corrected Qt MOC/AUTOMOC configuration and source formatting so `MainWindow` meta-object code is generated and linked correctly on Windows.
- Added the explicit `QByteArray` include required by the track-property helper declarations.

### Verification
- Code at commit `3314ef130cc42cbc3f86fbee60ab6efc9a64f05b` completed `Build Windows Portable` GitHub Actions run #10 successfully, including build, packaging, and artifact upload. Runtime verification of the reported MKV seek/subtitle behavior remains pending.
- Earlier code at commit `027178e0f5b4c12100cc5c1e006c8ea700f7bf18` completed run #8 successfully.
