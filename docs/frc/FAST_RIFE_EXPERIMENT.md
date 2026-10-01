# Fast RIFE experiment

The Smoothness menu includes **240 fps (Fast RIFE, experimental)** and a
three-stop quality slider. It uses the
existing D3D11 RIFE model on any adapter supported by that backend. It does
not require SVP or copy SVPflow. The mode estimates motion once at the midpoint
of each pair of source frames, then scales that motion to synthesize the other
requested times. Motion analysis defaults to 540 pixels high; synthesis samples
the original output-resolution frames. This saves repeated neural-network
inferences at high target rates, at the cost of less accurate motion on objects
that accelerate, change shape, or uncover background.

**Stable** analyzes at 360p, **Balanced** at 540p, and **Maximum smoothness** at
720p. The maximum setting reads the scene-difference score and, if the previous
source pair had GPU headroom, runs one extra exact RIFE inference on an
intermediate timestep of a harder pair. The extra timestep alternates between
early and late positions on successive pairs. Reusable motion is restored after
that inference. This bounded refinement is not a per-pixel neural pass; it
costs more GPU time and may not run on every pair. Sustained load lowers the
effective analysis setting, and recovery waits 15 seconds before raising it.

The occlusion rule moves the fusion weight toward the closer real source frame
near each end of the interval. It also favors that source when the two warped
colors disagree. This reduces translucent duplicate objects but can distort
edges. The ordinary Universal D3D11 modes still run RIFE separately for each
requested time and are the quality reference.

On the local Radeon RX 9070 XT, `frc_bench` at 3840×2160, 24→240 fps, 540p
analysis measured a median **21.43 ms** of GPU work per source pair for Fast
RIFE (five runs), compared with **95.62 ms** for ordinary RIFE (three runs).
The known-motion synthetic pair measured 24.85 dB PSNR at the midpoint versus
20.71 dB for a plain blend; at t=0.1 and t=0.9 it measured 24.48 and 24.60 dB,
respectively. These are specific to the test clips and GPU; they do not prove
240 fps presentation in the application. An automated app run had invalid
100×30 UI video geometry and is not a valid fullscreen presentation test.
On that synthetic pair at t=0.2, 720p reusable flow measured 24.01 dB, while
Maximum's selective exact inference measured 24.64 dB. Its three-run 4K
median without a refinement was 24.76 ms; an individual refined synthetic
pair took up to 38.48 ms, within one 24 fps source interval on this GPU.

The player overlay reports generated fps, frames actually swapped by Qt,
effective target fps, drops per second, repeated swaps per second, and p95
source-to-swap latency.
The older `presentedPerSec` counter means frames selected by the presentation
timer, which can exceed what Qt displays when repaint requests coalesce. The
timer no longer posts repeated zero-delay wakeups before 240 Hz ticks.

The fast target is capped to the primary screen's refresh rate. If sustained
presentation pressure remains after dropping to 360p analysis, the mode halves
its effective output target; it waits for 20 seconds of clean swaps and GPU
headroom before restoring the full target. This can make a 240 fps selection
run at 120 fps temporarily. The overlay shows the effective rate.

The mode reads its existing 256-sample scene-difference result once per source
pair. For nearly identical frames it repeats the nearer real frame; across a
hard cut it does the same. In either case it skips the RIFE network entirely.
At 3840×2160 on the local RX 9070 XT, the static test pair took 2.52 ms and
the hard-cut pair 2.96 ms, versus 20.58 ms for a moving pair (medians of three,
three, and five runs). The benchmark now reports how many pairs were skipped
or refined. The controller batches D3D output-copy flushes and counts only
outputs that actually obtained display slots.

An automated live run with a 1360×840 player and 3840×2372 source-sized FRC
input settled at roughly 118–119 Qt swaps/s with a 120 fps adaptive target,
after it could not sustain 240 in that window. This validates the fallback,
not stable 4K/240 presentation. The player's initial video geometry now fills
its root area until the web controls report their rectangle, which also keeps
the invalid 100×30 startup state out of the FRC path.

Direct DXGI flip presentation remains a separate rendering-path experiment:
the video uses a `QOpenGLWidget` composited underneath transparent Qt WebEngine
controls. A standalone DXGI swapchain would bypass that composition, so the
experimental mode uses Qt's existing swap path while measuring its actual rate.

To repeat the isolated 4K throughput test, run `frc_bench interp rife-reuse`
with a 3840×2160 A/B pair, `--analysis 540 --fps 240 --time 0.5 --repeat 5`,
and the RIFE v4.26 model directory. `LAMBDA_FRC_AUTOSTART=generic-fast` enables
the menu mode after media loads for local runtime diagnostics. The option is
experimental and has not been released.
