#pragma once
#include <cstdint>
#include <string>
// Mission loading screen and relaunch hand-off (loading.cpp), called from the ShowScreen hook in hud.cpp
struct X86Context;

namespace game::loading {

// loading.cpp, a mission picture ShowScreen decoded (path in lower case, BGRA rows top down): with Xbox One button
// prompts the White or Black button its tip names becomes LB or RB (loading_tips.cpp)
void EditTip(const std::string& path, uint8_t* picture, uint32_t pitch);

// before and after the render device's ShowScreen(path, persist, fade, clear): notes the picture it loads, and in an
// instance started by a relaunch loads the previous instance's last picture again in place of the kept display
void BeforeShowScreen(X86Context& ctx);
void AfterShowScreen(X86Context& ctx);

// ShowScreen with no picture, kept display or fade, done here in its place: the back buffer's frame is shown and
// copied back on the GPU (the game's copy goes through 640x480 memory)
void BothBuffers(X86Context& ctx, uint32_t render_device);

// a movie frame is being drawn: hud.cpp redoes its fit for the frame's shape
inline thread_local bool g_movie = false;

}  // namespace game::loading
