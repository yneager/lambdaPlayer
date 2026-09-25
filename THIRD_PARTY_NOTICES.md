# Third-Party Notices

LAMBDA Player bundles the open-source components below in the portable Windows
package (`LAMBDA Player-Windows-x64`). The verbatim license texts are shipped in
the `licenses/` directory of the package and of this repository. Upstream
copyright notices are preserved unmodified.

All versions listed here are pinned in `.github/workflows/windows-portable.yml`
and `.github/scripts/assemble-rife-runtime.ps1` and are verified by SHA-256 at
build time. Change them deliberately and update this file when you do.

## Playback core (pre-existing)

| Component | Version | License | Files in package |
|-----------|---------|---------|------------------|
| Qt 6 (Widgets, Gui, Core + platform plugins) | 6.8.3 (MSVC 2022 x64) | LGPL-3.0 (dynamic linking) | `Qt6*.dll`, `platforms/`, `styles/`, … |
| mpv / libmpv (shinchiro/mpv-winbuild-cmake build) | release `20260923`, mpv git `6fd80b2003` (v0.41.0-1055), asset `mpv-dev-x86_64-20260923-git-6fd80b2003.7z` | GPL-2.0-or-later (mpv core; this is a `gpl`-enabled build that also bundles GPL FFmpeg components, so the DLL as a whole is distributed under the GPL). Client API headers: ISC. | `libmpv-2.dll` |
| Microsoft Visual C++ 2015-2022 runtime | from the CI runner's Visual Studio 2022 redistributable | Microsoft Visual Studio redistributable terms (app-local deployment permitted) | `msvcp140*.dll`, `vcruntime140*.dll`, `concrt140.dll` |

License texts: `licenses/mpv-LICENSE.GPL.txt`, `licenses/mpv-Copyright.txt`.
The complete corresponding source for the libmpv build is the
`shinchiro/mpv-winbuild-cmake` release `20260923` (build scripts) and the mpv
git commit `6fd80b2003`; LAMBDA Player links libmpv dynamically. Qt's LGPL text
is available at <https://www.qt.io/licensing/open-source-lgpl-obligations>.

## Frame interpolation runtime (added in the RIFE 2x milestone)

| Component | Version / commit | License | Files in package |
|-----------|------------------|---------|------------------|
| Python (embeddable distribution) | 3.12.10, `python-3.12.10-embed-amd64.zip` | Python Software Foundation License 2.0 | `vapoursynth/python*.dll`, `vapoursynth/python312.zip`, `vapoursynth/*.pyd`, `vapoursynth/python.exe`, … |
| VapourSynth | R80, `VapourSynth64-Portable-R80.zip` (wheel `vapoursynth-80-cp312-abi3-win_amd64.whl`) | LGPL-2.1-or-later (dynamically loaded by libmpv, unmodified) | `vapoursynth/Lib/site-packages/vapoursynth/` (`vsscript.dll`, `libvapoursynth.dll`, `libvapoursynthfilters*.dll`, `vapoursynth.pyd`, `vspipe.exe`, `plugins/avscompat.dll`, …) |
| VapourSynth-RIFE-ncnn-Vulkan (styler00dollar fork of HolyWu's plugin) | tag `r9_mod_v33`, commit `c3ec6aabc07c8fa37a4f58d7fed9e2ad1fc1b13f`, asset `librife_windows_x86-64.dll` | MIT (Copyright (c) 2021-2022 HolyWu) | `rife/librife_windows_x86-64.dll` |
| rife-ncnn-vulkan (nihui) | inference code embedded in the plugin above | MIT (Copyright (c) 2020 nihui) | (statically linked inside `rife/librife_windows_x86-64.dll`) |
| ncnn (Tencent) | ncnn submodule of the plugin at the commit above | BSD-3-Clause, plus the third-party components listed in `ncnn-LICENSE.txt`; ncnn's Vulkan path also embeds glslang (BSD-3-Clause / MIT / Apache-2.0 components, see <https://github.com/KhronosGroup/glslang/blob/main/LICENSE.txt>) | (statically linked inside `rife/librife_windows_x86-64.dll`) |
| RIFE v4.6 model (`rife-v4.6_ensembleFalse`, converted to ncnn by the plugin authors) | files `flownet.bin`, `flownet.param` from the plugin repository at commit `c3ec6aab…` (plugin model index 23) | MIT (RIFE, Copyright (c) Megvii Inc. / hzwer, <https://github.com/hzwer/ECCV2022-RIFE>) | `rife/models/rife-v4.6_ensembleFalse/` |
| vs-miscfilters-obsolete (`misc.SCDetect`) | release R2, asset `miscfilters-r2.7z` (`win64/MiscFilters.dll`) | LGPL-2.1 | `rife/MiscFilters.dll` |
| mpv-RIFE (Lafourkad) | `rife.vpy` structure adapted from the `main` branch | MIT (Copyright (c) 2026 Lafourkad) | attribution in `rife/rife.vpy` |

License texts: `licenses/Python-LICENSE.txt`, `licenses/VapourSynth-COPYING.LESSER.txt`,
`licenses/VapourSynth-RIFE-ncnn-Vulkan-LICENSE.txt`, `licenses/rife-ncnn-vulkan-LICENSE.txt`,
`licenses/ncnn-LICENSE.txt`, `licenses/RIFE-LICENSE.txt`,
`licenses/vs-miscfilters-obsolete-LICENSE.txt`, `licenses/mpv-RIFE-LICENSE.txt`.

`president-not-sure/mpv-interpolation` (GPL-2.0) was studied for ideas only; no
code from it was copied into LAMBDA Player.

## System requirements introduced by the interpolation runtime

* A Vulkan-capable GPU with a vendor driver that provides `vulkan-1.dll`
  (NVIDIA, AMD or Intel). The Vulkan SDK is not required.
* Windows 10/11 x64. No separate Python, VapourSynth, RIFE or ncnn installation
  is needed; everything is inside the portable package.


## Inter

- Component: Inter variable font
- Upstream: https://github.com/rsms/inter
- Pin: commit `353b61b9f4430d5f420d56605a6e7993e0941470`
- Bundled file: `InterVariable.woff2`
- SHA-256: `693B77D4F32EE9B8BFC995589B5FAD5E99ADF2832738661F5402F9978429A8E3`
- License: SIL Open Font License 1.1
- License copy: `licenses/Inter-LICENSE.txt`

## Streaming engine (added in v0.2.5)

| Component | Version / commit | License | Files in package |
|-----------|------------------|---------|------------------|
| stremio-native/stream-server | commit `f585ab6` (pinned in `tools/stream-server/versions.json`), built from source with LAMBDA's host and patches in `tools/stream-server/` | MIT | `lambda-stream-server.exe`, `licenses/stream-server-LICENSE.txt` |
| libtorrent-rasterbar, OpenSSL, Boost, UnRAR source, 7-Zip/LZMA SDK and Rust crates (statically linked into the engine) | as resolved by the pinned vcpkg commit and upstream `Cargo.lock` | BSD-3-Clause and others / Apache-2.0 / BSL-1.0 / UnRAR license / LGPL-2.1 | see `licenses/THIRD-PARTY.txt` |

## AMD FRC frame generation (post-render interpolation)

| Component | Version / commit | License | Where |
|-----------|------------------|---------|-------|
| AMD Advanced Media Framework SDK (headers `amf/public/include/**`, `amf/public/common/{AMFFactory,AMFSTL,Thread,TraceAdapter}.*`, `Windows/ThreadWindows.cpp`) | GPUOpen-LibrariesAndSDKs/AMF `6277e353fd625121a8f627b1d0540323ef372a49` | MIT, © Advanced Micro Devices, Inc. | copied unmodified to `third_party/amf/`, compiled into `LambdaPlayer.exe`; license `licenses/AMF-LICENSE.txt` |
| AMD SimpleFRC sample (`amf/public/samples/CPPSamples/SimpleFRC/SimpleFRC.cpp`) | same commit | MIT, © 2024 Advanced Micro Devices, Inc. | FRC setup and the SubmitInput/QueryOutput/resubmit loop adapted in `src/frc/amffrcinterpolator.cpp` and `tools/frc-proof/frcproof.cpp` |

The AMF runtime itself (`amfrt64.dll`) is part of the AMD graphics driver and is
not redistributed; on other GPUs the option is shown as unavailable. The
Windows.Graphics.Capture samples (robmikh/Win32CaptureSample,
microsoft/Windows.UI.Composition-Win32-Samples, both MIT) were studied but no
code from them is used; see `docs/frc/RESEARCH.md`.
