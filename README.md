# Midnight Club: Los Angeles - native PC port

A native Windows build of the Xbox 360 game *Midnight Club: Los Angeles - Complete Edition*
(title ID 545407F8), made by statically recompiling the game's PowerPC code to x64 with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and running it on a custom runtime
(kernel, audio, input) with a Direct3D 12 renderer that translates the Xenos GPU's shaders.

**No game files are included.** You need your own copy of the game.

## Playing

1. Download the `release/Midnight Club Los Angeles` folder.
2. Copy the files from your Xbox 360 disc of the Complete Edition into its `game\` folder,
   so that `game\default.xex` exists (`tools/xdvdfs.py` extracts them from a disc image).
3. Run `Midnight Club Los Angeles.exe`. Settings are in `MCLA.ini`; see `README.txt` in
   that folder for controls, fullscreen and known issues.

Requires Windows 10/11 x64 and a GPU with Direct3D 12 (shader model 6.0). An Xbox
controller is recommended; keyboard controls are listed in the release README.

## Building

Requirements: Visual Studio 2022 Build Tools with the C++ Clang component, CMake, Ninja
(both come with the Build Tools) and Python 3.11+.

```
git clone --recursive https://github.com/EnvyMachine/MCLA_PC_PORT.git
cd MCLA_PC_PORT
git -C thirdparty/XenonRecomp apply ../../patches/xenonrecomp-mcla.patch
python tools/xdvdfs.py <disc.iso> extract game
mkdir extracted
copy game\default.xex extracted\
python tools/xex.py extracted/default.xex dump extracted/default.pe
powershell -ExecutionPolicy Bypass -File tools\build_xenonrecomp.ps1
powershell -ExecutionPolicy Bypass -File tools\recompile.ps1
powershell -ExecutionPolicy Bypass -File tools\build_mcla.ps1
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1
```

`recompile.ps1` generates `ppc/` (the recompiled game code) from your `default.xex`; it is
not part of this repository. `notes/PROGRESS.md` documents how the port works, the
problems found along the way and the debugging options.

## Status

Playable: free roam, races, career, audio and controllers work at the game's native
30 fps. Open issues are listed in `release/Midnight Club Los Angeles/README.txt`.

Midnight Club: Los Angeles is a trademark of Rockstar Games / Take-Two Interactive. This
project is not affiliated with them.
