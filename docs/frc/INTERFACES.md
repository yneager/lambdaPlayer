# Post-render FRC interface audit

Audit baseline: local commit `df10dff` (`codex/amf-frc-baseline`), 2026-09-25. This documents the current AMD FRC integration before adding another backend. Keep this file updated when the shared frame contract changes.

## Frame and device path

1. `MpvVideoWidget` is a `QOpenGLWidget`. mpv is initialized with its OpenGL render API (`vo=libmpv`); there is no video child HWND and the Chromium controls are a separate Qt-composited layer above the widget.
2. mpv's update callback may arrive on an mpv thread. `onMpvUpdate` coalesces it and queues `requestFrame()` to the Qt GUI thread. `requestFrame()` consumes `mpv_render_context_update()` and, when FRC is active, makes the widget GL context current.
3. `ScreenInterpolationController::ensureDevice()` creates one D3D11 device. It attempts to choose the adapter whose DXGI name matches `GL_RENDERER`, then enables `ID3D11Multithread` protection because the device is shared across GUI and worker use. It initializes AMF on that device.
4. `WglDxInterop` opens the D3D11 device with the current WGL context using `WGL_NV_DX_interop2`. For a D3D11 `ID3D11Texture2D`, it creates/caches a GL texture and FBO registration. GL work must use the context that owns this registration.
5. On the GUI thread, `captureFrame()` obtains an AMF-pool `AMFSurface` in `AMF_MEMORY_DX11` / BGRA8. It locks the WGL registration, asks mpv to render its final picture into that FBO, unlocks it, timestamps the capture, then queues the AMF surface to the worker. No CPU pixel readback occurs.

The post-render input is the image mpv has already scaled and color-converted for the widget, with mpv subtitles/OSD included. Chromium controls are outside the widget and therefore excluded. `LAMBDA_FRC_INPUT=source` makes the capture larger to the decoded video dimensions; the normal default is widget physical pixels. The current GL↔D3D interop is runtime-probed and is not guaranteed on every OpenGL driver, even if that machine otherwise supports D3D11 compute.

## Ownership, threads, and synchronization

- GUI thread + widget GL context: mpv render into registered input FBO; WGL lock/unlock/register/release; initialize/destroy GL-facing display views; GL blit during `paintGL`; presentation timers and mpv audio-delay changes.
- FRC worker: AMF component initialize/terminate/flush and `SubmitInput`/`QueryOutput`; D3D `CopyResource` of outputs into a free display-ring texture; queue output metadata back to the GUI thread. AMF surfaces are reference-counted smart pointers carried by jobs and remain alive through processing.
- `jobMutex_` / `jobCv_` serialize work submission. `slotMutex_` protects the D3D display texture ring shared by paint and worker. WGL lock/unlock is the GL/D3D interop handoff. `ID3D11Multithread` is enabled on the D3D device. A generation number invalidates queued jobs/results after seek, reset, or reconfiguration.
- The AMF output and the display-ring slot are different resources. AMF returns pooled output surfaces; worker copies each output into an owned BGRA8 display slot before releasing the AMF result. The ring slots move through Free, Queued, and Shown states. The presenter keeps the newest scheduled image in `shownSlot_` while queued future images wait in `queue_`.
- During startup or reset, `lastInput_` can be displayed until a generated output arrives. When interpolation is inactive, `paintGL()` calls mpv directly into Qt's widget framebuffer.

## Timestamps, presentation, and audio

- `captureTimes_` is a monotonic-nanosecond ring indexed by a monotonically increasing source sequence (`seq_`). That sequence is also submitted as the AMF surface PTS and represented as the content coordinate (in source-frame units).
- AMF's `FRC_x2_PRESENT` yields a generated midpoint marked by `AMF_REPEAT` and a source output marked by `AMF_OK`. The current backend derives content coordinates from a history of submitted sequence values; for the default setting, its measured effective content hold is one source frame (two when future-frame mode is enabled). Do not use AMF PTS alone as the visible content timestamp; the standalone moving-square proof found those PTS did not describe actual image content time.
- `intendedNs(content)` maps source-frame positions to capture times. `onOutput()` measures time from intended source time until the output is available on the GUI thread. A rolling 99th-percentile estimate plus 3 ms becomes presentation latency after warm-up. Each output is assigned to the nearest output-rate tick; `presentDue()` chooses the best available slot for the due tick and slightly phase-corrects the epoch.
- `ScreenInterpolationController::applyAudioDelay()` records mpv's original `audio-delay`, adds the measured picture latency while running, and restores the original value on disable, failure, and teardown. This is deliberately coupled to actual presentation latency.
- Seek, pause/resume, file change, resize, and target-rate changes flush or reconfigure FRC state and reset the timeline/generation so stale frames are not presented across a discontinuity.
- The overload watchdog observes captured/presented rates and late outputs; after sustained failure it disables the path, restores audio delay, and continues native playback.

## License and source-reuse boundary

There is no root project `LICENSE`/`COPYING` file in this checkout. Do not assume the desktop application grants permission to incorporate copyleft implementation code. VipleStream's repository declares GPL-3.0 for its Moonlight-Qt client and its Generic FRUC files live inside that repository. HopperRender is GPL-3.0; its mpv integration is also distributed under GPL/LGPL terms. MPC Video Renderer is GPL-3.0. Their implementations are research references only for this integration unless the product owner separately decides to change distribution/licensing.

The generic prototype should independently implement the published block-matching/SAD + vector-field warp/blend technique using D3D11 and HLSL, without copying source, comments, constants chosen by the original implementation, or shader text. Keep references and any independent design notes here. AMD AMF's headers/sample remain separately licensed MIT and are already vendored with notices; the existing AMF backend is preserved as a selectable baseline.

## Upstream references

- VipleStream Generic FRUC and shaders: <https://github.com/finaltwinsen/VipleStream> (repository GPL-3.0; exact paths named in the task).
- HopperRender and mpv integration: <https://github.com/HopperLogger/HopperRender> and <https://github.com/HopperLogger/mpv-frame-interpolator> (GPL-3.0 / GPL-2.0 and LGPL-2.1 licensing as declared by those repositories).
- VideoProcessor usage reference: <https://github.com/Aleksoid1978/VideoRenderer> (GPL-3.0; inspect patterns only).
- Microsoft D3D11 FRC documentation: `D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_FRAME_RATE_CONVERSION`, `GetVideoProcessorRateConversionCaps`, and `VideoProcessorSetStreamOutputRate`.
