# Renderer design (custom, D3D12)

Chosen 2026-10-07: a custom renderer that uses XenosRecomp for shaders. This
note records how the game's graphics reach it.

## Where the state comes from
The game's statically linked D3D (XDK 2.0.6995) already turns every API call into
Xenos GPU commands (PM4). The command processor (`MCLA/gpu/command_processor.cpp`)
executes that stream and keeps the GPU register file, so at each draw packet the
complete state is in registers: render targets, depth/stencil/blend/raster state,
viewport, shader constants (0x4000+), fetch constants (0x4800+, textures and
vertex buffers), index buffer (packet), primitive type.

This avoids reimplementing the D3D API: RAGE inlines much of the 360 D3D API
(constant and texture setters write the device's register shadow directly), so
hooking D3D functions would miss state that the command stream always carries.

D3D-level hooks are used where the command stream lacks information:
- **Shader containers** (`MCLA/gpu/shader_registry.cpp`): XenosRecomp needs the
  D3D shader container (constant table, vertex elements, interpolators), but the
  stream only holds microcode. Hooks after RAGE's calls to D3D's shader creation
  functions (sub_82418100 pixel, sub_82418210 vertex) map the microcode address
  (object +0x18 / +0x20, plus the program's physical offset) to the container.
  D3D re-emits vertex shaders inline with patched vertex fetches; a hook in
  sub_82424320 maps those copies, and a content hash that ignores vertex fetch
  instructions catches copies made elsewhere. Coverage: about 98% of vertex and 95%
  of pixel shader loads. The rest are D3D's built-in shaders (clear and
  startup), to be translated by hand.

## Pieces
1. **Window + D3D12 device + swap chain**: present at each XE_SWAP.
2. **GPU mirror of guest physical memory**: a 512 MB buffer. Pages are uploaded
   when a draw first uses them, then write-protected in every host view (A0/C0/E0)
   so a CPU write faults, marks the page dirty and unprotects it. Host code that
   writes guest memory in bulk (file reads) must invalidate the range first.
3. **Shaders**: XenosRecomp's translator built into the runtime, modified so
   vertex fetches read the mirror directly, with the byte swap, format, stride
   and offset taken from the patched microcode. Compiled with DXC at first use
   and cached on disk.
4. **Draw translation**: register state goes to PSOs (blend, depth, raster, RT
   formats) and to dynamic state (viewport, scissor, stencil reference, blend
   factor). Constants go into constant buffers. Index buffers are byte-swapped on
   upload.
5. **Textures**: a compute shader untiles from the mirror and converts formats.
   Textures are cached by fetch constant and page write generation.
6. **EDRAM / render targets**: host render targets keyed by EDRAM base, pitch,
   format and MSAA. A resolve copies a render target into a host texture registered
   at the destination guest address. Later fetches of that address use it, and the
   swap presents it.
