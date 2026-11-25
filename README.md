# Splinter Cell: Double Agent - XBE Recomp
An unofficial native PC port of the original Xbox version of Splinter Cell: Double Agent via static recompilation, built into a 64 bit Windows executable. The game's x86 code is translated to C++ ahead of time, the Xbox kernel, Direct3D and DirectSound calls are implemented on Windows, and everything is drawn with Direct3D 12 and played through XAudio2. Made with the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool, created for this game originally.

A native Linux build is also available: the same recompiled game, drawn with Vulkan and run through SDL3, packaged as an AppImage with a launcher that takes your disc image. See [Linux](#linux).

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

## Installation
The latest version of [SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp/releases) can be found in the Releases page.

- Place your game files in the `assets` folder. This can be an ISO, or the extracted data files.
- Run `SCDA.exe`. The first start compiles the game's code for your PC, this only happens once.
- Recomp Settings can be adjusted in the in-game options menu while the game is running, or through the Settings overlay, which is opened with <kbd>F10</kbd> by default.

<img width="1920" height="1080" alt="Recomp Settings" src="https://github.com/user-attachments/assets/d13e1fb6-ba97-4067-84e1-c2362d66d7c4"/>

### Linux
> [!NOTE]
> The Linux port (the Vulkan renderer, the SDL3 host, the AppImage and its launcher) was written with AI assistance (Claude Code), building on the original project's code. See [Credits](#credits).

- Run `SCDA-XBERecomp-x86_64.AppImage`. A launcher opens.
- Click **Choose disc image...** and pick your game's `.iso` (or **Choose game folder...** for the extracted files). The launcher checks it is the release this build was made from. The ISO stays where it is.
- Pick the display settings, then **Play** (story and co-op) or **Versus**.
- The first start compiles the game's code for your PC with the compiler the AppImage carries, this only happens once.
- Settings, saves, logs and the compiled code are kept in `~/.local/share/scda-xberecomp`.

## Features
### Display
- **Monitor** - Picks the monitor the game shows on, for Fullscreen and Borderless (and where Windowed opens). Lists every connected monitor by its Windows number and desktop resolution.
- **Display Mode** - Switch between Fullscreen, Borderless and Windowed.
- **Resolution** - Lists every mode your monitor reports and defaults to your desktop resolution, replacing the original 640x480 resolution.
- **Render Resolution** - Renders the game at 20% to 200% of the output resolution, allowing supersampling or downscaling independent of the window size.
- **Field of View** - Sets the horizontal field of view during gameplay from 45 to 140 degrees, with 75 being the game's default. Cutscenes, scopes, gadget views and lock picking keep the original.
- **Max Frame Rate** - Caps the frame rate to your monitor's refresh rate or a custom value, or removes the cap entirely, replacing the Xbox's 30 FPS limiter.
- **V-Sync** - Reduces screen tearing.
- **HDR Output** - Enables HDR10 output on displays with Windows HDR turned on, detected automatically. Highlights extend up to the display's peak brightness.
- **FPS Counter** - Displays the frame rate on screen.
- **FMV Scale** - Choose between Fill, which makes the FMVs fill widescreen displays, and 4:3, which shows them in a 4:3 frame as on the original Xbox.
- **Colour Blind Mode** - Adjusts the colours for Protanopia, Deuteranopia or Tritanopia.

### Graphics
- **Multisample Antialiasing (MSAA)** - Enables 2x, 4x or 8x MSAA to smooth jagged edges while preserving a sharp image. MSAA does not smooth alpha-tested edges such as fences and foliage.
<div align="center">
  <table>
    <tr>
      <td width="50%"><img width="512" src="https://github.com/user-attachments/assets/150a11f4-c437-4014-9ee2-b7ee1857713b"></td>
      <td width="50%"><img width="512" src="https://github.com/user-attachments/assets/d84194f6-5175-4953-8390-df206cc6cedc"></td>
    </tr>
    <tr>
      <td align="center">Off</td>
      <td align="center">MSAA 8x</td>
    </tr>
  </table>
</div>

- **Subpixel Morphological Antialiasing (SMAA)** - Enables SMAA to smooth jagged edges across the whole frame with a softer image. SMAA also smooths alpha-tested edges such as fences and foliage, and can be combined with MSAA.
<div align="center">
  <table>
    <tr>
      <td width="50%"><img width="512" src="https://github.com/user-attachments/assets/daff599b-8bf2-4156-a3e3-a5f1ebd87617"></td>
      <td width="50%"><img width="512" src="https://github.com/user-attachments/assets/5dc8e76a-fcd7-4b8e-9964-f76cc1cfdde3"></td>
    </tr>
    <tr>
      <td align="center">Off</td>
      <td align="center">SMAA</td>
    </tr>
  </table>
</div>


- **Anisotropic Filtering** - Forces the selected anisotropic filtering level on all textures.
<div align="center">
  <table>
    <tr>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/594acdec-3a98-4dc5-ba6c-62e238db3895"></td>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/7a55879d-4f1d-49db-98c2-c687b0213c3c"></td>
    </tr>
    <tr>
      <td align="center">Off</td>
      <td align="center">Anisotropic 16x</td>
    </tr>
  </table>
</div>

- **Shadow Resolution** - Raises the shadow resolution from the Xbox's 512x512 (default) up to 8192x8192.
<div align="center">
  <table>
    <tr>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/abda5e45-ce01-4f3b-a5ca-2e71de8d0c26"></td>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/95d964c2-2f57-4e3e-aaf6-b652a70db091"></td>
    </tr>
    <tr>
      <td align="center">512x512</td>
      <td align="center">8192x8192</td>
    </tr>
  </table>
</div>

- **Shadow Distance** - Increases the distance at which lights and their shadows turn off by up to 8x.
<div align="center">
  <table>
    <tr>
      <td width="50%"><img width="512" src="https://github.com/user-attachments/assets/2afe8ca6-e7f7-4279-950f-c41f3de23113"></td>
      <td width="50%"><img width="512" src="https://github.com/user-attachments/assets/71d0f145-272b-4244-9af9-1e290466c669"></td>
    </tr>
    <tr>
      <td align="center">Shadow Distance 1x</td>
      <td align="center">Shadow Distance 8x</td>
    </tr>
  </table>
</div>

- **Shadow Filter** - Choose between SM1.1, the Xbox shadow filter (default), and SM3 Soft, the Chaos Theory PC Shader Model 3 soft shadow filter.
- **Ambient Occlusion** - Adds screen space ambient occlusion, creating soft contact shadows where surfaces meet. Choose between SSAO and GTAO.
### Shader Model 3
- **Shader Model** - Choose between Shader Model 1.1, the Xbox look, and Shader Model 3.0, which enables the Chaos Theory PC style features.
<div align="center">
  <table>
    <tr>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/52b7ed68-cb87-466e-847b-8364589a6d8b"></td>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/d82cc615-ea9a-4621-991c-663e7d3bd242"></td>
    </tr>
    <tr>
      <td align="center">Off</td>
      <td align="center">On</td>
    </tr>
  </table>
</div>

- **HDR Rendering** - Enables Shader Model 3 HDR rendering.
- **Tone Mapping** - Enables Shader Model 3 tone mapping.

### Textures
- **Texture Replacement** - Replaces the game's textures with the `.dds` or `.png` images in the `textures\replacement` folder beside the exe.
- **Texture Dumping** - Saves every texture the game uses into the `textures\dumps` folder beside the exe as you play.
- **Dump Format** - Choose between PNG or DDS, which keeps the original compression and mipmaps.

### Input
- **Mouse and Keyboard** - Full keyboard and mouse support with raw input, adjustable horizontal and vertical sensitivity, inverted look and rebindable controls.
- **Controller** - Controller with SDL3 support, buttons can be rebound from the Controller page. Defaults match the Xbox layout.

<div align="center">
  <table>
    <tr>
      <td width="50%"><img width="512" src="https://github.com/user-attachments/assets/baec07ee-feb3-4748-a121-d1a5bf44e8d3"></td>
      <td width="50%"><img width="512" src="https://github.com/user-attachments/assets/3b344418-9592-4e8d-b165-f64bdee8c311"></td>
    </tr>
    <tr>
      <td align="center">Mouse and Keyboard</td>
      <td align="center">Controller</td>
    </tr>
  </table>
</div>

### General
- **Skip Intro** - Skips the intro logos.
- **Pause On Focus Loss** - Pauses the game and its sound while the window is in the background or minimized.
- **Co-op Support** - Co-op can be played online by directly connecting to a friend's IP address, over LAN, or in split-screen.
- **Co-op Tick Rate** - Raises the co-op update rate, which is capped at 22 Hz in the original game, up to 142 Hz. Both players must use the same value.
- **Co-op Min Input Delay** - Lowers the minimum input delay in co-op from the original 50 ms down to 0 ms. Both players must use the same value.

### Miscellaneous
- **Boot Splash** - Place a `SplinterCell4Logo.png` (or `.bmp`) beside the exe to display a splash screen at launch.
- **Custom Menu Cursor** - Place a `cursor.png` (or `cursor.dds`) beside the exe to replace the Windows arrow in the game's menus.

## Playing Online
Co-op and Versus can be played online by connecting directly to the host's IP address. No third-party programs are needed.
- Press F10 by default to open the Settings overlay.
- Enter the host's IP address in **Coop Join IP** for Co-op or **Versus Join IP** for Versus.
- From the Co-op or Versus menu, select LAN and join the host's session from the list.

### Hosting a Game Session
As the host, you must port forward the following ports on your router so other players can connect to you:

| Protocol | Port |
|----------|------|
| UDP      | 9100 |
| UDP      | 1000 |
| UDP      | 1010 |

> [!TIP]
> If you're unable to port forward, such as when your connection is behind CGNAT, or would like to keep your IP address private, you can use a virtual LAN service to play together instead.

## System Requirements
### Minimum
- **OS**
  - Windows 10 64-bit
- **CPU** with 2 cores and support for SSE4.2 and POPCNT
  - Intel Nehalem (Intel Core 1st Generation)
  - AMD Bulldozer (AMD FX series)
- **GPU** with support for Direct3D 12
  - NVIDIA GeForce GT 630 (Kepler)
  - AMD Radeon HD 7750
  - Intel HD Graphics 510 (Skylake)
- **Memory**
  - 4 GB
- **Storage**
  - 6 GB for the game files, the compiled game code, saves and shader cache

### Linux
- **OS** - a 64-bit Linux distribution with glibc as new as the build machine's (see [Building on Linux](#building-on-linux)), on Wayland or X11
- **GPU** with a Vulkan 1.3 driver (Mesa RADV, NVIDIA, Intel ANV) that supports push descriptors
- **Audio** through PulseAudio, PipeWire or ALSA

## Building
### 1. Clone the Repository
Clone this repository using [Git](https://git-scm.com/).
```
git clone https://github.com/TGP482/SCDA-XBERecomp.git
```

### 2. Add the Required Game Files
Place your extracted game data files in the `assets` folder. `default.xbe` and `sc4_adv.xbe` must be there, with the `Default`, `Media` and `adv` folders beside them.

> [!NOTE]
> Building does not support the ISO format, so the files must be extracted. ISOs only work for playing.

### 3. Install Dependencies
Install [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) and select the following **Workloads** and **Individual components**:
- Desktop development with C++
- C++ Clang tools for Windows
- C++ CMake tools for Windows

> [!TIP]
> `build.bat` offers to install the Clang and CMake components for you if they're missing.

### 4. Build the Project
- Run `build.bat`. The first build recompiles the entire game and can take 30 to 90 minutes on weaker systems.
- When it finishes, run `out\build\release\SCDA.exe`.

## Building on Linux
### 1. Install Dependencies
On Arch Linux:
```
sudo pacman -S --needed clang lld cmake ninja git vulkan-headers vulkan-icd-loader glslang spirv-tools libpng
```
Other distributions need the same tools and the development packages SDL3 builds against (X11, Wayland, PulseAudio).

### 2. Add the Game Files
As on Windows: `default.xbe` and `sc4_adv.xbe` with the `Default`, `Media` and `adv` folders beside them, in the `assets` folder (a symbolic link to them works too).

### 3. Build
```
cmake --preset linux-release
cmake --build out/build/linux-release -j
./out/build/linux-release/SCDA
```
The game code builds in a minute or two on a fast machine. Use `linux-debug` for full logs.

### 4. AppImage
```
tools/linux/make_appimage.sh
```
It needs [appimagetool](https://github.com/AppImage/appimagetool) installed (or `APPIMAGETOOL=/path/to/appimagetool`) and writes `out/appimage/SCDA-XBERecomp-x86_64.AppImage`, with no game code or data in it. Built on a rolling distribution, the AppImage needs that glibc or newer: build it in an older distribution's container to share it.

The Linux port's changes, design and progress are in [docs/linux-port](docs/linux-port).

## Credits
- **[TGP482](https://github.com/TGP482)**: created SCDA-XBERecomp and the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool it is made with.
- **[Joshhhuaaa](https://github.com/Joshhhuaaa)**: co-author of SCDA-XBERecomp.
- **Linux port**: written with AI assistance (Claude Code). It reuses the original project's recompiler, kernel, XDK reimplementation, game hooks and renderer design; the Vulkan renderer follows the original Direct3D 12 one function for function.
