# 4K interpolation investigation brief

Prepared 2026-09-24 for independent engineering review. This describes the current local working tree and measurements; it is not a claim that 4K real-time interpolation is solved. The owner asked to keep native 4K detail and prioritize quality. Changes are uncommitted and have not been pushed.

## Problem and observed behavior

With a 4K Game of Thrones episode open through Comet, enabling interpolation leaves it enabled but playback falls to roughly 8–10 fps. The source is 3840×2160. The desired behavior is native 4K image detail with smooth playback; silently downscaling the video is not acceptable. The other reported issues were fullscreen leaving black bars and a faint animated blue element over the video.

The local reproduction used synthetic 3840×2160 10-bit HEVC video derived from a small sample, not the user's Comet stream or the actual episode. It reproduces the expensive 4K inference path but cannot establish whether Comet's stream format, bitrate, decoder, timestamps, or hardware decode settings add another bottleneck. The user's RX 9070 XT was identified from application/runtime context. Record exact driver, clocks, power, temperature, and stream codec before drawing hardware-specific conclusions.

## Relevant playback pipeline

LAMBDA is a Windows C++20/Qt 6 app. libmpv decodes, times, and displays video through its render API in `MpvVideoWidget` (`QOpenGLWidget`). Optional interpolation uses mpv's VapourSynth video filter. `resources/rife/rife.vpy` loads VapourSynth R80 and the pinned `VapourSynth-RIFE-ncnn-Vulkan` plugin, model `rife-v4.6_ensembleFalse` (plugin commit `c3ec6aabc07c8fa37a4f58d7fed9e2ad1fc1b13f`). The plugin uses Vulkan inference. The script converts input frames to RGB for RIFE, generates intermediate frames, restores source colour/range metadata, and converts back for mpv.

At 3840×2160, each RGB float frame is large: 3840 × 2160 × 3 × 4 bytes, about 95 MiB before intermediate buffers, model activations, and other copies. For 24 fps input and 60 fps output, the filter must produce 36 additional output frames each second. It must also keep input decoding, color conversion, transfers, inference, and output scheduling moving on time. This is a pipeline throughput problem; core count or GPU utilization alone does not identify the slow stage.

Current RIFE scheduling in the shipped app is 4 concurrent requests, 4 buffered frames, and up to 8 VapourSynth CPU workers (leaving scheduling headroom). The plugin's built-in GPU worker default remains 2. mpv filter concurrency is configured in `src/mainwindow.cpp`. Source colours are carried per frame because frame requests are concurrent. The script accounts for R80 `_Range` values (0 limited, 1 full). Do not change the range handling casually: it was tested against the actual plugin for full/limited range and BT.601/BT.709.

## Measurements collected

These are local synthetic-frame benchmarks against the actual bundled VapourSynth runtime and pinned RIFE plugin, on the user's RX 9070 XT. They are useful for relative comparisons, not equivalent to a full Comet playback test.

| 3840×2160 configuration | Measured output |
| --- | ---: |
| 2 GPU workers / 4 requests | about 10.7 fps |
| 4 GPU workers / 8 requests | about 16.0 fps |
| 8 GPU workers / 16 requests | 12.0 fps in one run, then Vulkan allocation failure |

The actual benchmark runs varied somewhat (another batched 2-worker run was about 8 fps). Greater concurrency improved throughput up to 4 workers, then memory pressure caused failures/regression. A Windows GPU-engine counter sampled the benchmark process's compute engine at about 52–62% in several samples with the 2-worker setup. This means the GPU was not continuously busy in those samples, but does not prove the unused share can be converted into useful RIFE throughput. CPU submission, synchronization, memory transfers, allocation pressure, and dependencies between frames can leave the device waiting.

Resolution scaling from the same plugin/pipeline showed roughly 30–42 fps at 1920×1080 with 2–4 GPU workers, and about 87 fps at 1280×720 with 4 workers. This supports pixel/inference cost as a major factor. It does not justify reducing the owner's source resolution; the owner explicitly chose native 4K quality.

Three other RIFE model directories were benchmarked locally but not made part of the app: `rife-v4.26_ensembleFalse` was about 7.2 fps and had an allocation failure; `rife-v4.12_lite_ensembleFalse` about 8.6 fps; `rife-v4.25-lite_ensembleFalse` about 8.2 fps with an allocation failure. These results do not support swapping to those models for speed. Model file compatibility, visual quality, and licensing/notice requirements would need review before shipping any new model.

The plugin source was inspected at the pinned commit. It enables Vulkan compute, fp16 packed and storage, but sets fp16 arithmetic false. Its README documents GPU-thread tuning and an UHD option. In this plugin's v4 processing path, the UHD option does not affect the selected v4 implementation, so toggling it is not a valid optimization for the current model. Changing precision requires an actual plugin build/validation; it was not done. No Vulkan SDK or ncnn development setup was found in the local environment.

## Current changes and their effect

* An overload watchdog in `src/mainwindow.cpp` watches dropped-frame growth and A/V sync while interpolation is active. After sustained severe overload (three samples, after a warm-up), it turns interpolation off, returns to source frame rate, and shows a message. It resets around seeks, file loads, pauses/cache waits, and mode changes. In the local 4K test, drift had grown to about 3.10 seconds with 27 dropped frames; after fallback, playback returned to source 24 fps, A/V drift was about 0.00002 seconds, and drops stopped increasing in the observed interval. This recovers usable playback while preserving source resolution; it does not make 4K interpolation real-time.
* The blue spinning pseudo-element and other animated ambient layers are removed from the active-video state in `resources/vui/styles.css`. The temporary center state is hidden during playback. Browser inspection on a local clip confirmed these layers computed to `display:none`.
* Fullscreen defaults to fill/crop via mpv `panscan=1`; a new Fit/Fill setting is exposed in the player settings. Fit (`panscan=0`) shows the whole image and can leave aspect-ratio bars; Fill crops the image edges to fill the screen. A 4:3 test on a portrait monitor confirmed Fill removed the aspect-ratio bars and Fit retained them. This only addresses bars caused by display/source aspect ratio; black bars encoded inside the video image are part of the pixels and require separate crop detection/handling.
* Earlier UI/render changes in the working tree coalesce mpv render notifications and player state updates, avoid blocking the Qt GUI thread on mpv's target render time, optimize timeline DOM updates, and reduce expensive Home-page blur/scroll repaint work. The scroll measurement recorded in `HANDOFF.md` was 180 animation frames: median/p95 improved from 33.3/50.1 ms to 16.7/16.7 ms on the same local setup.

## What remains unresolved

Real-time 4K interpolation is not achieved. Best measured throughput from the tested current model/configuration was around 16 fps at 4K, versus a 48–60 fps target, and even 24 fps source playback required watchdog recovery in the synthetic 4K run. The actual Comet/Game of Thrones file has not been benchmarked in this workspace. Do not represent the watchdog as an interpolation performance fix; it is a graceful failure path.

Fullscreen Fill handles aspect ratio, but not encoded black borders. Comet-specific fullscreen opening behavior has not been validated against the user's actual movie/add-on. The app is locally built at `_local/app/LambdaPlayer.exe`; do not push or commit until the owner decides.

## Suggested investigation for the next engineer/AI

Start with the real stream and separate these stages with timestamps: demux/decode, VapourSynth frame request wait, RGB/YUV conversion, CPU↔GPU transfer, Vulkan inference, output scheduling/render. Capture GPU engine types (3D/compute/copy/video decode), VRAM committed/available, CPU per-thread utilization, clocks/power, mpv `hwdec-current`, `container-fps`, `avsync`, `frame-drop-count`, and VapourSynth queue depth while testing. A global GPU percent is not enough; GPU video-decode and compute engines are distinct.

Then inspect the pinned plugin's frame/thread model and profile one RIFE call at native resolution. Potential avenues to evaluate, with quality checked against the user's stated preference, include reducing unnecessary full-frame copies/conversions, avoiding CPU round-trips, asynchronous staging/overlap, a newer Vulkan/ncnn/plugin path with the RX 9070 XT supported well, validated half precision, and an architecture/model explicitly optimized for UHD or real-time 4K. Benchmark each change on the same actual source, and record visual artifacts as well as output FPS. Avoid increasing workers blindly: 8 workers already caused Vulkan allocation failures. Do not assume adding CPU cores helps until CPU submission or conversion is measured as limiting.

## Reproduction and local artifacts

Build the desktop app with `.\build.ps1 -NoRun`; run the test suite with `.\build.ps1 -Test`. The engine build script is `tools/stream-server/build.ps1`, but streaming engine sources were not changed in this work. Temporary clips, scripts, captures, benchmark output, and isolated app profiles are under ignored `_local/` paths and are not tracked. In particular, `_local/tools/bench-continuous.py`, `_local/tools/bench-utilization.py`, `_local/grab/4k-early-stats.json`, and `_local/grab/4k-recovered-stats.json` held the benchmark/diagnostic evidence during the investigation. The debug stats hook is enabled with `LAMBDA_DEBUG_GRAB`; use `*-stats.request` for JSON without an OpenGL screenshot, because screenshots can stall presentation and distort frame-drop measurements. Diagnostic fields include frame drops, A/V sync, hwdec, source dimensions/pixel format, panscan, and OSD dimensions; no stream URL or headers are recorded.

The RIFE color/timing regression check is `tests/rife/color_runtime.py` and runs using `_local/app/vapoursynth/python.exe`. The current workspace also has eight Qt protocol test suites, which passed before the final fullscreen/watchdog edits; see `HANDOFF.md` for the prior validation details. Re-run relevant checks after any further implementation change.
