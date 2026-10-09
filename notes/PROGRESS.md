# MCLA static recompilation — progress log

Project root: `C:\dev\mcla-recomp` (kept out of OneDrive on purpose).
Disc image: `C:\Users\trevo\OneDrive\Desktop\PC_PORT\Midnight Club - Los Angeles - Complete Edition (USA, Europe)\*.iso`

## Phase 1: binary analysis (done, 2026-10-06)

Disc: XGD2, game partition at 0xFD90000. Files: `default.xex` (9.25 MB), 6 Bink
videos, `nxeart`, and four RAGE archives (`xarchive_audio/audlo/cache/music.rpf`).
Full listing in `disc_listing.txt`.

Executable (`tools/xex.py extracted/default.xex info`):
- Original name `mc4_xenon_final.exe`, title ID 545407F8, XDK 2.0.6995
- AES-encrypted, basic (zero-run) compression; decrypts to a valid PowerPC BE PE
  (`extracted/default.pe`, loaded at 0x82000000, entry 0x821322B8)
- `.text` 0x82130000-0x827BDCE4 (6.6 MB) + `BINK` section (Bink video, statically linked)
- `.pdata` lists 20,071 functions, covering 91% of `.text`
- Imports: 246 functions + 11 variables from xboxkrnl.exe and xam.xex (`imports.txt`)

Recompiler facts (`config/MCLA.toml`):
- All 8 register save/restore helpers found (unique matches)
- No setjmp/longjmp linked
- No C++ exceptions; 31 functions use C SEH via `__C_specific_handler` (0x827BCF04)
- ~570 likely jump tables (indexed load + mtctr + bctr)

Hard parts identified:
- **Graphics**: D3D9 is statically linked and drives the GPU directly through Vd*
  (ring buffer, system command buffer, VdSwap). Either emulate the Xenos command
  processor (Xenia-style) or locate and hook the game's D3D functions
  (Unleashed Recompiled-style). Hooking is faster at runtime but means identifying
  unsymbolized D3D code.
- **Audio**: XMACreateContext/XAudio* → hardware XMA decoding has to become
  software decoding.
- RAGE `.rpf` archive I/O goes through NtCreateFile/NtReadFile etc.; needs a VFS
  mapping onto extracted disc files.

Static-library landmarks (callers of Vd*/XMA*/XAudio* imports):
- D3D9/video cluster ~0x82412A60-0x8242E710
  - sub_82412A60: ring buffer init (VdInitializeRingBuffer, RPtr writeback)
  - sub_82419E98: present path (VdSwap, VdGetSystemCommandBuffer, VdPersistDisplay)
  - sub_82426468 / sub_82426840: engine init/shutdown, graphics interrupt callback
  - sub_82426360: VdSetDisplayMode; sub_8242C570: EDRAM retrain
- Audio cluster ~0x823EC248-0x823F3908
  - sub_823F3718/3840/3908: XAudio render driver register/unregister/submit frame
  - sub_823EDCD8 / sub_823ED3C8: XMA context create / release
  - sub_823EC248: voice category volumes

RTTI is intact (`tools/rtti.py` -> `vtables.txt`): 1722 classes (698 in `rage::`),
1697 vtables, 56,213 virtual slots. Better hook points than raw D3D:
- File system: `rage::fiDeviceLocal` (vtable 0x8201219C), `rage::fiPackfile`
  (0x82012BDC, RPF reader), `rage::fiCachedDevice`. Paths seen: `game:/xarchive_*.rpf`,
  `cache:\$cache$\...`, `$/preload/*.rpf`, DLC `%s:\xarchive_dlc_%c.rpf`.
- Graphics: `rage::grcTextureFactoryXenon` (0x8200A3B4), `grcRenderTargetXenon`
  (0x8200A524), `grcTextureXenon`, `grcVertexBufferD3D`, `grcIndexBufferD3D`, `grmShaderFx`.
- sub_82130000 (first function in .text) is an empty `blr`; used for no-op virtual slots.

## Phase 2: toolchain + first recompilation (done, 2026-10-06)

Toolchain: VS 2022 Build Tools 17.14 + clang-cl, CMake, Ninja.
- `tools/build_xenonrecomp.ps1` builds XenonRecomp/XenonAnalyse into `build/XenonRecomp`
- `tools/build_mcla.ps1` builds the translated game code (`MCLARecompLib`)

Pipeline (re-run after any config/recompiler change):
1. `build\XenonRecomp\XenonAnalyse\XenonAnalyse.exe extracted\default.xex config\MCLA_switch_tables.toml`
2. `build\XenonRecomp\XenonRecomp\XenonRecomp.exe config\MCLA.toml thirdparty\XenonRecomp\XenonUtils\ppc_context.h`
3. `python tools\fix_switch_functions.py` if step 2 logs "Switch case ... outside function", then step 2 again
4. `tools\build_mcla.ps1`

Results: 527 jump tables; 31,124 functions; 125 C++ files (~122 MB).
Recompiler log went from 1,830 messages to 0, and generated `__builtin_debugtrap()`s from 96 to 0:
- 1,571 switch-boundary errors: leaf functions with no .pdata entry were cut at the
  first bctr. `tools/fix_switch_functions.py` derives real extents (forward branches +
  jump-table labels, including backward labels) → 98 explicit `functions` in MCLA.toml.
- XenonRecomp changes (`patches/xenonrecomp-mcla.patch`, apply to upstream ddd128b):
  - Disassembler: dropped PPC_OPCODE_403 from the default dialect. It enabled the PPC405
    multiply-accumulate family, which shadowed VMX128 encodings (0x1260E311 vsldoi128 was
    decoded as `mullhwu.`). Verified: only that one instruction changed in the output.
  - New instructions: frsqrte, mulhdu, eqv, dcbst (no-op), vsel128, vspltish, vrfip(128),
    vpkswss(128), vpkuwus(128), vpkuwum(128), vpkuhum(128), vctuxs/vcfpuxws128, vcmpbfp(128).
  - vpkd3d128 rewritten: pack into a temporary, then merge lanes with Xenia's permute
    table (all pack/shift forms); added NORMPACKED32 2_10_10_10 and FLOAT16_2. Upstream
    silently emitted debugtrap for these types.
  - vupkd3d128: added FLOAT16_4 (94 sites) and NORMPACKED32 (1 site); previously silent traps.
  - Semantics checked against Xenia Canary sources saved in `thirdparty/reference/`.
  - Not yet verified against real hardware or Xenia's PPC test suite (XenonTests).

## Phase 3: runtime (in progress, 2026-10-06)

Status (2026-10-07): the game runs stably (70+ s tested, no crashes) at ~27-30 fps of
game logic, issuing 3,800-5,300 draws/frame by ~30 s (3D city, likely the attract
sequence). No picture yet: draws are parsed and skipped. Audio engine runs on silent
XMA output. Next milestone: the renderer.

Fixes that got it here (each was a hard stall or crash):
- XMA decoder (apu/xma.cpp): register side effects via mid-asm hooks at the only
  kick/lock/clear stores (0x823EE13C/0x823EDE04/0x823EDD6C) and the
  CurrentContextIndex read (0x823EDF34); contexts allocated from the published array;
  Xenia's "fake" decode (consume packets, emit silence).
- CPU numbers: ExCreateThread takes the processor from creation flags bits 24-29
  (else inherits the parent's), KeSetAffinityThread moves PCR current_cpu. XAudio
  workers rendezvous on per-CPU flags; D3D uses per-CPU callback queues.
- IRQL >= DISPATCH masks interrupts on that CPU (per-CPU recursive lock shared with
  interrupt delivery). D3D's per-CPU deferred-callback ring relies on it.
- GPU: WAIT_REG_MEM blocks indefinitely (CPU threads gate the GPU with it);
  predication (SET_BIN_MASK/SELECT, packet bit 0) - executing predicated packets
  unconditionally fired per-tile callbacks/writes repeatedly and corrupted D3D state.
- Diagnostics: MCLA_TRAP_E0=1 (E0 view unmapped to catch use), MCLA_LOG_PHYS=1
  (physical alloc/free log), GPU parse errors print the buffer chain + packet history.

Previous status: boots the game through D3D init and first frames, then stalled on XMA.

Build/run:
- `tools\recompile.ps1`: XenonAnalyse, missed-jump-table pass, XenonRecomp into a
  clean `ppc/`, switch-boundary fixes, import stub generation
- `tools\build_mcla.ps1`: builds `build\mcla-relwithdebinfo\MCLA\MCLA.exe` (lld-link, PDB)
- `tools\run_mcla.ps1 -Seconds N [-Trace] [-DumpAfter S]`: log is `mcla.log` next to the exe.
  `-DumpAfter` logs every guest thread's stack plus the D3D device GPU fields.

Runtime layout (`MCLA/`):
- `runtime/memory.cpp`: 4 GB placeholder reservation. Virtual heaps (4K/64K/system) are
  committed on allocation. One 512 MB section is mapped at 0xA0000000, 0xC0000000 and
  0xE0000000 (aliased like hardware). 4 KB physical allocations come from the A0 view
  because the hardware E0 view is shifted by 0x1000, which D3D hard-codes and Windows
  can't map at 4 KB granularity.
- `runtime/thread.cpp`: PCR/KTHREAD/TLS/stack per guest thread (Xenia layouts), 64 MB host stacks.
- `runtime/kernel.cpp`: handles, guest-memory dispatcher objects, waits, timers, TLS slots.
- `runtime/loader.cpp`: XEX via XenonUtils, function table, import slots (paired by name).
- `runtime/vfs.cpp`: game:/d:/cdrom0 map to `game/` (extracted disc), cache:/hdd:/content to `userdata/`.
- `gpu/command_processor.cpp`: PM4 front end (ring and indirect buffers, register file
  mirrored at 0x7FC80000, fences/events/scratch writeback/waits/interrupts). The
  independent vblank thread is required: the GPU waits on flips that only the vblank
  ISR completes. Interrupts run with PCR current_cpu set to the target CPU (the ISR
  acknowledges by clearing bit 1<<cpu). Draws are parsed and skipped: there is no renderer yet.
- `kernel/*.cpp`: 235 of 246 imports implemented. `generated_stubs.cpp` covers the rest
  (Io*/Stfs*).
- `MCLARecompLib/ppc_overrides.h`: checked indirect calls report missing functions.

Fixes found while booting: 3 jump tables missed by XenonAnalyse (`tools/find_jump_tables.py`);
stale XenonRecomp output files (it never deletes them, so always recompile into a clean folder).

### (Resolved) XMA hardware decoder
Main thread spins in sub_82547140 (audio state triple-buffer full). The consumer thread
(sub_82144888, threads "guest 22/23") waits in sub_82136700 for flag byte 0x828317A1 to
clear. That flag is set/cleared by XAudio pass begin/end callbacks registered through
sub_823EA888. The XAudio worker thread (sub_823EC8C8 -> sub_823EB3E0) polls the XMA
decoder through APU registers at 0x7FEA0000 (reg index*4), which nothing emulates.
Next step: implement the XMA register interface from Xenia
(`thirdparty/reference/xenia_apu_*`: ContextArrayAddress 0x600, Kick 0x650+n,
Lock 0x690+n, Clear 0x6A0+n, plus the XMA_CONTEXT_DATA layout in xma_context.h). Start
with a decoder that consumes input buffers and emits silence, then add real XMA2
decoding (FFmpeg xma2) and route XAudioSubmitRenderDriverFrame to WASAPI/XAudio2.

### Renderer (chosen 2026-10-07: custom renderer + XenosRecomp-derived shader translation)
Design in `notes/RENDERER.md`; D3D library map in `notes/D3D_MAP.md`. RAGE inlines
much of D3D, so the D3D12 renderer reads the GPU register state the game's own D3D
writes into the command stream, and hooks D3D only to capture shader containers.
Xenia's GPU sources (`thirdparty/xenia-canary`, sparse) are a reference only.

Status 2026-10-07 (evening):
- Shaders: all 2,137 menu-time shaders translate and compile (DXC, disk cache).
  Capture moved to the exits of D3D's create functions (every caller); lookups
  are stage-aware (a trivial `export r0` program is identical as VS and PS).
- Textures (`render/texture_cache.cpp`): CPU untiling (Xenos 2D/3D tiling, packed
  mip tails), endian swap, DXT1/3/5/DXN/DXT5A as BC1-5, 16-bit formats expanded,
  CTX1/DXT3A expanded, float/16-bit formats; fetch-constant swizzle as the SRV
  mapping, signs as SNORM/sRGB views or shader-side conversion; samplers from
  fetch constants. Guest writes to texture pages (write-protect watch) reload them.
- Resolves: per (address, size, format) targets; predicated-tiling bands map back
  into the full surface. Depth resolves not implemented yet.
- Input: `runtime/input.cpp` - XInput pads, keyboard for player 1, MCLA_INPUT_SCRIPT.
- XAM: overlapped extended errors are HRESULTs (fixed the "unexpected error while
  loading the game" dialog); system music player (XMP) reports idle.
- Reached: legal screens, title, new save created (`userdata/content/545407F8/
  00000001/mc4.sav`), vehicle select, free roam with HUD. The 3D world is still black.
- MmQueryAllocationSize returns the size from the address to the end of its
  allocation (RAGE memsets with it; returning the whole size wiped shader microcode).

### Playable (2026-10-08)
Free roam drives at a steady 30 fps with audio. Root causes found on the way:
- **Timebase**: `mftb` was recompiled to raw `__rdtsc()` (3.3 GHz here) but game
  code converts ticks with a hardcoded 1/49875000 (e.g. the frame timer at
  0x821BDA90). XenonRecomp now emits `PPC_QUERY_TIMEBASE()`; `ppc_overrides.h`
  scales the TSC to 49.875 MHz and KeQueryPerformanceFrequency returns 49875000.
- **Timer resolution**: without `timeBeginPeriod(1)` every short sleep (GPU thread
  idle, guest waits) lasted a 15.6 ms tick. With it, plus 2 ms of polling in the
  command processor before sleeping: 16 -> 29 fps.
- **Missing player car** (vpkd3d128): the car's physics bound extents
  (`phBoundComposite`, sub_82599FB0) became NaN, so the frustum test in
  sub_8235F738 rejected the car every frame. The XNA-math idiom packs all-ones
  (NaN) with `vpkd3d128 D3DCOLOR` to build a 1,1,1,0 select mask; the hardware packs
  NaN as 0 (Xenia: max before min). XenonRecomp's clamp let NaN through.
- **Can't drive** (profile settings): MCLA reads XPROFILE_GAMER_RACE_* (0x10040026-29)
  and the control sensitivity. Answering XSOURCE_NO_VALUE made it build a layout with
  gas/brake on the right stick (mcControlConfig scheme table: control 2 = type 9 /
  param 26 = right stick up, instead of type 8 / param 1 = RT). The layout is stored
  in the save, so saves made before the fix keep it.
- **Audio**: XMA decodes with FFmpeg 7.1's `xma2` decoder, loaded at run time from
  `<exe>\ffmpeg` (copied from the OBS Studio install by CMake; `apu/ffmpeg.cpp`
  verifies the struct offsets it uses). One 2 KB packet at a time, following packet
  skip chains, loops and streaming buffer swaps (`apu/xma.cpp`). The 5.1 mix the title
  submits goes to XAudio2 (`apu/audio_out.cpp`), which also paces the guest's audio
  callback.
- **Draw recording**: the texture/sampler lookups read the fetch constants back out of
  the write-combined upload heap; reading a local copy halved per-frame CPU cost.
- The pause menu's white screen is gone (was a symptom of the above). LARecomp's
  menu-list null guard (sub_82661210) is in `runtime/game_patches.cpp`.
- **Interpolator linkage**: the hardware links VS export N to PS register N. The
  vertex program the command buffer loads is D3D's copy re-linked to the current
  pixel shader, so naming its exports from the container's own table was wrong
  whenever D3D re-packed them (the minimap's road shader read zero alpha). Raw
  microcode now takes the partner container's semantics, and a container VS whose
  table disagrees with the PS takes the PS's (`xenos::ContainerLinkage`, keyed into
  the host shader).
- **Hitches**: textures are placed resources in 64 MB heaps (first-fit, 4 KB small
  placement, aliasing barrier, deferred frees); committed resources cost ~1 ms of
  kernel time each and made streaming stall. `MCLA_LOG_HITCHES` breaks slow frames down
  (draw sections, textures, mirror, pipelines, write faults); `MCLA_GPU_PASSES` gives
  GPU time per pass.
- Window: DPI aware; Alt+Enter / F11 / `--fullscreen` borderless fullscreen; presents
  without tearing (`MCLA_TEARING` to allow it).

Debugging aids added: `MCLA_PROFILE=<from>-<to>` (sampling profiler),
`MCLA_CAR_PROBE` (vehicle draw/visibility/input probes in `runtime/game_patches.cpp`),
`crash::WatchWrites` (hardware write watchpoints logging the guest call stack;
read watches never fire), `MCLA_INPUT_SCRIPT` analog holds (`rt`/`lt`/`sl`/`sr`/
`ru`/`rd` with `:<seconds>`), `MCLA_AUDIO_DUMP=<seconds>` (WAV of the output).

Later the same day: half floats round like Xenia (`PPCFloatToXenosHalf`), pipelines go
through an ID3D12PipelineLibrary disk cache, `MCLA.ini` sets the `MCLA_*` options, DPI
awareness, flip-model presents without tearing (unless `MCLA_TEARING`).

**Races** (pause > MODES > ARCADE > race type > race): load, count down and run at 30.0
fps with no frame over 50 ms during racing (hitches only on loading screens). Restart
Race, the race pause tab (Continue / Next / Quit / Restart / Favorites / View Replay)
and the replay prompt work. In free roam the PAUSE tab only has CONTINUE (normal).
Scripted input can't steer a whole route, so the finish/results screen is untested.

**GPU time is not the limit.** "GPU ~22 ms/frame" on the 780M barely moves when work is
removed: dropping all MSAA (every surface 1x) gave 20.7 ms, skipping the mirror-upload
barriers changed nothing. The iGPU clocks down to fill the 33 ms frame; "waiting for
GPU" is ~0, so frame pacing is CPU-side. The stats line now also prints draws (~5000),
transition barriers (~190) and mirror-upload commits (~100-200) per frame.
Breakdown (MCLA_GPU_PASSES): main 2x pass ~11 ms (predicated tiling: 2 tiles), shadow
cascades ~5 ms (depth-only 4x at pitch 640, resolved via sample 0 into a 1280x1280
atlas - a 1x render would be equivalent), 512x256 reflection map ~1.3 ms.

**Roof-edge strip in races**: a green/yellow/red band on every racer's roof edge is the
reflection of hard-edged coloured quads (traffic-light glows) near the centre of the
512x256 reflection map (EDRAM pitch 520 4x, resolved to 1C886000; dump with
MCLA_DRAW_LOG + MCLA_DUMP_RESOLVES). All reflection-pass draws are opaque
(blend 00010001), so they may be authentic; not confirmed against the console.

### Rendering accuracy (2026-10-08 evening, from user reports)
- **RB_COLOR_INFO.color_exp_bias is bits 20-25** (signed; format is 16-19). MCLA renders
  its 7e3 scene with +4 and resolves it with RB_COPY_DEST_INFO.copy_dest_exp_bias -4
  (planar reflection +2/-2). The hardware scales the PS output (all of RGBA, after alpha
  test, before blending) - `ColorOutput()` in common.hlsli, `colorExpBias` system
  constant - and the resolve shader applies the destination bias (+ clamps unorm
  destinations). Ignoring both looked right for opaque geometry but not for: clears
  (raw EDRAM bits get no bias, so they were 16x too bright), and alpha (the mirror
  glass uses alpha 1/16, made opaque by the x16).
- **Resolve clears** decode RB_COLOR_CLEAR in the surface's format (`DecodeColorClear`):
  0x21986619 is 7e3 gray 2.39 with alpha 0, not 8888 teal.
- 7e3 targets clamp color to 0..31.875 and store a 2-bit alpha (quantized for unblended
  writes).
- **Linkage with raw pixel shaders**: a container VS drawn with a raw PS links export i
  to TEXCOORDi. D3D's two clear vertex shaders share a raw PS; the second's container
  had no semantic for its color export, so some full-screen clears wrote black - the
  distant buildings' lighting flickered.
- Side mirrors are mostly-transparent glass over the scene clear; the white "debug cube"
  line list (24 verts, prim 2) is a real game draw, harmless.

Debug tools added: `MCLA_PICK=x,y` (pixel history, sample coordinates; logs which draw or
non-draw operation changed the sample, before/after values, mask and flags),
`MCLA_DRAW_LOG=a-b` (frame ranges), `MCLA_DUMP_VS` also dumps PS constants (as c256+),
`MCLA_DUMP_TEXTURE=<addr>`, `MCLA_SCREENSHOTS_RANGE=a-b` (bursts; scratch scripts
blocks.ps1/flicker.py find flicker), `MCLA_TIME_OFFSET_HOURS` (title screen time of day
follows the clock), `MCLA_TEST_BAR=<bmp>`.

Window: fullscreen bar (top middle hover: minimize / exit full screen / close), window
menu "Full screen", idle cursor hiding (`MCLA_CURSOR_HIDE_SECONDS`).

### Fixes 0.4.1 (2026-10-08 night, from user reports)
- **Crash at race end / garage**: sub_821BA038 (matrix -> quaternion, leaf, no .pdata)
  jumps through an inline absolute table right after its bctr; the previous run's
  boundaries split the function at the table, so find_jump_tables.py rejected it.
  Inline tables are now bounded by the next .pdata function. fix_switch_functions.py
  never ran: notes/xenonrecomp.log is UTF-16 (Windows PowerShell redirection).
- **Minimap blob**: the minimap (EDRAM tile 168, pitch 240, 4x) clears only a border
  strip and draws its streets (navy, alpha-blended) over whatever earlier passes left in
  its tiles - on the console those are cleared by the 128x128 passes at tiles 224/288.
  Each host surface kept its own contents, so streets smeared across frames. Now a
  resolve marks the EDRAM tiles its surface occupies (`ClaimTiles`), and a color surface
  first drawn in a frame whose tiles another surface resolved since is zeroed first
  (`DiscardIfOverwritten`). Full EDRAM aliasing is still not emulated.
- **Motion blur off by default**: the shader parameter lookup sub_8218A568 returns 0 for
  "blurEffectTintFade"/"blurEffectDesatScale" (as the community Xenia patch does);
  MCLA_MOTION_BLUR=1 restores it.
- `MCLA_WATCH_ADDR=<addr>@<seconds>[,<hits>]`: hardware write watch armed after a delay.
- Reference: github.com/ShootanoJumper/mcla-xenia-edge-patches (title 545407F8) has
  60 FPS (timing filter + render interpolation via code caves), motion blur, MSAA and
  shadow patches with addresses.
- **Open: night headlight "stage light"**: the headlights are a deferred spot light
  (vs 673E264C / ps 87508CB6, 180-index cone, blend DestColor*src: scene x (1 + 8 x
  falloff x color), c27.x = 1/16 compensates the +4 bias). Constants, linkage, depth
  resolve (both tiles) and reversed-Z reconstruction checked fine; hard-edged uniform
  trapezoids remain. Next: pixel history on a beam pixel to read the factor, depth
  precision of the R32 resolve. The flares (vs C3EE3410 / ps C10D539B) are fine.

### Packaging (2026-10-08)
- Icon: `MCLA/res/mcla.ico` from the box art (`MCLA/res/boxart.png`, `tools/make_icon.ps1`:
  logo + car crop for 64-256 px, car front for 16-48 px), version info in `res/mcla.rc`.
- The executable is a Windows-subsystem program now (no console; entry stays `main`).
- `release/Midnight Club Los Angeles/` is the self-contained game folder: the exe (renamed),
  DLLs, ffmpeg, shader cache, MCLA.ini (fullscreen on), README, `userdata/` (copy of the
  user's save from dist/MCLA-test at packaging time) and `game/` (hard links to `game/`).
  `tools/package_release.ps1` refreshes the program files without touching saves/settings.
  Desktop shortcut "Midnight Club Los Angeles" points at it. The user plays from there now.

### Next
1. Race finish / results screen and career missions (need someone at the wheel).
2. Race start grid is the heaviest scene (~6900 draws, GPU ~27 ms, recording ~21 ms on
   the 780M): close to the 33 ms budget.
3. Pipeline creation is still synchronous on a cache miss (first time a shader pair is seen).
4. Resolve dest swap and the fetch constant's swizzle are both ignored for resolved
   textures (they cancel for MCLA so far).

## Phase 3 plan (original)

The translated code compiles but cannot run without a host runtime. Rough order,
following Unleashed Recompiled's structure:
1. Executable shell: reserve 4 GB guest address space, load the PE image sections at
   0x82000000, build the indirect-call table from PPCFuncMappings, set up TLS, call the
   entry point 0x821322B8 on a guest thread.
2. Kernel/XAM imports (246 functions): stub everything and log calls; then implement
   memory, threads/events/critical sections, file I/O (map `game:` → extracted disc,
   `cache:` → local folder), timing.
3. Disc data: extract the four .rpf archives (or read them through the ISO).
4. Graphics: hook rage::grc* / the D3D cluster rather than emulate Xenos.
5. Audio: replace XAudio/XMA with a software decoder.
6. Input: XamInputGetState → XInput/SDL.
