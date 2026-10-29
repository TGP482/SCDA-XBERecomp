#pragma once
// The game's button icons (Magma's Button_* textures) and the Options Controls page's pad by their texture pack key,
// and the PlayStation 2 art drawn in their place, ps2\icons\<name>.png beside the exe. Written by
// tools/ps2_art/xbox_keys.py from the Xbox release this executable is made from.
#include <cstdint>

namespace gpu {

struct Ps2Icon {
    uint64_t key;
    const char* name;
};

inline constexpr Ps2Icon kPs2Icons[] = {
    {0x6baab311b36bb98dull, "cross"},  // Button_A
    {0xe5c260f3d5d0b0a9ull, "rstick"},  // button_ad
    {0xc8dc7b260cdd9577ull, "r3"},  // button_ad_clic
    {0x1b15c316c741aa69ull, "lstick"},  // button_ag
    {0xc7aa964761f46ca6ull, "l3"},  // button_ag_clic
    {0xd00e510fb779a62bull, "lstick_up"},  // button_ag_up
    {0xe12dd3fcbea69ad1ull, "lstick"},  // button_ai
    {0x209d159928a1baa8ull, "l3"},  // button_ai_clic
    {0xfd5e17e9d68863a8ull, "lstick_up"},  // button_ai_up
    {0xf405553ea8a6ce48ull, "lstick"},  // button_al
    {0x04518e0468e15c5dull, "l3"},  // button_al_clic
    {0x24eaf9ca0b615d2full, "lstick_up"},  // button_al_up
    {0x8f70f31924d09309ull, "rstick"},  // button_ar
    {0x59cf393e496fc7c0ull, "r3"},  // button_ar_clic
    {0x7f5962effbc710b7ull, "lstick"},  // button_as
    {0x9275ce9923652954ull, "l3"},  // button_as_clic
    {0x46ccfb5eec531d7full, "lstick_up"},  // button_as_up
    {0xe0eff8764b87f266ull, "circle"},  // Button_B
    {0xa24c9d341c131df0ull, "select"},  // button_back
    {0xda912222d91d83cdull, "l2"},  // Button_black
    {0x3c54e3b57825476full, "dpad_down"},  // Button_DD
    {0x68fed2564b877f4eull, "dpad_left"},  // Button_DL
    {0x8d488028c5e94af9ull, "dpad_right"},  // Button_DR
    {0x3cdadb827d7ab104ull, "r1"},  // button_dt
    {0xcb9e5eb9ff2140f4ull, "dpad_up"},  // Button_DU
    {0xc219377388325659ull, "l1"},  // button_gt
    {0x62e281a0c67d83c2ull, "l1"},  // button_it
    {0xf7cf626fd009b7c0ull, "l1"},  // button_lt
    {0x949a62887e776700ull, "r1"},  // button_rt
    {0x5604d9d9678b74e6ull, "l1"},  // button_st
    {0x273f64886bcfeab5ull, "start"},  // button_start
    {0x1f65e78c3ad5b4cbull, "r2"},  // Button_white
    {0x22fbd4cee40a2f19ull, "square"},  // Button_X
    {0x83caafaa90f6f934ull, "triangle"},  // Button_Y
    {0x038efbaa29d291f8ull, "controls"},  // Controller
};

}  // namespace gpu
