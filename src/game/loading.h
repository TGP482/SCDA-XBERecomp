#pragma once
#include <cstdint>
#include <string_view>
// Mission loading screen and relaunch hand-off (loading.cpp), called from the ShowScreen hook in hud.cpp
struct X86Context;

namespace game::loading {

// Magma's fonts the loading pictures are written in, kept from the game's own as they load (Title_18, Text_16):
// nullptr until its page has been read from its texture
struct Font;
const Font* KeptFont(std::string_view name);
float TextWidth(const Font& font, std::u16string_view text);  // at its size, a missing letter as a space
int CapHeight(const Font& font);
// text drawn into a picture (BGRA rows, pitch bytes apart, w x h) from left on baseline at the font's size, ink in BGR
void DrawText(uint8_t* picture, uint32_t pitch, int w, int h, const Font& font, std::u16string_view text, float left,
              float baseline, const float (&ink)[3]);

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
