"""Run with the packaged vapoursynth/python.exe; requires its RIFE/Vulkan runtime.

Usage: _local/app/vapoursynth/python.exe tests/rife/color_runtime.py [rife-dir]
Checks concurrent real-plugin frames, including original and generated frames.
"""
from pathlib import Path
import runpy
import sys

import vapoursynth as vs

repo = Path(__file__).resolve().parents[2]
runtime = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else repo / "_local/app/rife"
core = vs.core

for matrix, limited, luma in [(1, True, 64), (6, False, 100)]:
    clip = core.std.BlankClip(width=128, height=72, format=vs.YUV420P8,
                              length=12, fpsnum=30, color=[luma, 128, 128])
    clip = core.std.SetFrameProps(clip, _ColorSpace=matrix, _Range=0 if limited else 1,
                                  _DurationNum=1, _DurationDen=30)
    runpy.run_path(str(repo / "resources/rife/rife.vpy"), init_globals={
        "video_in": clip, "container_fps": 30, "user_data": "double|" + str(runtime),
    })
    output = vs.get_output(0).clip
    pending = [output.get_frame_async(n) for n in range(10)]
    for request in pending:
        frame = request.result()
        assert frame.props["_Range"] == (0 if limited else 1), dict(frame.props)
        assert frame.props["LambdaMatrix"] == matrix, dict(frame.props)
        assert abs(int(frame[0][20, 20]) - luma) <= 2, (matrix, limited, int(frame[0][20, 20]))
        assert frame.props["_DurationDen"] / frame.props["_DurationNum"] == 60
    print(f"PASS: concurrent 60 fps frames, matrix={matrix}, limited={limited}")
