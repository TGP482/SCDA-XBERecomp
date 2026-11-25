# Linux port: progress

A native Linux build next to the Windows one. The Windows code paths stay as they are; Linux gets its own files
(`src/linux/`, `src/gpu/vulkan*.cpp`) or `#ifdef _WIN32` branches. See `DESIGN.md` for the decisions behind it.

Game files for testing: `assets` is a symlink to the loose files folder in the repo root (both ignored by git).

## Status

Legend: `[ ]` not started, `[~]` in progress, `[x]` done (built on Linux), `[t]` tested in game

### Build
- [x] CMake: Linux branch (no RC, system clang/lld, `.so` game code, SDL3/Vulkan/libpng, VMA); presets `linux-*`
- [x] `x86_ops.h`: ELF linkage for the game code (weak aliases, default visibility, `x86_import`)
- [x] xbe_recomp runs on Linux and generates `generated/game` and `generated/versus`
- [x] Game code `.so` builds (`SCDA.so`, `SCDA_Versus.so`), ~36 s on 32 threads
- [ ] `build.sh` (counterpart of `build.bat`)
- [x] AppImage: `tools/linux/make_appimage.sh` (needs appimagetool installed: AUR appimagetool-bin, or APPIMAGETOOL=path).
  Launcher GUI (`src/launcher`), bundled clang + lld, data in `~/.local/share/scda-xberecomp`. Built on this machine
  it needs this glibc (2.43+): build in an older distro's container to share it

### OS layer
- [x] memory (Commit/Decommit already had POSIX paths)
- [x] threads (already had pthread paths)
- [x] disc image reader (pread)
- [x] relaunch + handover (posix_spawn, pipe in SCDA_HANDOVER_FD instead of a named event)
- [x] crash handler (SIGSEGV/SIGABRT, guest stack to log) — in `src/linux/main.cpp`
- [x] dispatch: dlopen/dlsym instead of LoadLibrary/GetProcAddress
- [x] system info, settings (language from LANG, displays from SDL: `src/linux/display.cpp`)
- [x] sockets (`src/linux/winsock_compat.h`: error codes, option values, select, MSG_NOSIGNAL)

### App shell (SDL3)
- [x] main loop, window, display modes, fullscreen/borderless/windowed (`src/linux/main.cpp`)
- [x] input: Win32 compat layer (`src/linux/win32_compat.*`: VK codes, layouts, relative mouse mode)
- [x] ImGui SDL3 backend (imgui v1.91.9 on Linux, Windows stays on v1.91.0)
- [x] cursor (cursor.png; cursor.dds not on Linux), splash, focus pause, exit menu close
- [x] game code first-run build (`src/linux/game_code.cpp`: system clang++, SDL progress window)

### Renderer (Vulkan)
- [x] device, swapchain, frames, upload ring, VMA
- [x] HLSL to SPIR-V (glslang), shader + pipeline disk cache, async build threads (`vulkan_shaders.cpp`, `vulkan_cache.cpp`)
- [x] targets, textures, samplers, draws, clears, copies
- [x] readback: Sync, SyncLate/DeliverLate/RecordLate
- [x] post: HDR resolve/bloom/exposure, SMAA, AO, soft shadows, gamma, colour blind, HDR10 output (shared HLSL in `gpu/post_shaders.h`)
- [x] ImGui Vulkan backend
- [x] frame capture (F3, debug builds)
- [x] texture packs / dumps (libpng instead of WIC, inotify instead of change notifications)

### Audio
- [x] mixer output through SDL3 (`src/linux/audio_sdl.cpp`)

### Testing
- [t] boots to the title screen (Bink video background, prompt): frames dumped and checked
- [~] in-game rendering: attract demo (Iceland) draws characters, terrain, HUD, night vision; brightness of snow levels not yet compared with the D3D12 build
- [~] audio: crackling on SDL's PipeWire driver; now PulseAudio first (SDL_AUDIODRIVER overrides) + queued pump thread (retest)
- [ ] keyboard/mouse/controller
- [t] mission relaunch (handover): menus to a new game, relaunched into x01_iceland_a with the handover
- [ ] MSAA, SMAA, HDR10 output, AO, soft shadows, HDR rendering
- [t] first-start game code build (rebuilds the .so when stale)
- [ ] versus executable

## Log
- 2026-11-25: survey done, plan written, started.
- 2026-11-25: build, OS layer, app shell, audio written; whole host compiles on Linux. Next: `src/gpu/vulkan.cpp`.
- 2026-11-25: Vulkan backend written; everything builds. `tools/linux/shader_test.cpp`: 243 generated/post shaders compile and pass spirv-val. Testing starts.
- 2026-11-25: first runs. Fixed: X86_EXTERN_FUNC name stringizing (GUEST_ORIG), process groups in the first-start
  build. Title screen and attract demo render, ~72 fps (v-sync) in the debug build, no errors in the log.
- 2026-11-25: fixed: the game window never appeared on Wayland with pause on focus loss on (the default). The window
  is only mapped at its first frame, so it had no focus yet and the game paused before presenting. Focus pause now
  waits for the window's first focus (win32::EverFocused). Present/acquire failures are logged.
- 2026-11-25: first level played by hand and ran well. Audio crackled: the SDL output ran the mixer inside SDL's
  callback with a 10 ms device buffer. Now a thread of its own keeps 30 ms queued and adds 10 ms each time the
  device runs dry (up to 100 ms, logged "audio: output starved"), as the XAudio2 path does.
- 2026-11-25: AppImage. Launcher (SDL3 + ImGui, F10 panel colours): pick the disc image or game folder (linked as
  assets/ in the data folder), checks both xbes against the releases this build was made from, display settings,
  Play / Versus. In an AppImage the game writes to the data folder (`app::ExeDir`), reads the compiler from the
  install folder (`app::InstallDir`), relaunches through `$APPIMAGE --exe NAME`. The first start links the .so with
  lld alone (no C runtime files or libraries: they bind at dlopen). Tested from the AppDir with the system clang off
  PATH: game code built with the bundled compiler, the game ran from the ISO.

## Testing aids
- `SCDA_DUMP_FRAMES=n`: every nth presented frame to `captures/frame_NNNNN.bmp` (BGRA with alpha: view with alpha off)
- `SCDA_TEST_INPUT="3000:Return,8000:Down"`: scripted key taps (SDL key names) at ms from start, window taken as focused
- `SCDA_VULKAN_DEBUG=1`: validation layer (install vulkan-validation-layers), `SCDA_VULKAN_DEVICE=<name part>`
- `SCDA_HDR_PAPER_NITS`, `SCDA_HDR_PEAK_NITS`: HDR10 output levels (Linux can't read them from the system)
- For runs in the background, put `pause_on_focus_loss = false` and `display_mode = 'windowed'` in `scda.toml` beside the exe
- `SCDA_LAUNCHER_SHOT=file.bmp`: the launcher saves a frame there and quits (with `SDL_VIDEO_DRIVER=offscreen`)
- `SCDA_DATA_DIR=dir`: where the game writes (an AppImage defaults to `~/.local/share/scda-xberecomp`)
- `tools/linux/shader_test.cpp`: compiles every post pass and a spread of generated shaders through the Vulkan path:
  `clang++ -std=c++20 -Isrc tools/linux/shader_test.cpp src/gpu/shaders.cpp src/gpu/vulkan_shaders.cpp -lglslang -lSPIRV-Tools-opt -lSPIRV-Tools -lglslang-default-resource-limits -o shader_test && ./shader_test outdir`

## Next
1. Drive the menus into a mission (tests relaunch + handover), then SCDA_Versus
2. Toggle MSAA / SMAA / HDR / AO / soft shadows / shader model 3 and check frames
3. Compare frames with the D3D12 build on Windows; build the Windows side to confirm nothing broke
4. `build.sh`, release packaging (a stub-free first start works: the .so is built at runtime when missing)
