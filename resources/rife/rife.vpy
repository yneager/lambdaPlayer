# LAMBDA Player RIFE frame-interpolation script for mpv's `vapoursynth` video filter
# (modes: double the frame rate, or 60 fps).
#
# Pipeline:  libmpv decode -> mpv vapoursynth filter -> this script
#            -> VapourSynth-RIFE-ncnn-Vulkan (ncnn/Vulkan) -> libmpv rendering
#
# This script performs no interpolation itself. It only wires mpv's `video_in`
# clip into the existing RIFE VapourSynth plugin and returns the result.
#
# Structure adapted from the MIT-licensed mpv-RIFE project
# (https://github.com/Lafourkad/mpv-RIFE, Copyright (c) 2026 Lafourkad) and the
# VapourSynth-RIFE-ncnn-Vulkan plugin documentation
# (https://github.com/styler00dollar/VapourSynth-RIFE-ncnn-Vulkan, MIT).
# All machine-specific paths were removed; every path is resolved relative to the
# runtime directory that LAMBDA Player passes in through mpv's `user-data` sub-option
# (falling back to the directory that contains this script).
#
# Runtime directory layout expected next to this script:
#   librife_windows_x86-64.dll           VapourSynth-RIFE-ncnn-Vulkan plugin
#   MiscFilters.dll                      vs-miscfilters-obsolete (misc.SCDetect)
#   models/rife-v4.6_ensembleFalse/      RIFE v4.6 model (flownet.bin/.param)

import math
import os
from fractions import Fraction

import vapoursynth as vs

core = vs.core
# Bound CPU conversion/scene-detection work so decode, audio and Qt retain
# scheduling headroom. GPU inference keeps the plugin's two-worker default.
core.num_threads = max(1, min(8, (os.cpu_count() or 2) - 2))

RIFE_PLUGIN_FILE = "librife_windows_x86-64.dll"
MISC_PLUGIN_FILE = "MiscFilters.dll"
# Model folder name as shipped by the pinned plugin release (r9_mod_v33).
# This is the plugin's model index 23 ("rife-v4.6 (ensemble=False)").
RIFE_MODEL_DIR = os.path.join("models", "rife-v4.6_ensembleFalse")

# misc.SCDetect threshold used by mpv-RIFE; frames flagged as scene changes are
# duplicated by the plugin instead of being interpolated across the cut.
SCENE_CHANGE_THRESHOLD = 0.1

# Modes selected by LAMBDA Player through mpv's `user-data` sub-option, formatted
# as "<mode>|<runtime dir>":
#   double : 2x the source frame rate (24 -> 48, 30 -> 60, ...)
#   60     : 60 fps output for sources below 60 fps
MODE_DOUBLE = "double"
MODE_60 = "60"
TARGET_60 = Fraction(60)

# Standard rates used to clean up container frame rates that are slightly off
# (e.g. a 24 fps MP4 reported as 24.1 fps because of its timebase).
_STANDARD_RATES = [Fraction(24000, 1001), Fraction(24), Fraction(25), Fraction(30000, 1001),
                   Fraction(30), Fraction(48), Fraction(50), Fraction(60000, 1001), Fraction(60)]


def _parse_user_data():
    raw = globals().get("user_data", "") or ""
    if "|" in raw:
        mode, directory = raw.split("|", 1)
        return mode.strip(), directory
    return MODE_DOUBLE, raw


MODE, USER_DIR = _parse_user_data()


def _source_fps():
    """Source frame rate as reported by mpv (container_fps), snapped to a standard rate."""
    fps = globals().get("container_fps", 0.0) or 0.0
    if not math.isfinite(fps) or fps <= 1.0:
        return None
    nearest = min(_STANDARD_RATES, key=lambda rate: abs(fps - float(rate)))
    if abs(fps - float(nearest)) / float(nearest) < 0.005:
        return nearest
    return Fraction(fps).limit_denominator(1001)


def _factor():
    """RIFE factor_num/factor_den for the selected mode.

    The plugin's fps_num/fps_den options need a clip frame rate, which mpv's
    `video_in` does not carry, so the target is expressed as a factor instead.
    Output timing still comes from mpv's per-frame durations (the plugin
    scales them by factor_den/factor_num), so A/V sync is unaffected.
    """
    if MODE == MODE_DOUBLE:
        return 2, 1
    if MODE == MODE_60:
        fps = _source_fps()
        if fps is None:
            raise vs.Error("60 fps mode needs a known video frame rate, but this file does not report one.")
        if fps >= Fraction(59):
            raise vs.Error(f"60 fps mode is only for videos below 60 fps (this video is {float(fps):.3f} fps).")
        factor = TARGET_60 / fps
        return factor.numerator, factor.denominator
    raise vs.Error(f"Unknown LAMBDA Player RIFE mode: {MODE!r}")


def _runtime_dir():
    """Return the directory that holds the plugin DLLs and models."""
    candidates = []
    if USER_DIR:
        candidates.append(USER_DIR)
    script_file = globals().get("__file__")
    if script_file:
        candidates.append(os.path.dirname(os.path.abspath(script_file)))
    for candidate in candidates:
        if os.path.isfile(os.path.join(candidate, RIFE_PLUGIN_FILE)):
            return candidate
    raise vs.Error(
        "LAMBDA Player RIFE runtime not found. Looked in: "
        + (", ".join(candidates) if candidates else "<no candidates>")
    )


def _load_plugin(namespace, path):
    if not hasattr(core, namespace):
        if not os.path.isfile(path):
            raise vs.Error(f"Missing VapourSynth plugin: {path}")
        core.std.LoadPlugin(path)


# mpv's vapoursynth filter fills the `_ColorSpace` frame property with
# libavcodec (ITU-T H.273) matrix codes. zimg (core.resize) accepts the same
# integer codes via `matrix_in`/`matrix`. Anything zimg cannot handle is
# replaced by mpv's own resolution-based guess (width >= 1280 or height > 576
# -> BT.709, otherwise BT.601 / SMPTE 170M).
_ZIMG_MATRICES = {1, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14}


def _guess_matrix(width, height):
    return 1 if (width >= 1280 or height > 576) else 6


class _ColorState:
    """Remembers the source matrix so the RGB->YUV step can undo the RGB conversion."""

    def __init__(self, width, height):
        self.default_matrix = _guess_matrix(width, height)
        self.matrix = self.default_matrix
        self.limited_range = True

    def update_from_props(self, props):
        matrix = props.get("_ColorSpace", 2)
        if matrix not in _ZIMG_MATRICES:
            matrix = self.default_matrix
        self.matrix = matrix
        color_range = props.get("_Range")
        if color_range in (0, 1):
            # R80 uses _Range: 0 = limited, 1 = full. Reading the deprecated
            # _ColorRange through .get() does not invert the enum in R80.
            self.limited_range = color_range == 0
        elif "_ColorRange" in props:
            self.limited_range = props["_ColorRange"] == 1
        return self.matrix


def build(clip):
    if clip.format is None:
        raise vs.Error("LAMBDA Player RIFE: variable-format input is not supported")
    if clip.format.color_family != vs.YUV:
        raise vs.Error("LAMBDA Player RIFE: only YUV input from mpv is supported")

    runtime_dir = _runtime_dir()
    _load_plugin("misc", os.path.join(runtime_dir, MISC_PLUGIN_FILE))
    _load_plugin("rife", os.path.join(runtime_dir, RIFE_PLUGIN_FILE))

    model_path = os.path.join(runtime_dir, RIFE_MODEL_DIR)
    for model_file in ("flownet.bin", "flownet.param"):
        if not os.path.isfile(os.path.join(model_path, model_file)):
            raise vs.Error(f"Missing RIFE model file: {os.path.join(model_path, model_file)}")

    source_format = clip.format.id

    # Scene-change detection must run on the YUV clip (plugin requirement).
    # The resulting _SceneChangeNext property survives the RGB conversion.
    clip = core.misc.SCDetect(clip, threshold=SCENE_CHANGE_THRESHOLD)

    # YUV -> 32-bit float RGB (the only input format the RIFE plugin accepts).
    # The matrix is read per frame from mpv's `_ColorSpace` property; zimg reads
    # `_ColorRange`/`_ChromaLocation` from the frame properties itself.
    to_rgb_cache = {}

    def to_rgb(n, f):
        state = _ColorState(clip.width, clip.height)
        matrix = state.update_from_props(f.props)
        key = (matrix, state.limited_range)
        converted = to_rgb_cache.get(key)
        if converted is None:
            converted = core.resize.Bicubic(clip, format=vs.RGBS, matrix_in=matrix)
            # RGB conversion changes _ColorRange. Carry source colour metadata
            # on each frame so concurrent output requests can restore it.
            converted = core.std.SetFrameProps(converted, LambdaMatrix=matrix,
                                               LambdaLimited=int(state.limited_range))
            to_rgb_cache[key] = converted
        return converted

    rgb_template = core.std.BlankClip(clip, format=vs.RGBS, keep=True)
    rgb = core.std.FrameEval(rgb_template, to_rgb, prop_src=clip)

    # Interpolation with the existing RIFE plugin (model_path pins v4.6).
    # GPU selection is left to the plugin/ncnn default (ncnn::get_default_gpu_index).
    factor_num, factor_den = _factor()
    interpolated = core.rife.RIFE(
        rgb,
        model_path=model_path,
        factor_num=factor_num,
        factor_den=factor_den,
        sc=True,
    )

    # RGB -> the original YUV format so mpv keeps its normal rendering path
    # (bit depth, subsampling and range are restored; HDR metadata is untouched
    # because mpv reattaches the original image parameters to the output).
    to_yuv_cache = {}

    def to_yuv(n, f):
        # Frame requests run concurrently: do not read mutable colour state
        # left behind by a different (possibly future) input frame.
        matrix = f.props.get("LambdaMatrix", _guess_matrix(clip.width, clip.height))
        limited_range = bool(f.props.get("LambdaLimited", 1))
        key = (matrix, limited_range)
        converted = to_yuv_cache.get(key)
        if converted is None:
            converted = core.resize.Bicubic(
                interpolated,
                format=source_format,
                matrix=matrix,
                range=0 if limited_range else 1,
            )
            to_yuv_cache[key] = converted
        return converted

    yuv_template = core.std.BlankClip(interpolated, format=source_format, keep=True)
    return core.std.FrameEval(yuv_template, to_yuv, prop_src=interpolated)


if "video_in" not in globals():
    raise vs.Error("LAMBDA Player RIFE: this script must be loaded by mpv's vapoursynth filter")

build(video_in).set_output()
