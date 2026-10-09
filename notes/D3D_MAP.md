# MCLA D3D9 (Xbox 360 XDK 2.0.6995) map

Groundwork for the custom renderer (hook D3D-level calls, translate shaders with
XenosRecomp). Addresses are guest addresses in `default.xex`.

## How it was found
- `tools/d3d_tables.py`: pointer tables into the D3D code and functions that build
  PM4 DRAW headers.
- `tools/gen_d3d_trace.py enable` + `tools/recompile.ps1` + rebuild, then run with
  `MCLA_D3D_TRACE=<seconds>`: hooks the entry of all 1,812 functions in
  0x823FA000-0x82470000 and writes `notes/d3d_trace.txt` (calls, calls/frame,
  % of calls with r3 == device, top callers, first argument sets, and the device's
  render-state setter table). Tracing is inert unless the env var is set.
  `gen_d3d_trace.py disable` removes the hooks.
- XenonRecomp extension: mid-asm hook register `"ctx"` passes the whole PPCContext.

## Device
- The D3D device lives at 0x40002080 at runtime; D3D publishes it in the
  `VdGlobalDevice` kernel variable. It is also the graphics interrupt context.
- GPU-progress fields: +0x2A88, +0x2A90 (-> fence), +0x2A94 (-> interrupt/callback
  block, physical), +0x2A9C (last issued fence).
- Flip queue: +0x40A4 vblank count, +0x40BC.. 8-entry queue {surface, target vblank},
  head +0x413C, tail +0x4140 (vblank handler sub_82419718).
- Per-CPU deferred callback rings: device + 0x2C3C + cpu*0x50 (write +0x38, read +0x3C),
  serviced by two D3D threads (start sub_8242FB88). Callback queued by sub_8242FD00.
- **Render-state setters**: `device + 0x40`, 0x65 entries (XDK `m_SetRenderStateCall`).
  RAGE applies changed states through it in sub_8218C9D8.
- **Sampler-state setters**: `device + 0x1D4`, 0x14 entries, `fn(device, sampler, value)`.
  (Earlier notes called this the render-state table; the trace dump was corrected.)
- Current shaders: vertex at `device + 0x3198`, pixel at `device + 0x3194`.

## Shaders
- Create: sub_82418100 (pixel), sub_82418210 (vertex). Both take a shader
  container (flags 0x102A1100 pixel / 0x102A1101 vertex; virtual part + physical
  microcode), copy the virtual part into the object (pixel +0x28, vertex +0x368)
  and the microcode into new physical memory (pixel base at +0x18, vertex at +0x20;
  programs load at base + Shader::physicalOffset). The renderer captures every
  shader at their common exits (mid-asm hooks at 0x82418208 / 0x824182F8).
- sub_82424320 re-emits vertex shaders inline (PM4 IM_LOAD_IMMEDIATE) with vertex
  fetches patched for the vertex declaration (hook at 0x824243F4 records copies).

## Resolves
- With predicated tiling (menus/gameplay draw 1280x720 in 1280x256 EDRAM bands)
  D3D resolves each band by moving RB_COPY_DEST_BASE inside the full tiled surface
  (base + n * 32x32-pixel tiles); the renderer maps such bases back to the surface.

## Functions identified so far
| Address | Calls/frame (attract) | Notes |
|---|---|---|
| 0x8241CD88 / 0x8241D220 | ~497 | predicated DRAW_INDX (draw indexed) |
| 0x8241D620 | ~545 | predicated DRAW_INDX, called from RAGE 0x821793B0 |
| 0x8241D230 | ~107 | predicated DRAW_INDX |
| 0x82413068, 0x82417538, 0x82418350, 0x82422488, 0x824225E0, 0x82427898, 0x8242DE08 | | other DRAW headers (init/clears/UP draws) |
| 0x82412A60 | init | ring buffer setup (VdInitializeRingBuffer) |
| 0x82419E98 | per frame | present path (VdSwap, VdGetSystemCommandBuffer) |
| 0x82411478 | | graphics interrupt handler (source 0 vblank, 1 CP) |
| 0x82412F98 / 0x82411E98 | | wait-for-GPU with hang detection |
| 0x82417678, 0x82415DC8, 0x82417758, 0x82410F88 | 1311 / 1164 / 861 / 687 | hottest device methods - candidates for SetTexture / shader constants / SetStreamSource |
| 0x8243D780-0x82447278 | | XGraphics texture tiling/format routines (table at 0x8206A2EC, 4 variants per format) |

## Save system (RAGE, around 0x82616000)
- Per-user state machine (168 bytes per user at 0x82905C20). Enumerating saves:
  XamEnumerate must return ERROR_IO_PENDING; the poller (sub_826166A0) accepts a
  failed overlapped only when its extended error is 0x80070012
  (HRESULT_FROM_WIN32(ERROR_NO_MORE_FILES)), classified by sub_82616588.

## Next
1. Name the render states (setter -> GPU register field -> D3DRS) if a D3D-level
   hook ever needs them (the renderer reads the PM4 register state instead).
2. Classify the hot device methods by arguments (samples in d3d_trace.txt).
