# Linux port: changes

Everything the `linux-port` branch adds to or changes in SCDA-XBERecomp, by area. The port was written with AI
assistance (Claude Code). The project it ports, its recompiler, kernel, XDK reimplementation, game hooks and the
D3D12 renderer this one follows, are the work of TGP482 and Joshhhuaaa.

Commits, oldest first:

| Commit | What |
|--------|------|
| `8197e67` | Linux port: SDL3 host and Vulkan renderer |
| `62bc754` | No focus pause before the window's first focus (the window never appeared on Wayland) |
| `2fb4229` | Audio: a queued pump thread instead of the device callback (crackling) |
| `7848f91` | Audio: prefer SDL's PulseAudio driver |
| `22bda7a` | AppImage: launcher, data folder, bundled compiler |

## What works

Built and tested on Arch Linux, KDE Plasma on Wayland, AMD Radeon RX 9070 XT (RADV), PipeWire:

- The first start builds the game code from the player's files, with the system's clang or the AppImage's own
- Title screen with its video, the menus, a new game relaunching into the first mission, the first level played
- Rendering through Vulkan 1.3, audio through SDL3, keyboard and mouse through SDL3
- The AppImage's launcher reads an ISO, checks it is the release this build was made from, and starts the game

Not yet tested: versus, controllers, MSAA, SMAA, HDR10 output, shader model 3 effects (HDR rendering, tone mapping,
soft shadows), ambient occlusion, online play, the Windows build after these changes (it can't be compiled on
Linux; the Windows-side edits are small and listed below).

## Build

- `CMakeLists.txt`: a Linux branch beside the Windows one. No resource compiler; the system clang and lld; SDL3,
  Vulkan, glslang, libpng and Vulkan Memory Allocator (fetched, header only). The game code builds as `<exe>.so`
  beside the executable, after it and relinked whenever it changes, so a build's `.so` counts as current. Linux
  sources are `src/linux/*` and `src/gpu/vulkan*`; Windows-only sources are left out on Linux and the other way
  round. imgui is v1.91.9 on Linux (an SDL3 backend that knows SDL 3.2's API), v1.91.0 on Windows as before.
- `CMakePresets.json`: `linux-debug`, `linux-release`, `linux-relwithdebinfo`.
- `src/cpu/x86_ops.h`: ELF linkage for the game code. Each `sub_X` is a weak alias of its body, so an override the
  executable exports (default visibility, `-rdynamic`) replaces it when the `.so` is opened. The executable's calls
  into the game code (`X86_EXTERN_FUNC`) look their symbol up by name on first use (`x86_import`), so the
  executable has no link-time tie to a `.so` the first start may still have to build.
- `src/cpu/dispatch.cpp`: `dlopen`/`dlsym` for the game code; `x86_import`.

## OS layer

- `src/kernel/disc.cpp`: reads disc images with `pread`.
- `src/kernel/boot.cpp`: relaunch (front end to mission, to versus) with `posix_spawn`, keeping the arguments.
  The new process signals its first frames over a pipe (`SCDA_HANDOVER_FD`), as the named event does on Windows,
  so the old window stays up until then. From an AppImage, through `$APPIMAGE --exe NAME`.
- `src/kernel/memory.cpp`: an include (the POSIX paths were there already).
- `src/linux/winsock_compat.h`: the Winsock that `src/xdk/xnet.cpp` is written against, on BSD sockets: Winsock's
  error codes and option values translated, `select`, `ioctlsocket`, no `SIGPIPE`. `xnet.cpp` takes the guest's
  `FIONBIO` by its Winsock value and counts its own `fd_set`.
- `src/app/system_info.cpp`, `src/app/settings.cpp`: the log's system lines and the "auto" language (from
  `LANG`) on Linux; the Windows display code behind `#ifdef`.
- `src/linux/paths.cpp`: `app::ExeDir()` (where the game writes) and `app::InstallDir()` (its executables). From an
  AppImage the game writes to `$XDG_DATA_HOME/scda-xberecomp`; `SCDA_DATA_DIR` picks another folder.

## App shell (SDL3)

- `src/linux/main.cpp`: entry and window thread. SDL3 window, event loop, display modes (fullscreen, borderless,
  windowed, on the chosen monitor), the host keys (F10, hide HUD, console), text input, mouse and wheel, pad hot
  plug, the menu cursor, the fps cap, a crash handler that logs the guest's registers and stack.
- `src/linux/win32_compat.*`: the part of Win32 `src/core/input.cpp` uses, on SDL3 state. Keys keep their Windows
  virtual-key codes (bindings are saved by name), mapped from SDL scancodes on the current layout. Mouse look uses
  SDL's relative mode where Windows clips the cursor. Also `MessageBoxW` through SDL.
- `src/core/input.cpp`: compiles against the compat header on Linux; the raw mouse handling moved into one
  function both platforms call; pads hot plug from the main loop on Linux.
- `src/linux/display.cpp`: monitors and their modes through SDL3.
- `src/linux/cursor.cpp`, `splash.cpp`, `handover.cpp`, `focus_pause.cpp`: the Windows counterparts on SDL3.
  Pause on focus loss waits for the window's first focus: a Wayland window is only on screen after its first frame.
- `src/linux/game_code.cpp`: the first-start build of the game code. `xbe_recomp`, then clang (the AppImage's own,
  `SCDA_CXX`, or `clang++` on `PATH`), then `ld.lld` alone to link the `.so`. Processes in their own groups, so a
  cancel ends them all; a small SDL progress window.
- `src/linux/audio_sdl.cpp`: the mixer's output through SDL3: a thread of its own keeps 30 ms queued and adds 10 ms
  each time the device runs dry (as the XAudio2 path does). PulseAudio first, then PipeWire, then ALSA;
  `SDL_AUDIODRIVER` overrides.
- `src/xdk/d3d8.cpp`, `src/game/exit_menu.cpp`, `src/app/menu.cpp`, `src/app/game_files.cpp`: the compat header on
  Linux, a Vulkan message where Windows names Direct3D 12, the mode list from SDL.

## Renderer (Vulkan)

- `src/gpu/vulkan.cpp`: the `gpu::Backend` the D3D12 one implements, laid out the same way (same function names,
  same order). Vulkan 1.3: dynamic rendering, synchronization2, push descriptors, timeline semaphore, VMA. Images
  carry their D3D12 state as a layout; a rendering instance begins when a draw needs it and ends before any barrier,
  copy or clear outside it. Everything the D3D12 renderer does: render scale, MSAA, CPU-written targets, readbacks
  (`Sync`, the late ones), texture packs, async pipeline builds, HDR rendering with bloom and exposure, SMAA,
  SSAO/GTAO, soft shadows, gamma, colour blind filters, HDR10 output, the settings panel, the F3 frame capture.
  D24S8 depth where the device has it, else D32S8 with the depth bias kept in 24 bit units
  (`VK_EXT_depth_bias_control`).
- `src/gpu/vulkan_shaders.*`: the HLSL to SPIR-V through glslang's HLSL front end, the D3D12 root signature's
  registers at fixed bindings. FXC-only syntax and the AO passes' stencil channel adapted on the way.
- `src/gpu/vulkan_cache.*`: SPIR-V and `VkPipelineCache` on disk, the pipeline keys earlier runs drew, the build
  threads (as `pipeline_cache.cpp` for D3D12).
- `src/gpu/post_shaders.h`: the full screen passes' HLSL, moved out of `d3d12.cpp` byte for byte so both renderers
  use it (the D3D12 shader cache keys don't change). `d3d12.cpp` now refers to it.
- `src/gpu/dxgi_format.h`: the `DXGI_FORMAT` values the shared code names, defined on Linux.
- `src/gpu/texturedumper.cpp`: PNG through libpng (BC textures decoded for dumps), the pack folder watched with
  inotify.
- `src/gpu/render_thread.cpp`: thread naming on Linux.

## AppImage

- `src/launcher/launcher.cpp` (`SCDA_Launcher`): SDL3 + ImGui in the settings panel's colours. Picks the disc image
  or game folder, links it as `assets/` in the data folder (files the player put there are never touched), checks
  both xbes against the releases this build was made from, sets display mode, resolution, v-sync, HDR output, FPS
  counter, intro skipping, and starts story/co-op or versus.
- `tools/linux/make_appimage.sh`: builds, lays out `out/appimage/AppDir` (executables, `xbe_recomp`, the configs and
  headers, the system's clang and lld with their libraries, glslang/SPIRV-Tools/libpng), and makes
  `SCDA-XBERecomp-x86_64.AppImage` with `appimagetool` (installed separately). No game code or data goes in it.
- `tools/linux/appimage/`: `AppRun` (the launcher, or `--exe NAME`), the desktop file, the icon.
- The game executables carry an `RPATH` of `$ORIGIN/../lib` for the libraries the AppImage carries.

## Testing aids

- `tools/linux/shader_test.cpp`: every post pass and a spread of generated game shaders through the Vulkan path,
  checked with `spirv-val` (243 of 243 valid).
- `SCDA_DUMP_FRAMES`, `SCDA_TEST_INPUT`, `SCDA_VULKAN_DEBUG`, `SCDA_VULKAN_DEVICE`, `SCDA_HDR_PAPER_NITS`,
  `SCDA_HDR_PEAK_NITS`, `SCDA_LAUNCHER_SHOT`, `SCDA_DATA_DIR`: see `PROGRESS.md`.

## Windows-side edits

All small, to keep the Windows build as it was: `#ifdef _WIN32` around Windows-only includes and code in shared
files, `HostSelect` in `xnet.cpp` (Winsock's `select(0, ...)` as before), the raw mouse handling in `input.cpp`
moved into a function `OnRawInput` calls with the same values, `HasExtension` in `texturedumper.cpp` (the same
`_wcsicmp`), the post pass HLSL moved byte for byte, and `x86_ops.h`'s Windows macros unchanged inside `#if
defined(_WIN32)`. They have not been compiled on Windows yet.
