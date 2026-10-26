// Movies on a Magma page (the main menu's Cinematics, PH_Video) are drawn by the render interface's movie draw,
// thiscall(movie) with the Bink at movie +0x88: sub_002E6D5B decodes and draws, sub_002E708A draws a held frame. Its
// fit takes a pixel aspect of 4/3 in a widescreen mode, except in the menu map (the controller's vtable +0xDC,
// sub_001D05CB, map name "menu"). That keeps the 4:3 menu background from being pillarboxed, but letterboxed the 16:9
// cinematics as well, so a movie wider than 4:3 is fitted there as it is in a level.
#include <chrono>
#include <cstdint>
#include <thread>

#include "game/loading.h"
#include "game/subtitles.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kMovieBink = 0x88;  // Bink: +0 width, +4 height
constexpr uint32_t kBinkPaused = 0xFC;
thread_local bool g_wide_movie = false;

bool Wide(uint32_t movie) {
    uint32_t bink = movie ? R32(movie + kMovieBink) : 0;
    return bink && uint64_t(R32(bink)) * 3 > uint64_t(R32(bink + 4)) * 4;
}

}  // namespace

// The viewport draw (sub_001CDA8A) skips every frame its movie is not due on, menus included, which held the main
// menu to the 30 fps of its background movie. It is told the movie is due, and a movie that is not draws held.
X86_EXTERN_FUNC(GUEST_ORIG(BinkWait));
X86_FUNC(GUEST(BinkWait)) {
    if (R32(ctx.esp) == 0x1CDDB9) {
        ctx.eax = 0;
        ctx.esp += 8;  // ret 4
        return;
    }
    // the fullscreen player spins on this between frames, sleep instead of holding a whole core
    bool player_loop = R32(ctx.esp) == 0x3948C;
    GUEST_ORIG(BinkWait)(ctx, base);
    if (player_loop && ctx.eax) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

X86_EXTERN_FUNC(__imp__sub_002E6D5B);
X86_EXTERN_FUNC(__imp__sub_002E708A);
X86_FUNC(sub_002E6D5B) {
    uint32_t movie = R32(ctx.esp + 4), bink = movie ? R32(movie + kMovieBink) : 0;
    g_wide_movie = Wide(movie);
    game::loading::g_movie = true;
    if (bink && !R32(bink + kBinkPaused) && CallGuest(ctx, SYM(BinkWait), {bink}))
        __imp__sub_002E708A(ctx, base);
    else
        __imp__sub_002E6D5B(ctx, base);
    g_wide_movie = game::loading::g_movie = false;
    if (bink) game::subtitles::AfterPageMovie(bink);
}

X86_FUNC(sub_002E708A) {
    uint32_t movie = R32(ctx.esp + 4), bink = movie ? R32(movie + kMovieBink) : 0;
    g_wide_movie = Wide(movie);
    game::loading::g_movie = true;
    __imp__sub_002E708A(ctx, base);
    g_wide_movie = game::loading::g_movie = false;
    if (bink) game::subtitles::AfterPageMovie(bink);
}

// the level is the menu (thiscall, no arguments)
X86_EXTERN_FUNC(__imp__sub_001D05CB);
X86_FUNC(sub_001D05CB) {
    if (g_wide_movie) {
        ctx.eax = 0;
        ctx.esp += 4;
        return;
    }
    __imp__sub_001D05CB(ctx, base);
}
