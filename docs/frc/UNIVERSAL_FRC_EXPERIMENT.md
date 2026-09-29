# Universal post-render FRC experiment

Status: experimental, uncommitted, local Windows validation on one GPU (AMD Radeon RX 9070 XT). The AMD AMF backend is unchanged and remains the known-good comparison (`codex/amf-frc-baseline` at `df10dff`); this work is on `codex/universal-frc`.

## Result in one paragraph

The hand-built block matcher was replaced as the default motion estimator of the Generic D3D11 backend by **RIFE v4.26 run natively in D3D11 compute** on a reduced "analysis" copy of each frame pair. RIFE's flow and fusion mask are upsampled to output size and the **original full-resolution frames** are warped and blended — RIFE v4's own final synthesis step, done at output resolution. The displayed/output resolution is never reduced. The D3D11 executor reproduces the reference VapourSynth-RIFE ncnn/Vulkan plugin output to 52–54 dB PSNR. On native 4K frames it beats the old block matcher by ~3.2 dB mean PSNR (the block matcher was worse than a plain crossfade on every test set), matches AMF on slow content, and is visibly cleaner than AMF and the block matcher on fast occlusion and static subtitles. In-app, 4K (3840x2372 capture) 24→48 fps runs at 48.0 presented/s with ~23 ms GPU per source pair at 720p analysis; 24→60 fps holds ~60/s; 120/200 fps are not sustainable at 4K on this GPU and fall back through the existing watchdog.

## Architecture

```
mpv render (unchanged) -> D3D11 BGRA capture texture, full size (e.g. 3840x2372)
   |                                   |
   | prepare (area filter, GPU)        |  original A/B textures stay full size
   v                                   |
analysis tensors A', B' (e.g. 1166x720, zero padded to 64)
   | RIFE v4.26 flownet, D3D11 compute, once per requested t
   v                                   |
flow A->t, flow B->t, mask (analysis size)
   | packMotion -> float textures      |
   v                                   v
synthesize (output size): out = warp(A, up(flowA) * scale) * up(mask)
                               + warp(B, up(flowB) * scale) * (1 - up(mask))
   v
RGBA8 output -> existing controller output ring / presenter / audio delay
```

* Flow vectors are rescaled from analysis to output pixels (`size / analysisSize` per axis), not only resampled.
* RIFE v4's flow depends on the timestep, so one inference runs **per requested output time** (1 per pair at 2x, 1.5 average at 24→60). Nothing is linearly reused across t.
* RIFE v4.x has no refinement U-Net: its final output *is* `warp(img0)·mask + warp(img1)·(1−mask)` (verified from the graph tail). The only approximation versus native RIFE is that flow/mask come from the analysis size. Running the flownet at the native 2104-line size did not improve the large-motion failure case (below), and 540→1080 analysis changed mean PSNR by only ~0.3 dB.
* Scene cuts: mean absolute luma difference of the analysis frames (GPU reduction, no readback) above 0.1 (the threshold `rife.vpy` uses with `misc.SCDetect`) shows the nearer source frame instead of blending (`LAMBDA_RIFE_SCENE_THRESHOLD`, ≤0 disables).
* All work is on the one D3D11 device that already owns the captured frames: no Vulkan/D3D12 interop, no CPU staging, no new runtime dependency.

### Files

| File | Role |
|---|---|
| `src/frc/rifed3d11.{h,cpp}` | ncnn param/bin loader (fp16/fp32 weights), shape inference, dead-layer pruning, ResConv fusion (`leaky(conv·β + x)` in one pass), liveness-based buffer reuse, pre-baked immutable per-dispatch constants. Supports exactly the layer set of the RIFE v4 flownets (Convolution 3x3 s1/2, Deconvolution, Interp bilinear, BinaryOp, Eltwise, Crop (channel), Concat, PixelShuffle, ReLU, Sigmoid, Split, MemoryData, rife.Warp); anything else is refused with a reason. |
| `resources/frc/rife_ops.hlsl` | Layer kernels. `conv3x3` (16x8 tile, 16 outputs/thread, groupshared input tile + float4 weights), `deconv4x4s2` (2x2 output quad per input cell; 8x faster than the generic transposed-conv kernel), bilinear interp with ncnn/PyTorch `align_corners=False` coefficients, warp with border clamp/`align_corners=True`. |
| `resources/frc/rife_io.hlsl` | `prepare`, `sceneDiff`, `packMotion`, `synthesize`. |
| `src/frc/genericd3d11fruc.{h,cpp}` | Backend: `Motion::Rife` (default) or `Motion::Block` (`LAMBDA_GENERIC_FRC_MOTION=block`, old matcher kept only for A/B). |
| `src/frc/computeshaders.{h,cpp}`, `CMakeLists.txt` | Kernels are compiled by `fxc` at build time and embedded as `:/frc/cso/*.cso`. Runtime `D3DCompile` is a cached fallback only: the RIFE conv/deconv kernels take ~23 s to compile, which previously blocked every (re)configuration. |
| `tools/frc-bench/main.cpp` (`frc_bench`, not deployed) | `validate`, `interp`, `profile`, `synth`, `grid`, `tof32`. |
| `src/frc/screeninterpolationcontroller.cpp` | Only diagnostics (`genericMotion`, `rifeModel`, `rifeAnalysisWidth/Height`) and a backend-neutral failure message. Capture, ring, presenter and audio compensation unchanged. |

### Configuration

| Variable | Default | Meaning |
|---|---|---|
| `LAMBDA_RIFE_ANALYSIS_HEIGHT` | 720 at ordinary rates; 540 for more than 2× output or 4K ≥60 fps | flownet input height (clamped to the capture height); output always stays at capture resolution |
| `LAMBDA_RIFE_MODEL` | first of `rife-v4.26_ensembleFalse`, `rife-v4.25-lite_ensembleFalse` in `rife/models` | model directory name or absolute path |
| `LAMBDA_GENERIC_FRC_MOTION` | RIFE | `block` selects the old block matcher |
| `LAMBDA_RIFE_SCENE_THRESHOLD` | 0.1 | scene-cut threshold, ≤0 disables |
| `LAMBDA_RIFE_FLUSH_DISPATCHES` | 0 | Flush every N dispatches (experiment; no measured benefit) |

RIFE v4.6 (shipped for the VapourSynth path) is **not** selected automatically: it doubled a static subtitle in the synthetic test. Without a v4.26/v4.25-lite model the Generic option is disabled with the reason "No RIFE v4 model found". `.github/scripts/assemble-rife-runtime.ps1` now adds `rife-v4.26_ensembleFalse` from the same pinned plugin commit (`c3ec6aab…`), SHA-256 verified; the local files in `_local/testmodel` were verified byte-identical to that commit.

## Test-setup repair (the invalid `generic-640` run)

`_local/grab/generic-640.json` reported a 100x30 video area and a 7200x2160 FRC input. 100x30 is Qt's default size for a widget never given geometry: the native video widget is positioned only from `reportVideoRect()` in `player.js`, which runs from `requestAnimationFrame`. Chromium suspends rAF for occluded/background pages, so a benchmark window launched behind other windows never reported its rectangle. The 7200x2160 input then follows mechanically from `inputSizeFor` in source mode: k = max(3840/100, 2160/30) = 72 → 7200x2160. The same launch on an unobstructed window gives 1360x840 / 3840x2372.

The production pipeline was not changed. The local harness `_local/tools/frctest.ps1` now:

1. starts the app with `--disable-backgrounding-occluded-windows --disable-renderer-backgrounding --disable-background-timer-throttling`;
2. calls `lambdaUi.reportVideoRect()` through CDP (no rAF dependency) and reads the `.player` rectangle and DPR back;
3. **fails with exit code 2** unless the widget equals the page rectangle × DPR, the area is at least `-MinVideo` (default 1280x720), and the FRC input equals the widget size or, with `-InputMode source`, exactly the size that holds the decoded video at native pixels. Verified negative case: `-MinVideo 3000x2000` → `PREFLIGHT FAILED: video area 1360x840 is below 3000x2000`.

Every in-app number below comes from runs that passed this preflight.

## Correctness of the D3D11 RIFE executor

Same inputs (two real frames, 960x540, raw float RGB) through `frc_bench validate` and `_local/tools/rife-reference.py` (the pinned VapourSynth-RIFE-ncnn-Vulkan plugin, which stores activations as fp16):

| Model | PSNR D3D11 vs plugin | max abs diff | (A vs B, for scale) |
|---|---:|---:|---:|
| rife-v4.6 | 54.00 dB | 0.031 | 32.99 dB |
| rife-v4.25-lite | 52.64 dB | 0.127 | |
| rife-v4.26 | 54.13 dB | 0.0037 | |

Re-checked after each kernel optimisation (still 54.13 dB).

## Quality at native resolution

All scores: `frc_bench interp` through the real `FrameInterpolator` backends at 3840x2160 (synthetic: 3640x2104), t = 0.5, PSNR against the true middle frame. AMF is given two history frames (`--pre`), without which it returns frame A.

Real frames (`_local/testmedia/4k30s.mkv`, frames s+4 / s+6 → truth s+5; a slow camera move on detailed foliage):

| Case | set 96 | 300 | 460 | 620 | mean |
|---|---:|---:|---:|---:|---:|
| plain blend | 42.64 | 37.80 | 38.45 | 40.97 | 39.97 |
| **old block matcher** | 41.18 | 37.34 | 37.32 | 39.59 | **38.86** |
| AMD AMF | 45.54 | 40.66 | 40.18 | 42.56 | 42.24 |
| RIFE 4.26 @540 | 45.53 | 40.17 | 39.86 | 41.80 | 41.84 |
| **RIFE 4.26 @720 (default)** | 45.78 | 40.19 | 40.03 | 42.21 | **42.05** |
| RIFE 4.26 @1080 | 45.88 | 40.21 | 40.04 | 42.42 | 42.14 |
| RIFE 4.26 @2160 (set 96) | 45.91 | | | | |
| RIFE 4.25-lite @720 | 44.88 | 39.94 | 39.85 | 41.41 | 41.52 |
| RIFE 4.6 @720 | 46.21 | 40.79 | 40.22 | 43.10 | 42.58 |

Synthetic known motion (`frc_bench synth`, `_local/tools/frc-synth.ps1`): background pans 48x12 px per source interval, a dark card with 12 thin white lines and text moves 291 px per interval (occlusion + disocclusion), a thin red line, and (second run) a static burned-in subtitle:

| Case | pan + card | + static subtitle |
|---|---:|---:|
| plain blend | 20.70 | 20.71 |
| old block matcher | 20.41 | 20.42 |
| AMD AMF | 26.07 | 26.00 |
| RIFE 4.26 @540 / @720 / @1080 | 25.22 / 25.04 / 25.46 | 24.85 / 24.85 / 25.36 |
| RIFE 4.26 @2104 (native analysis) | 25.73 | |
| RIFE 4.6 @720 / @1080 | 25.91 / 25.72 | 22.49 / 23.26 |
| RIFE 4.25-lite @720 | 25.90 | 25.23 |

Visual review (`frc_bench grid`, crops under `_local/frcout/…/grid-*.png`):

* **Old block matcher**: rectangular ghost blocks around every moving edge, doubled text, doubled roots on the slow real footage. Confirms the owner's report; it is worse than a crossfade.
* **AMF**: clean on slow footage and on the static subtitle; on the fast card it produces blocky tearing and smeared background on both edges and breaks the moving text into blocks.
* **RIFE 4.26**: card edges and the revealed background are clean; thin lines on the card wobble slightly; the fast-moving card text is bent and partially doubled; the card's lower corner is rounded into the background. Static subtitle stays clean.
* **RIFE 4.6**: better PSNR on the slow real footage, but the card's thin lines braid across each other and the static subtitle is visibly doubled ("Sttatic subttitle") — the reason it is not the default.
* The fast-text failure is **not** caused by the reduced analysis size: native-size analysis produced the same kind of distortion (25.73 dB, visually no better). It is the model's limit at ~145 px displacement per half interval.

## Performance (RX 9070 XT, driver 32.0.31041.1004 per earlier notes)

Network only, `frc_bench profile`, synchronous GPU timestamps, warm clocks:

| Tensor | v4.26 | v4.6 |
|---|---:|---:|
| 960x576 | 9.0 ms | 10.1 ms |
| 1280x768 | 13.5 ms | 14.9 ms |
| 1920x1088 | 26.3 ms | 32.6 ms |

At 960x576, conv3x3 is ~6.6 ms of 56 dispatches (the small late blocks are latency-bound: vectorising the weight reads gained only ~4%), concat copies 0.85 ms, deconv 0.87 ms (was 7.1 ms with the generic transposed-conv kernel). Prepare + scene metric + pack + synthesize at 4K: ~0.3 ms total. Arena memory: 179 MB for v4.26 at 960x576 (buffers are reused by lifetime).

In app (`_local/tools/frcrun.ps1` / `frctest.ps1`, `4k30s.mkv`, windowed 1360x840, `-InputMode source` → capture 3840x2372, analysis 1166x720, per-second samples after warm-up). `gpu` is the D3D11 timestamp span of one source pair's work including the controller's output-slot copies, **not** GPU utilisation:

| Target | captures/s | generated/s | presented/s | late | dropped presentations | GPU per pair | CPU submit | latency (= audio delay) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 48 (2x) | 24.0 | 24.0 | 47–49 | 0 | 3 in ~12 s | 22–23 ms | 0.4 ms | 26–27 ms |
| 60 | 24.0 | 48.0 | 57–62 | 0 | 9–11 in ~10 s | 27–34 ms | | 65–67 ms |
| 120 | — | — | ~30 | | watchdog disabled FRC | 36 ms (540p analysis) | | |

Lifecycle (`_local/tools/frclifecycle.ps1`, fullscreen 2560x1440, capture 3840x2160): playing 47.9/s, paused 0 (mpv draws), resumed 48.0/s, seek to 70% and 20% back to 47.4–48.0/s, A/V sync within ±6 ms throughout, turning FRC off restored `audio-delay` to 0.

**120/200 fps at 4K**: generation keeps up at 540p analysis (118.7 outputs/s, 36 ms per 41.7 ms source interval) but presentation falls to ~30/s: the GUI thread's `wglDXLockObjectsNV` in `paint()` waits for all queued D3D11 work on the device (paint P95 ≈ 37 ms ≈ one full RIFE batch). Splitting RIFE submissions with periodic `Flush()` did not shorten that wait. The existing watchdog turns FRC off and playback continues at the source rate. Fixing this needs a presentation path that does not serialise behind the compute queue (separate D3D11 device/queue or a different present path) — not attempted.

## Heavy 4K follow-up: presentation pipeline fixes

The owner reported that performance was still bad on a real 4K movie. The local test clips were very low bitrate (2.8-8.5 MB), so a movie-like file was generated with the shipped libmpv (`_local/tools/encode-heavy.py`): the 4K clip plus temporal grain, 10-bit HEVC at ~90 Mbit/s (hevc_amf), tagged BT.2020/PQ so mpv tone-maps. On it, fullscreen 2560x1440 with default settings, **every** FRC backend presented only 30-41 of 48 frames/s, AMF included, while generation kept up. Measurements (`_local/tools/frcrun.ps1`, `gpu-engines.ps1`):

* not CPU (process 64% of one core on 28 threads), not GPU throughput (3D engine ~10%, decode engine 32%), not the decoder (software decode behaved the same), not HDR tone mapping (the SDR encode behaved the same);
* capture instants jittered 14-18 ms (P95) around the 41.7 ms source grid with the high-bitrate decode;
* every rejected frame was "mapped to an already presented tick": the presenter derived ticks by rounding arrival-based wall times and phase-locked its clock to the frames it showed, so late frames dragged the clock and cascaded;
* with RIFE, the GUI thread's `paint()` waited behind the whole inference batch on the shared D3D11 device (paint P95 23-34 ms).

Changes (all backends benefit; production behaviour otherwise unchanged):

1. **Phase-locked capture grid** (`captureFrame`): intended source times follow a grid that absorbs capture jitter (gain 0.05, whole-interval steps for skipped frames, re-anchor on larger disturbances).
2. **Deterministic presenter** (`onOutput`/`presentDue`): tick = (content - origin) / content-per-tick, with content-per-tick frozen from the configured backend grid; the tick clock follows the grid + latency, never arrival times; a due frame is shown at the next wake-up unless a newer one was shown.
3. **Separate compute device for the Generic backend** (`GenericD3D11Fruc`): RIFE runs on its own D3D11 device on the same adapter; captures and outputs cross devices through NT-handle shared textures ordered by D3D11.4 shared fences; the worker CPU-waits for finished outputs before the controller's small slot copy on the interop device, so the OpenGL present never queues behind inference. Falls back to one device if fences/sharing are unavailable (`LAMBDA_GENERIC_FRC_SEPARATE_DEVICE=0` forces it). Bench output is identical (40.19 dB on set 300 either way).
4. `LAMBDA_MPV_OPTIONS` developer hook and new diagnostics (`workerMs`, `captureJitterP95Ms`, late-reason counters, `genericSeparateComputeDevice`).

Heavy 4K HDR file, fullscreen 2560x1440, 24 fps source:

| Mode | before | after | late | dropped | paint P95 |
|---|---|---|---:|---:|---:|
| RIFE 2x (48) | 29-41/s | 48.0-49.1/s | 0 | 1 | 14 ms |
| RIFE 60 | 49-60/s, drops growing | 59.7-60.2/s | 0 | 5 | 14 ms |
| AMF 2x | 30-39/s | 48.0/s | 0 | 0 | 2 ms |
| AMF 60 | - | 59.4-60.6/s | (finer-grid frames only) | 1 | 7 ms |
| RIFE 120 | fallback | fallback at 720p analysis (56 ms per pair); 93-98/s at 540p | | | |

Lifecycle on the heavy file (RIFE 2x): play 47.5/s, pause, resume 47.6/s, seek 70% 47.9/s, seek 20% 46.8/s, A/V sync within 3 ms, off restores `audio-delay` 0. Light clip windowed: 48.0/s, 0 late.

## Reproduction

```powershell
.\build.ps1 -NoRun                                   # app + frc_bench; fxc precompiles the kernels
Copy-Item _local\build\frc_bench.exe _local\app\      # bench uses the app's Qt DLLs
python _local\tools\extract-frames.py _local\testmedia\4k30s.mkv _local\frames\4k30s 96 7   # libmpv, native PNG
.\_local\tools\frc-quality.ps1                         # real-frame table
.\_local\tools\frc-synth.ps1 -Dx 48 -Dy 12 -Name sub48 # synthetic table
_local\app\frc_bench.exe profile <model dir> 960 576   # per-layer GPU profile
_local\app\frc_bench.exe validate <model dir> 960 540 a.f32 b.f32 out.f32   # vs tools\rife-reference.py
.\_local\tools\frcrun.ps1 -Option "fps (Universal D3D11)"          # in-app, preflighted
.\_local\tools\frclifecycle.ps1 -Option "fps (Universal D3D11)"
```

`ctest --test-dir _local\build --timeout 120`: 8/8 suites passed (no hang this time).

## Licensing

RIFE weights: MIT (hzwer / Megvii, Practical-RIFE). ncnn model conversions: from styler00dollar/VapourSynth-RIFE-ncnn-Vulkan at the already-pinned commit, MIT. The D3D11 executor and shaders are LAMBDA code written against ncnn's layer semantics; no ncnn, rife-ncnn-vulkan, VipleStream, HopperRender (GPL) or MPC-VR code was copied. `THIRD_PARTY_NOTICES.md` lists the new model. The checkout still has no root license; a project-wide review is still required before distribution.

## Not done / open

* The user's actual Comet stream (Game of Thrones S1E1, 4K) was not available in this workspace; all real-footage tests use the local `4k30s.mkv` (slow camera motion) plus synthetic fast motion. Dark scenes and real cuts were not measured (the scene-cut path is implemented but untested on real content).
* Only the RX 9070 XT was tested. The executor needs D3D11 feature level 11.0 compute and ~200–700 MB of buffers depending on analysis size; NVIDIA/Intel are untested, and the OpenGL/D3D11 interop dependency is unchanged.
* Remaining failure modes: fast-moving text/fine repeated structure (RIFE itself), and RIFE at 120/200 fps at 4K (compute-bound on this GPU; the watchdog falls back).
* Conv kernels are straightforward fp32 compute; fp16 arithmetic or wave-level optimisations could cut the ~9–14 ms network time further.
* D3D11 VideoProcessor FRC is still only capability-probed; ONNX Runtime/DirectML and IFRNet were not pursued because the native executor met the 2x/60 fps budget without new runtime dependencies or cross-API sharing.
