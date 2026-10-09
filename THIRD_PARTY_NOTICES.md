# Third-party components

- **XenonRecomp** (thirdparty/XenonRecomp, submodule; local changes in
  patches/xenonrecomp-mcla.patch) and **XenosRecomp** (thirdparty/XenosRecomp, submodule):
  https://github.com/hedge-dev - see their LICENSE files.
- **DirectX Shader Compiler** (`dxcompiler.dll`, `dxil.dll` in the release, from
  XenosRecomp's dxc-bin): https://github.com/microsoft/DirectXShaderCompiler - University
  of Illinois/NCSA Open Source License; dxil.dll under Microsoft's redistribution terms.
- **FFmpeg** (`ffmpeg\*.dll` in the release: avcodec, avutil, swresample, with libx264 and
  zlib), the build shipped with OBS Studio. It is licensed under the GNU GPL v2 or later
  (libx264 makes the build GPL). Source code: https://ffmpeg.org/download.html and
  https://github.com/obsproject/obs-deps. The port loads it at runtime for the game's
  XMA2 audio.
- Xenia (https://github.com/xenia-project/xenia, BSD license) served as the reference for
  Xbox 360 GPU and audio behavior.
