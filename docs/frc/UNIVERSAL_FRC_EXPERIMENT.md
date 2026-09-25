# Universal post-render FRC experiment

Status: experimental, uncommitted, local Windows validation only. The working AMD AMF backend remains available on `codex/amf-frc-baseline` at `df10dff`; this work is on `codex/universal-frc`.

## Integration and ownership

The common interface is `src/frc/frameinterpolator.h`. It passes a D3D11 texture plus an owning reference, and returns the same texture/owner shape with a fractional source-frame content coordinate. `AmfFrcInterpolator` implements the interface while retaining AMF surface ownership and existing timestamps. `GenericD3D11Fruc` uses its own D3D11 textures. `ScreenInterpolationController` still owns capture, WGL interop, the output ring, presentation timing, and audio-delay compensation; Chromium remains outside the captured/interpolated picture.

mpv renders through its existing OpenGL render API into a D3D11 BGRA8 input texture registered with `WGL_NV_DX_interop2`. The controller's source-resolution capture on the test's 1360x840 widget became 3840x2372 because the render surface includes the letterbox area needed to preserve a 16:9 image in the widget's aspect ratio. The video content itself is 3840x2160. This is GPU-only after mpv rendering; no CPU pixel readback is used. Interop is runtime-probed and remains a dependency on every vendor.

## Generic compute prototype

The backend independently implements sparse block matching and bidirectional warping using D3D11 compute shaders; it does not copy VipleStream, HopperRender, or MPC Video Renderer source. Current parameters are 32x32-pixel motion cells, a 24-pixel search radius with 4-pixel displacement steps, and 16 luma samples per candidate block. It calculates forward and backward vector fields once for each source pair, then reuses them to render exact requested output timestamps. The 60/120/200 fps options use one source-pair motion calculation and multiple full-resolution warp dispatches as needed. Source frames that land on the requested output grid are copied through a channel-order conversion shader.

This is a prototype-quality block matcher, not an adoption of a complete production implementation. It has no vector confidence cleanup, edge-aware smoothing, occlusion/disocclusion resolution, or scene-cut detection yet. Those omissions can produce ghosting, block boundaries, or incorrect motion on crossings and scene changes. The output is RGBA8, matching the normal widget's 8-bit path; it does not preserve an HDR/10-bit pipeline that the widget does not currently provide.

The 4K GPU duration uses a non-blocking D3D11 timestamp-query ring around motion estimation, all output warps and the controller's output-slot copies. `genericCpuSubmitMs` is CPU time spent submitting that work. Neither value is a GPU utilization percentage. This measurement is not available for the AMF backend.

## D3D11 VideoProcessor capability probe

Startup probes a 3840x2160 progressive 24-to-48 fps enumerator, checks BGRA8 input/output support, reads all rate-conversion groups, looks for `D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_FRAME_RATE_CONVERSION`, creates a matching processor, and configures its stream with `RepeatFrame=FALSE`. Microsoft documents that this flag represents interpolation and that `RepeatFrame=FALSE` requests interpolation rather than repetition ([processor caps](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_video_processor_processor_caps), [output-rate method](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11videocontext-videoprocessorsetstreamoutputrate)). The probe deliberately does not expose a selectable backend: it has not submitted real surfaces through `VideoProcessorBlt` or checked output quality.

On the tested RX 9070 XT driver, one rate-conversion mode advertised interpolation, both BGRA directions were supported, a processor was created, and generated-rate setup succeeded. The advertised mode reported zero past and zero future reference frames. Microsoft describes those counts as the reference frames needed for optimal processing ([rate-conversion caps](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_video_processor_rate_conversion_caps)). This is a promising capability result, not proof of useful hardware FRC output.

## Local benchmark

Command used for each rate:

```powershell
.\_local\tools\frctest.ps1 -Clip "$PWD\_local\testmedia\4k30s.mkv" -Seconds 12 -InputMode source -Option "<rate> fps (Universal D3D11)"
```

The test file is a local synthetic 3840x2160 clip, not the user's Game of Thrones episode or Comet stream. The local machine reports AMD Radeon RX 9070 XT; 4K capture size was 3840x2372. Approximate last active per-second samples:

| Output target | Generic outputs/s | Presented/s | Generic GPU time per source pair | CPU submission | Pipeline latency | Presentation issues |
|---:|---:|---:|---:|---:|---:|---|
| 48 fps (2x) | 48 | 47–48 | 9.4 ms | below 0.1 ms before arbitrary-time path; later about 0.1 ms | 26–28 ms | 5–12 skipped/late events across the run |
| 60 fps | 60 | 60 | about 10.1 ms | 0.1–0.3 ms | about 39 ms | 3 dropped presentations in the run |
| 120 fps | 119–122 | about 120 | 10.6 ms | 0.18–0.24 ms | about 38 ms | 2 dropped presentations; timer lateness p95 about 6.7 ms |
| 200 fps | about 200 | about 176 | 11.3 ms | 0.27–0.31 ms | about 45 ms | 279 dropped presentations; timer lateness p95 about 7.6 ms; paint p95 about 11.5 ms |

The 60 and 120 targets were presented at their targets in the observed samples. At 200 fps the worker generated the requested rate, but the GUI presentation path did not keep up. The 240 Hz monitor is available, but the 5 ms target tick is shorter than measured p95 paint/timer latency at that mode. The presenter's existing schedule/drop behavior was retained. Do not describe 200 fps as a successful presentation result.

The same clip through the preserved AMD AMF mode produced about 48 presented fps, with about 76 ms pipeline latency and about 66–67 ms p95 intended-to-available time. Generic at 48 fps measured about 26–28 ms pipeline latency and about 22 ms p95 intended-to-available time. This is a local timing comparison only; output quality was not scored and the streams were not pixel-compared.

The measured Generic compute occupies roughly 9–11 ms of GPU execution per 24 fps source interval on this one adapter. It is clear evidence of GPU execution, but not a readout of total GPU utilization or proof that all available GPU throughput is used. The 4K60 difficulty is the amount of work, timing, and presentation deadline: the prototype processes roughly 9.1 million pixels per source frame pair (including the scaled letterbox area), searches candidate motion on the GPU, then warps every output pixel; each 60 fps presentation has a 16.7 ms period. More CPU cores cannot parallelize the GPU dispatches automatically, and the measured CPU submission portion is small compared with the GPU work.

## Compatibility, licensing, and remaining work

Only one adapter/driver combination (RX 9070 XT) has been tested. D3D11 feature level 11 compute is widely available on AMD/NVIDIA/Intel, but LAMBDA's OpenGL-to-D3D texture sharing also needs a suitable WGL interop extension from the active GL driver. No NVIDIA or Intel runtime test has been done.

The checkout has no root `LICENSE`/`COPYING` file. VipleStream declares GPL-3.0, HopperRender declares GPL-3.0, the related mpv interpolator declares GPL-2.0/LGPL-2.1, and MPC Video Renderer declares GPL-3.0. They were studied as references only; their source and shader text were not copied. The generic technique was implemented independently. Existing AMD AMF files retain their vendor MIT notices. Do a project-wide license review before distribution.

Still required before recommending a universal default: visually score representative pans, text/subtitles, occlusion, scene cuts and thin lines; run the actual Comet episode; validate arbitrary timestamps at variable frame rates; improve motion cleanup/fallback; test the VideoProcessor with actual input/output views and measure its quality/latency; test NVIDIA and Intel; collect GPU-engine utilization/VRAM counters; and determine whether the 200 fps presenter can be paced without missing ticks. Current build command: ` .\build.ps1 -NoRun ` (remove surrounding spaces in the shell). Build passed after the current implementation changes.
