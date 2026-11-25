# Linux port: design notes

## Ground rules
- Windows code paths are left alone, so the Windows build keeps working. It can't be compiled from Linux here,
  so every Windows-side edit is kept minimal and mechanical.
- Linux-only sources live in `src/linux/` and `src/gpu/vulkan*.cpp`. CMake picks the set per platform.
- Small Win32 uses in shared files get `#ifdef _WIN32` branches. Larger Win32-shaped code (input) compiles against
  `src/linux/win32_compat.h`, a small Win32 subset backed by SDL3 state, so its logic stays shared.

## Game code linkage (ELF)
On Windows the recompiled code is `<exe>.dll`, delay-loaded. It imports the runtime and the host's overrides from
the exe's import library. `/alternatename` makes each `sub_X` fall back to its own `__imp__sub_X` body when the host
has no override.

On Linux:
- The game code is `<exe>.so`, built with `-fPIC -shared`, and opened by `dispatch::Load` with
  `dlopen(RTLD_NOW | RTLD_GLOBAL)`.
- `X86_WEAK_FUNC(sub_X, __imp__sub_X)` becomes a weak ELF alias. The exe exports its overrides (default
  visibility, `-rdynamic`), so the dynamic linker binds the `.so`'s references to `sub_X` to the override when there
  is one, or to the weak alias when there isn't. This includes the relocations in `X86FuncMappings`.
- The host only calls into the game code through `__imp__*` names declared with `X86_EXTERN_FUNC` (and the data
  tables, read with `dlsym`). On the Linux host those declarations become static inline wrappers that resolve
  the symbol with `dlsym(RTLD_DEFAULT, name)` on first use. So the exe has no link-time dependency on the `.so`,
  and the first-run build can produce it after start, as on Windows.
- The `.so` links against the system libstdc++/libm. The headers stay freestanding (`-nostdinc`).

## Renderer
`src/gpu/vulkan.cpp` implements `gpu::Backend` like `d3d12.cpp` does, structured the same way so the two can be
read side by side.
- Vulkan 1.3: dynamic rendering, synchronization2, push descriptors (`VK_KHR_push_descriptor`). One descriptor set
  layout: b0, b1 uniform buffers; t0-t3 sampled images; s0-s3 samplers.
- The game's HLSL (`shaders.cpp`) and the post-pass HLSL go through glslang's HLSL front end to SPIR-V.
  Bindings are shifted: b → 0+, t → 2+, s → 6+. Vertex inputs `ATTRn` get `[[vk::location(n)]]`. FXC-only syntax
  (`1.#INF`) is rewritten before compiling.
- D3D conventions are kept: the viewport has negative height (y up in clip space), and the front face maps
  directly (as vkd3d does).
- Depth: D24S8 when the device has it, else D32S8. With D32 the depth bias goes through
  `VK_EXT_depth_bias_control` (float representation) so the game's 24-bit unit biases hold.
- Memory through VMA. Barriers are conservative (full pipeline barrier per transition), tracked per image layout.
  Rendering begins lazily and ends before any barrier, copy or clear outside it.

## Audio
`src/linux/audio_sdl.cpp` implements `audio/output.h` with an SDL3 audio stream. The mixer renders 5.1. SDL
gets 6 channels when the device has them, else the same stereo fold the XAudio2 path uses.

## Window and input
SDL3 owns the window and event loop on the main thread, which plays the part of the Win32 window thread. Mouse look
uses SDL relative mouse mode in place of raw input + ClipCursor. Keys keep their Windows virtual-key codes inside
`input.cpp` (bindings are saved by name), mapped from SDL scancodes.
