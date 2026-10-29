#pragma once
// The Options Controls page laid out as the PlayStation 2 game's, the DualShock 2 in the Xbox pad's place: each
// element's rect on the Xbox page and the one it takes with PlayStation 2 prompts (left, right, top, bottom, in its
// parent's units; a callout piece the PS2 page has no use for gets an empty one). Written by
// tools/ps2_art/controls.py from the Xbox release this executable is made from.
#include <cstdint>

namespace game {

struct Ps2Rect {
    uint32_t id;
    int16_t xbox[4];
    int16_t ps2[4];
};

inline constexpr uint32_t kControlsPage = 0x91CC5199;

inline constexpr Ps2Rect kPs2Controls[] = {
    {0x1BC729EF, {55, 206, 163, 200}, {50, 210, 92, 129}},  // OPSAT / PDA MENU
    {0x6CC01979, {56, 190, 202, 240}, {50, 210, 114, 148}},  // WHISTLE / COOP ACTION (by PAUSE MENU)
    {0xFB12C0F1, {55, 235, 88, 125}, {50, 210, 152, 189}},  // KO ATTACK
    {0xFBB93D84, {55, 181, 322, 357}, {50, 210, 192, 227}},  // THERMAL VISION
    {0x8CBE0D12, {55, 201, 362, 396}, {50, 210, 231, 265}},  // EMF
    {0x15B75CA8, {53, 176, 242, 278}, {50, 210, 269, 305}},  // NIGHT VISION
    {0x62B06C3E, {55, 177, 282, 318}, {50, 210, 307, 343}},  // STICKY CAMERA
    {0xFC7F04E8, {55, 222, 127, 158}, {50, 210, 346, 396}},  // MOVEMENT / BACK TO WALL / SWITCH HAND
    {0xFCD4F99D, {444, 585, 282, 316}, {431, 590, 92, 130}},  // PAUSE MENU (by WHISTLE / COOP ACTION)
    {0x65DDA827, {442, 585, 322, 361}, {431, 590, 114, 153}},  // QUICK INVENTORY
    {0x621B914B, {390, 585, 88, 123}, {431, 590, 152, 187}},  // USE EQUIPPED / LETHAL ATTACK
    {0x65765552, {407, 585, 127, 162}, {431, 590, 192, 227}},  // EQUIP ITEM
    {0x127165C4, {418, 585, 177, 198}, {431, 590, 231, 252}},  // JUMP
    {0x8C15F067, {431, 585, 202, 236}, {431, 590, 269, 303}},  // CROUCH / STAND
    {0x8B78347E, {436, 585, 242, 277}, {431, 590, 307, 342}},  // INTERACT
    {0x12DA98B1, {416, 585, 362, 401}, {431, 590, 346, 396}},  // CAMERA / EEV / ZOOM
    {0x4AD9856A, {350, 351, 94, 170}, {320, 410, 98, 99}},  // callout
    {0x3DDEB5FC, {481, 482, 94, 169}, {409, 410, 98, 234}},  // callout
    {0xA4D7E446, {345, 350, 94, 95}, {320, 366, 120, 121}},  // callout
    {0xD3D0D4D0, {482, 494, 94, 95}, {365, 366, 120, 185}},  // callout
    {0x4DB44173, {508, 533, 208, 209}, {320, 334, 158, 159}},  // callout
    {0x44020D58, {470, 512, 132, 133}, {333, 334, 158, 194}},  // callout
    {0x33053DCE, {469, 470, 132, 218}, {333, 347, 193, 194}},  // callout
    {0x40EDBFA3, {341, 350, 217, 218}, {320, 376, 198, 199}},  // callout
    {0x37EA8F35, {488, 531, 237, 238}, {375, 376, 198, 230}},  // callout
    {0xD9E4EE19, {306, 338, 260, 261}, {320, 330, 237, 238}},  // callout
    {0x47807BBA, {294, 369, 273, 274}, {329, 330, 224, 238}},  // callout
    {0x30874B2C, {293, 294, 247, 274}, {329, 360, 224, 225}},  // callout
    {0xA98E1A96, {287, 381, 287, 288}, {320, 355, 275, 276}},  // callout
    {0xDE892A00, {394, 395, 260, 328}, {354, 355, 240, 276}},  // callout
    {0x4E363791, {292, 394, 327, 328}, {320, 366, 313, 314}},  // callout
    {0x39310707, {417, 418, 248, 367}, {365, 366, 252, 314}},  // callout
    {0x6BC0EC60, {382, 418, 247, 248}, {320, 396, 352, 353}},  // callout
    {0x1CC7DCF6, {311, 418, 367, 368}, {395, 396, 280, 353}},  // callout
    {0x85CE8D4C, {452, 453, 261, 367}, {445, 536, 98, 99}},  // callout
    {0xF2C9BDDA, {452, 521, 367, 368}, {445, 446, 98, 234}},  // callout
    {0x6CAD2879, {491, 492, 270, 327}, {491, 536, 120, 121}},  // callout
    {0x82A34955, {492, 545, 326, 327}, {491, 492, 120, 185}},  // callout
    {0xF5A479C3, {381, 382, 272, 288}, {522, 536, 158, 159}},  // callout
    {0x651B6452, {510, 548, 287, 288}, {522, 523, 158, 194}},  // callout
    {0xA3BA205F, {332, 341, 131, 132}, {509, 523, 193, 194}},  // callout
    {0x1BAA18EF, {341, 342, 131, 217}, {475, 536, 198, 199}},  // callout
    {0x121C54C4, {317, 325, 168, 169}, {475, 476, 198, 228}},  // callout
    {0x72DBDD21, {325, 326, 168, 245}, {526, 536, 237, 238}},  // callout
    {0x05DCEDB7, {299, 305, 207, 208}, {526, 527, 219, 238}},  // callout
    {0x9CD5BC0D, {305, 306, 207, 261}, {499, 527, 219, 220}},  // callout
    {0xEBD28C9B, {287, 294, 246, 247}, {508, 536, 275, 276}},  // callout
    {0x75B61938, {369, 370, 259, 274}, {508, 509, 243, 276}},  // callout
    {0x02B129AE, {509, 510, 255, 288}, {491, 536, 313, 314}},  // callout
    {0xAEE3DE8F, {490, 522, 181, 182}, {491, 492, 260, 314}},  // callout
    {0x9BB87814, {489, 490, 181, 199}, {460, 536, 352, 353}},  // callout
    {0xECBF4882, {508, 507, 208, 218}, {460, 461, 280, 353}},  // callout
    {0x7C005513, {531, 532, 237, 247}, {0, 0, 0, 0}},  // callout
    {0x0B076585, {532, 541, 246, 247}, {0, 0, 0, 0}},  // callout
    {0x03AF654A, {290, 546, 142, 398}, {319, 535, 138, 354}},  // pad
};

// labels that change sides draw with the text of the one whose alignment they take: element, its text's element
struct Ps2Text {
    uint32_t id;
    uint32_t text;
};

inline constexpr Ps2Text kPs2ControlsTexts[] = {
    {0x6CC01979, 0xFCD4F99D},  // WHISTLE / COOP ACTION
    {0xFCD4F99D, 0x6CC01979},  // PAUSE MENU
};

// labels that say more on the PS2 page: the text's element, the game's language, what follows the text
struct Ps2More {
    uint32_t text;
    const char* language;
    const char16_t* more;
};

inline constexpr Ps2More kPs2ControlsMore[] = {
    {0xFC7F04E8, "int", u" / SWITCH HAND"},  // MOVEMENT / BACK TO WALL
    {0xFC7F04E8, "fra", u" / CHANGER DE MAIN"},  // MOVEMENT / BACK TO WALL
    {0xFC7F04E8, "deu", u" / HAND WECHSELN"},  // MOVEMENT / BACK TO WALL
    {0xFC7F04E8, "ita", u" / CAMBIA MANO"},  // MOVEMENT / BACK TO WALL
    {0xFC7F04E8, "esp", u" / CAMBIAR DE MANO"},  // MOVEMENT / BACK TO WALL
    {0x12DA98B1, "int", u" / ZOOM"},  // CAMERA / EEV
    {0x12DA98B1, "fra", u" / ZOOM"},  // CAMERA / EEV
    {0x12DA98B1, "deu", u" / ZOOM"},  // CAMERA / EEV
    {0x12DA98B1, "ita", u" / AVVICINA"},  // CAMERA / EEV
    {0x12DA98B1, "esp", u" / ZOOM"},  // CAMERA / EEV
};

// The same layout as drawn into a picture (the controller screen a new game starts with), in page units: each label's
// text (OPTIONS ControllerAction[n] and what kPs2ControlsMore has for its element), its box and whether it is right
// aligned (the left side), the callouts' pieces and the pad's square
struct Ps2Label {
    int action;
    uint32_t text;
    bool right;
    int16_t box[4];
};

inline constexpr Ps2Label kPs2ControlsLabels[] = {
    {8, 0x1BC729EF, true, {50, 210, 92, 129}},  // OPSAT / PDA MENU
    {16, 0xFCD4F99D, true, {50, 210, 114, 148}},  // WHISTLE / COOP ACTION
    {2, 0xFB12C0F1, true, {50, 210, 152, 189}},  // KO ATTACK
    {12, 0xFBB93D84, true, {50, 210, 192, 227}},  // THERMAL VISION
    {13, 0x8CBE0D12, true, {50, 210, 231, 265}},  // EMF
    {10, 0x15B75CA8, true, {50, 210, 269, 305}},  // NIGHT VISION
    {11, 0x62B06C3E, true, {50, 210, 307, 343}},  // STICKY CAMERA
    {6, 0xFC7F04E8, true, {50, 210, 346, 396}},  // MOVEMENT / BACK TO WALL
    {9, 0x6CC01979, false, {431, 590, 92, 130}},  // PAUSE MENU
    {15, 0x65DDA827, false, {431, 590, 114, 153}},  // QUICK INVENTORY
    {1, 0x621B914B, false, {431, 590, 152, 187}},  // USE EQUIPPED / LETHAL ATTACK
    {5, 0x65765552, false, {431, 590, 192, 227}},  // EQUIP ITEM
    {4, 0x127165C4, false, {431, 590, 231, 252}},  // JUMP
    {3, 0x8C15F067, false, {431, 590, 269, 303}},  // CROUCH / STAND
    {7, 0x8B78347E, false, {431, 590, 307, 342}},  // INTERACT
    {14, 0x12DA98B1, false, {431, 590, 346, 396}},  // CAMERA / EEV
};

inline constexpr int16_t kPs2ControlsLines[][4] = {
    {213, 303, 103, 104},
    {302, 303, 103, 239},
    {213, 259, 125, 126},
    {258, 259, 125, 190},
    {213, 227, 163, 164},
    {226, 227, 163, 199},
    {226, 240, 198, 199},
    {213, 269, 203, 204},
    {268, 269, 203, 235},
    {213, 223, 242, 243},
    {222, 223, 229, 243},
    {222, 253, 229, 230},
    {213, 248, 280, 281},
    {247, 248, 245, 281},
    {213, 259, 318, 319},
    {258, 259, 257, 319},
    {213, 289, 357, 358},
    {288, 289, 285, 358},
    {338, 429, 103, 104},
    {338, 339, 103, 239},
    {384, 429, 125, 126},
    {384, 385, 125, 190},
    {415, 429, 163, 164},
    {415, 416, 163, 199},
    {402, 416, 198, 199},
    {368, 429, 203, 204},
    {368, 369, 203, 233},
    {419, 429, 242, 243},
    {419, 420, 224, 243},
    {392, 420, 224, 225},
    {401, 429, 280, 281},
    {401, 402, 248, 281},
    {384, 429, 318, 319},
    {384, 385, 265, 319},
    {353, 429, 357, 358},
    {353, 354, 285, 358},
};

inline constexpr int16_t kPs2ControlsPad[4] = {212, 428, 143, 359};

}  // namespace game
