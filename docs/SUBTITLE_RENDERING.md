# Subtitle rendering in 0.2.9

The reported screenshot showed intact subtitle/UI text with a corrupted, nearly
black video image after subtitle selection on an NVIDIA PC. The exact driver
failure is not confirmed locally.

libmpv's bundled render_gl.h requires standard OpenGL state before its API calls.
MpvVideoWidget now clears pixel unpack/pack buffer bindings and transfer strides,
resets blend/scissor/color-write state, and prepares the context before creation
and rendering. tests/tst_mpvglstate.cpp dirties these states, uploads a video
texture and reads it back exactly. The native Windows run passed on the local
RX 9070 XT; this validates transfer-state isolation, not an RTX 5080 reproduction.

On an NVIDIA adapter, choosing an embedded, local or addon subtitle also switches
an active d3d11va/nvdec direct-sharing decoder to auto-copy. Hardware decoding
continues, but frame transfer avoids the direct decoder/OpenGL texture-sharing
path. Already copied or software decoders are left as selected.

Test on the affected PC: play the same video in Original mode, select the same
subtitle, switch subtitle tracks and disable/re-enable subtitles. Repeat with
RIFE x2/60. Video, colors and subtitles should remain visible without restarting.
