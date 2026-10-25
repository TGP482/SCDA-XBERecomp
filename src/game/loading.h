#pragma once
#include <cstdint>
// Mission loading screen and relaunch hand-off (loading.cpp), called from the ShowScreen hook in hud.cpp
struct X86Context;

namespace game::loading {

// before and after the render device's ShowScreen(path, persist, fade, clear): notes the picture it loads, and in an
// instance started by a relaunch loads the previous instance's last picture again in place of the kept display
void BeforeShowScreen(X86Context& ctx);
void AfterShowScreen(X86Context& ctx);

// ShowScreen with no picture, kept display or fade, done here in its place: the back buffer's frame is shown and
// copied back on the GPU (the game's copy goes through 640x480 memory)
void BothBuffers(X86Context& ctx, uint32_t render_device);

// a font the game has loaded, before the loader's last step (fonts.cpp): Title_18's glyphs are kept for the pictures
void FontLoaded(uint32_t font);

// a movie frame is being drawn: hud.cpp redoes its fit for the frame's shape
inline thread_local bool g_movie = false;

}  // namespace game::loading
