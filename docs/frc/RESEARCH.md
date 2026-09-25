# Post-render frame generation with AMD AMF FRC — research and design

Goal: replace native-4K VapourSynth/RIFE (≈10–16 fps at 3840×2160) with an
optional post-render path: mpv decodes and renders the video normally, the
rendered image goes through AMD AMF Frame Rate Conversion on the GPU, and
source + generated frames are presented at 2× the source rate. RIFE stays as
the legacy option.

## Upstream sources studied

| Source | Revision | License | Used for |
|---|---|---|---|
| GPUOpen-LibrariesAndSDKs/AMF | `6277e353` (2026-09-22) | MIT | **Used.** `amf/public/samples/CPPSamples/SimpleFRC/SimpleFRC.cpp` (context/component setup, property order, SubmitInput/QueryOutput/resubmit loop), `amf/doc/AMF_FRC_API.md` (properties, formats, `FRC_x2_PRESENT`), `amf/doc/AMF_API_Reference.md` (`InitDX11`, `AllocSurface`, `Interop`, `Flush`). Vendored headers + runtime loader in `third_party/amf/` (unmodified copies). |
| robmikh/Win32CaptureSample | `49fefe7` | MIT | Studied; **no code used** (see "Capture strategy"). `WindowList.cpp` `IsCapturableWindow` only accepts root windows. |
| microsoft/Windows.UI.Composition-Win32-Samples `cpp/ScreenCaptureforHWND` | `ee50e2e` | MIT | Studied; **no code used**. `capture.interop.h` `CreateForWindow`, `Win32WindowEnumeration.h` (root windows only). |
| Blinue/Magpie | — | GPL-3.0 | Not inspected and not used (LAMBDA does not use window capture, so its capture/presenter design was not needed). |
| Khronos `WGL_NV_DX_interop` / `WGL_NV_DX_interop2` specification | — | extension spec | OpenGL↔D3D11 texture sharing (`src/frc/wgldxinterop.*`), written against the spec. mpv's `context_dxinterop.c` (LGPL) was not copied. |

## Findings that decided the design

1. **LAMBDA's video is not a window.** `MpvVideoWidget` is a `QOpenGLWidget`
   (no native HWND, `mainwindow.cpp` "Render through libmpv's render API into
   MpvVideoWidget (no native wid)"). Qt composites it and the Chromium player
   chrome (`PlayerChrome`, raised above the video) into the one top-level
   HWND. Windows.Graphics.Capture can only capture top-level windows or
   monitors (both MIT samples filter on `GetAncestor(hwnd, GA_ROOT) == hwnd`).
   - Option A (capture the video HWND) is impossible: there is none.
   - Option B (top-level capture + crop) would feed the controls, timeline,
     toasts and their animations into FRC, and the result could not be put
     back *under* the chrome, because video and chrome are the same HWND: an
     overlay window must either cover the controls or sit below the whole
     window.
2. **AMF has no DX11↔OpenGL interop.** `AMFData::Interop(AMF_MEMORY_OPENGL)`
   on a DX11 surface returns `AMF_NOT_SUPPORTED` (proof run). The AMD OpenGL
   driver exposes `WGL_NV_DX_interop2` (proof: "yes"), and
   `wglDXOpenDeviceNV` succeeds on the D3D11 device.
3. **FRC throughput at native 3840×2160 BGRA on the RX 9070 XT** (standalone
   proof, `tools/frc-proof`, includes GL rendering of the input and GPU sync):

   | profile | MV search | future | fallback | source frames/s | outputs/s |
   |---|---|---|---|---:|---:|
   | HIGH | NATIVE | off | off | 257 | 512 |
   | SUPER | NATIVE | off | off | 233 | 463 |
   | SUPER | PERFORMANCE | off | off | 327 | 650 |
   | SUPER | NATIVE | on | off | 231 | 459 |
   | SUPER | NATIVE | off | on (blend) | 241 | 479 |

   48 outputs/s need 24 source frames/s: ~10× headroom in every mode.
4. **Output order and content lag** (moving square, 64 px per source frame):
   after each `SubmitInput`, `QueryOutput` returns `AMF_REPEAT` + the
   generated frame, then `AMF_OK` + a source frame. Measured square centres
   `… 363 395 427 459 …` = generated midpoints between consecutive sources.
   The *content* trails the latest submission by **one source frame**
   (two with `FRC_USE_FUTURE_FRAME`), although the output pts are relabeled
   (`N-0.5`, `N`). The controller therefore derives the lag from this
   measurement, not from pts.

## Implemented architecture ("render-output capture", effectively option A)

```
mpv (decode, timing, subtitles/OSD, colour, scaling — unchanged)
  │ new frame (MPV_RENDER_UPDATE_FRAME)
  ▼
MpvVideoWidget::requestFrame  ── GUI thread, GL context current
  │ mpv_render_context_render into the FBO of an AMF-allocated
  │ D3D11 BGRA texture (WGL_NV_DX_interop2)       [mpv's own render pass]
  ▼
worker thread "LAMBDA AMF FRC" (SimpleFRC loop)
  │ SubmitInput → QueryOutput (REPEAT = generated, OK = source)
  │ CopyResource(output → display slot)            [1 full-frame GPU copy / output]
  ▼
GUI thread: presentation timer (Qt::PreciseTimer)
  │ generated frame at capture + D, source frame at capture + D + T/2
  ▼
MpvVideoWidget::paintGL: glBlitFramebuffer(slot → widget FBO)   [1 blit / presented frame]
  ▼
Qt composites the Chromium chrome above → DWM → monitor
```

* Capture = exactly the video area mpv renders (no UI, no window, no crop, no
  recursion possible: the input is mpv's output, never the screen).
* Input size = the widget's physical pixels (DPR applied), or with
  `LAMBDA_FRC_INPUT=source` the frame scaled so the video has its decoded size
  (3840×2160 for 4K sources on a 1440p monitor), downscaled when presented.
* Data stays on the GPU. Per source frame: mpv render pass (into the FRC input,
  replaces the render it would do anyway); per output: one `CopyResource`
  (FRC pool surface → display slot); per presented frame: one blit into the
  widget framebuffer (which Qt composites as before).
* D = 95th percentile of capture → presentable time (continuously measured,
  2 ms margin, 4–40 ms). A/V: `audio-delay` = original + measured lag
  (≈ 1.5 source frames + D; 2.5 with future frame), restored on disable,
  failure and shutdown.
* Seek / file change / pause / resume: `AMFComponent::Flush`, generation
  counter drops in-flight outputs; paused video is drawn by mpv directly.
* Resize / fullscreen: the widget draws normally until the size is stable for
  300 ms, then FRC is re-initialised for the new native size.
* Failure (AMF/interop error, sustained overload): FRC stops, audio-delay is
  restored, playback continues at the source rate with a message.

## Pre-existing bottleneck found and fixed

With the default swap interval 1, every repaint of the LAMBDA window blocked
the GUI thread in `SwapBuffers` for ~31 ms during playback (GUI event latency
32 ms p50 with FRC off; 0 ms on Home). That capped the whole app to ~30 paints
per second — FRC presented only ~25/s. `main.cpp` now sets swap interval 0
(DWM still composites/paces; `LAMBDA_SWAP_INTERVAL=1` restores the old
setting): GUI latency 0.06 ms p50 / 2–5 ms p95, FRC presents 48.0/s.

## HDR

The normal LAMBDA path renders into an 8-bit RGBA `QOpenGLWidget` framebuffer
(mpv tone-maps HDR to SDR there). The FRC path uses BGRA8 at the same point,
so it neither adds nor removes HDR capability. A 10-bit HDR output path would
need R10G10B10A2/RGBA_F16 through the whole widget chain first.

## 60 / 120 / 200 fps (cascade)

AMF FRC only produces 2x. Higher rates cascade up to four FRC components on
the worker thread: every output of stage n is submitted to stage n+1, giving
a 2x/4x/8x/16x frame grid. Each output carries its content time in source
frames (midpoint rule from the measured output order, per stage). Stages:
power-of-two ratios map 1:1 (30 -> 60 fps = 1 stage, 25 -> 200 = 3); other
ratios use a grid at least 3x the target (24 -> 60 fps: 192 grid, 24 -> 120 /
200: 384 grid) so the chosen frames step almost evenly.

The presenter ticks at the target rate, phase-locked to content time (5 %
correction per shown frame), and assigns every arriving frame to its nearest
tick, keeping only the closest frame per tick; frames are shown at mpv's
intended time + L, where L is the measured 99th percentile of
intended-time -> available + 3 ms (also the audio delay).

Measured (RX 9070 XT, 24 fps 4K clip, fullscreen 2560x1440 monitor):

| target | stages / grid | 2560x1440 input | 3840x2160 input | GPU 3D (4K) | latency |
|---|---|---:|---:|---:|---:|
| 48 (2x) | 1 / 48 | 47.6/s | 48.0/s | 13 % | 67-71 ms |
| 60 | 3 / 192 | 60.1/s | 60.0/s | 50 % | ~117 ms |
| 120 | 4 / 384 | 119.5/s | 119.8/s | 74 % | ~125 ms |
| 200 | 4 / 384 | 199.7/s | 201.0/s | 75 % | ~124 ms |

Cascading repeats interpolation on interpolated frames, so artefacts of the
first stage are carried into later ones; 48 fps (single stage) remains the
cleanest mode. A 200 fps output on a 240 Hz monitor cannot be paced evenly
(some frames stay for two refreshes).
