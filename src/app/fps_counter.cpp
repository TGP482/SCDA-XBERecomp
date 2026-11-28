// Frame rate (top left) and the main menu watermark, drawn by the game in its own font over the Magma pages.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "app/app.h"
#include "gpu/fsr.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr bool kVersus = std::string_view(IMAGE_XBE) == "sc4_adv.xbe";
// font list: +4 first, +8 last; screen: page height
constexpr uint32_t kFonts = kVersus ? 0x3D8D84 : 0x6A66E4, kScreen = kVersus ? 0x3D8D8C : 0x6A6668;
constexpr uint32_t kPageHeight = kVersus ? 0xAE : 0x86;
// versus draws without checking these: manager +0x38 viewport, its +0x64 canvas
constexpr uint32_t kManager = 0x3D8D74, kViewport = 0x38, kCanvas = 0x64;
// font: vtable +8 Draw, +0x10 Width(text) in font pixels (solo)
constexpr uint32_t kName = 0x08, kFontSize = 0x49, kPages = 0x478, kDraw = 0x08, kWidth = 0x10;
constexpr int kSize = 12, kMargin = 8;  // page units
// versus draws its pages 1.1 wide about the centre: page x 29 is the screen's left side
constexpr int kLeft = kVersus ? 29 : 0;

// the body text font, else any font with its page textures made
uint32_t Font() {
    uint32_t fonts = R32(kFonts), found = 0;
    if (!fonts) return 0;
    for (uint32_t p = R32(fonts + 4), end = R32(fonts + 8); p && p < end; p += 4) {
        uint32_t font = R32(p), pages = font ? R32(font + kPages) : 0;
        if (!pages || !R32(pages) || !G<uint8_t>(font + kFontSize)) continue;
        if (!std::strncmp(static_cast<const char*>(HostPtr(font + kName)), "Text_", 5)) return font;
        if (!found) found = font;
    }
    return found;
}

bool CanvasUp() {
    if (!kVersus) return true;
    uint32_t manager = R32(kManager), viewport = manager ? R32(manager + kViewport) : 0;
    return viewport && R32(viewport + kCanvas);
}

// y: baseline from the bottom; right: x is the text's end
void Text(X86Context& ctx, const char* text, int x, int y, int size, uint32_t colour, bool shadow, bool right) {
    static const uint32_t block = mem::AllocSystem(0x60);
    uint32_t font = Font();
    if (!block || !font || !CanvasUp()) return;
    // style: scale, colour with alpha in the top byte, spacing, flags
    uint32_t style = block, chars = block + 0x10;
    int n = int(std::min<size_t>(std::strlen(text), 31));
    for (int i = 0; i <= n; i++) W16(chars + uint32_t(i) * 2, i < n ? uint8_t(text[i]) : uint8_t(0));
    float scale = float(size) / G<uint8_t>(font + kFontSize);
    W32(style, std::bit_cast<uint32_t>(scale));
    W32(style + 8, 0);
    W32(style + 12, 0);
    uint32_t ecx = ctx.ecx;
    ctx.ecx = font;
    if (right) x -= int(std::lround(CallGuest(ctx, R32(R32(font) + kWidth), {chars}) * scale));
    // Draw(text, x, y, style, space width, count), one more in versus
    for (int s = shadow; s >= 0; s--) {
        W32(style + 4, s ? 0xFF000000u : colour);
        CallGuest(ctx, R32(R32(font) + kDraw), {chars, uint32_t(x + s), uint32_t(y - s), style, 0, uint32_t(n), 0});
    }
    ctx.ecx = ecx;
}

}  // namespace

void app::DrawFps(X86Context& ctx) {
    uint32_t screen = R32(kScreen);
    if (!screen) return;
    char text[32];
    float fps = g_fps.load();
    // frame generation's rate beside the rendered one
    uint32_t n = fsr::Generated();
    if (n) std::snprintf(text, sizeof text, "FPS: %.1f, FG %.1f", double(fps), fps * (n + 1.0));
    else std::snprintf(text, sizeof text, "FPS: %.1f", double(fps));
    Text(ctx, text, kLeft + kMargin, int16_t(R16(screen + kPageHeight)) - kMargin - kSize, kSize, 0xFFFFFFFFu, true, false);
}

// solo mirrors the button legend: its text starts 80 in on baseline 46, size 16, grey B0. Versus pages are drawn
// 1.1 wide, so its numbers keep the size; its legend reaches the right side, so the text sits a line above it
void app::DrawWatermark(X86Context& ctx) {
    constexpr int kEnd = kVersus ? 544 : 560, kBaseline = kVersus ? 64 : 46, kTextSize = kVersus ? 15 : 16;
    char text[32];
    std::snprintf(text, sizeof text, "XBE Recomp V%d", kVersion);
    Text(ctx, text, kEnd, kBaseline, kTextSize, 0xFFB0B0B0u, false, true);
}
