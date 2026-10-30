// Button legends on keyboard and mouse: the icon goes and the text leads with the key, "(ENTER) Select". Menus take
// Enter and Escape as A and B, play the bound keys. A change of device sets the legends on screen again. The in-game
// cinematic's prompts (Skip) are no legend: the game sets them every frame, so they are relabelled as they are
// drawn, "Skip (ESC)". The combo escape's buttons (knock, grab) have no text at all: their key's name is drawn in
// each icon's place with the row name's text.
#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "app/app.h"
#include "core/input.h"
#include "game/magma.h"
#include "xdk/hle.h"

using namespace kernel;

namespace game {
bool OnStack(uint32_t page);  // options_menu.cpp
uint32_t StackOwner(uint32_t page);  // options_menu.cpp
uint32_t DrawingPage();  // hud.cpp
uint32_t KeyboardPlayer();  // mouse.cpp
std::u16string KeyWording(std::u16string text);  // text.cpp
std::u16string XboxOneWording(std::u16string text);  // text.cpp
}

X86_EXTERN_FUNC(__imp__sub_002C64E0);  // Magma element draw (thiscall instance), hooked in hud.cpp
X86_EXTERN_FUNC(sub_002C64E0);

namespace {

// SetNavigation (thiscall owner, page, const FString& "A,Common_A,B,Common_B", type 0 panel, 1 panel_right,
// 2 panel_bar)
constexpr uint32_t kSetNavigation = 0x2915C1;
constexpr uint32_t kFind = 0x17D0E8;
constexpr uint32_t kSetText = 0x183598;
// the panels' names, by SetNavigation's type
constexpr uint32_t kPanels[] = {0x54362C, 0x543620, 0x565928};
constexpr uint32_t kBarButtons[] = {0x56591C, 0x565910, 0x565904, 0x5658F8};  // A_Button_1..4
constexpr uint32_t kBarText = 0x5658DC;
constexpr uint32_t kBarImage = 0x5658D4;
// element: +0x1C children, text +0x2C std::wstring (+4 buffer or pointer, +0x14 length, +0x18 capacity),
// image +0x3C material (+4 crc32 of its name); page +0x14 id, +0x1C layer
constexpr uint32_t kTextString = 0x2C;
constexpr uint32_t kStringData = 4;
constexpr uint32_t kStringLength = 0x14;
constexpr uint32_t kStringCapacity = 0x18;
constexpr uint32_t kStringInline = 8;  // capacity from which the characters sit behind a pointer
constexpr uint32_t kImageMaterial = 0x3C;
constexpr uint32_t kMaterialId = 4;
constexpr uint32_t kPageId = 0x14;
constexpr uint32_t kPageLayer = 0x1C;
// instance: +4 its id, +8 its widget, +0x18 bit 0 visible
constexpr uint32_t kInstanceId = 4;
constexpr uint32_t kInstanceWidget = 8;
constexpr uint32_t kVisible = 0x18;
// a box of instances: +8 begin, +0xC end
constexpr uint32_t kBoxBegin = 8;
constexpr uint32_t kBoxEnd = 0xC;
constexpr uint32_t kMaxReadChars = 0x200;  // longest text read back
constexpr uint32_t kHudLayer = 100;
constexpr uint32_t kFindId = 0x2BE900;  // thiscall(page or children, id): the child instance
constexpr uint32_t kHud = game::magma::Id("P_hud");
// name entry (name_entry.cpp): the keys type, Backspace deletes (X); the switch to the digits (Y) goes
constexpr uint32_t kNameEntry = game::magma::Id("P_VirtualKeyboard");
// P_hud's scripted event bar's prompts, names not recovered: text and icon, the text right aligned against the icon
// on its right, or (before) the icon on the text's left
struct Prompt {
    uint32_t text;
    uint32_t image;
    bool before;
};
constexpr Prompt kEventPrompts[] = {{0xDBC4BB81, 0x0E3CCC34, false}, {0x35CADAAD, 0xE032AD18, false},
                                    {0x42CDEA3B, 0x97359D8E, true}};
// combo escape rows (hud.mgm, names not recovered): the two rows, each of up to five button slots and four arrows
// placed left to right, and the row's name ("Knock", Title_18); a slot holds its button's icon
constexpr uint32_t kComboRows[] = {0x0CBEB816, 0x95B7E9AC};
constexpr uint32_t kComboArrows[] = {0x93BEFD95, 0x0AB7AC2F, 0x7DB09CB9, 0xE3D4091A};
constexpr uint32_t kComboIcon = 0xAAA53D32;
constexpr uint32_t kComboGlow = 0xEA7CB115;
constexpr uint32_t kComboName = 0xCF667FEC;
// widget: +0x10 left, +0x12 right, +0x14 top, +0x16 bottom (page units, in its area), an area instance's +0x10 x,
// +0x1C its children; vtable +8 Draw
constexpr uint32_t kRect = 0x10;
constexpr uint32_t kChildren = 0x1C;
constexpr uint32_t kDraw = 0x08;
constexpr uint32_t kRectRight = kRect + 2;
constexpr uint32_t kRectTop = kRect + 4;
constexpr uint32_t kRectBottom = kRect + 6;
constexpr uint32_t kTextWidget = 0x568290;  // vtable
// Magma screen: +0x7C the text given to the text being drawn in place of its own (a list row's)
constexpr uint32_t kScreen = 0x6A6668;
constexpr uint32_t kGivenText = 0x7C;
constexpr uint32_t kMaxWording = 0x300;
constexpr int kCharWidth = 10;  // Title_18 capitals, page units
// after a name wider than its icon, so the next arrow sits midway; names sit this much higher to line up with the
// arrows
constexpr int kNameGap = 12;
constexpr int kNameRaise = 4;

enum Code { A, B, X, Y, LT, RT, White, Black, Start, Back, Up, Down, Left, Right, Move, Look, LeftClick, RightClick };
struct Icon {
    uint32_t id;
    Code code;
    constexpr Icon(std::string_view name, Code c) : id(game::magma::Id(name)), code(c) {}
};
// other languages name the sticks and triggers by their own left (G, I, S) and right (D)
constexpr Icon kIcons[] = {
    {"Button_A", A},         {"Button_B", B},           {"Button_X", X},           {"Button_Y", Y},
    {"Button_LT", LT},       {"Button_GT", LT},         {"Button_IT", LT},         {"Button_ST", LT},
    {"Button_RT", RT},       {"Button_DT", RT},         {"Button_white", White},   {"Button_black", Black},
    {"Button_start", Start}, {"button_start", Start},   {"Button_Start", Start},   {"Button_back", Back},
    {"button_back", Back},   {"Button_Back", Back},     {"Button_DU", Up},         {"Button_DD", Down},
    {"Button_DL", Left},     {"Button_DR", Right},      {"Button_AL_up", Move},    {"button_al_up", Move},
    {"Button_AG_up", Move},  {"Button_AI_up", Move},    {"Button_AS_up", Move},    {"Button_AL", Move},
    {"button_al", Move},     {"button_ag", Move},       {"button_ai", Move},       {"button_as", Move},
    {"Button_AR", Look},     {"button_ar", Look},       {"button_ad", Look},       {"Button_AL_clic", LeftClick},
    {"Button_AG_clic", LeftClick}, {"Button_AI_clic", LeftClick}, {"Button_AS_clic", LeftClick},
    {"Button_AR_clic", RightClick}, {"Button_AD_clic", RightClick},
};

uint32_t g_block = 0;  // +0 FString, +0x10 text, +0x600 slot names
constexpr uint32_t kBlockSize = 0x800;
constexpr uint32_t kBlockText = 0x10;
constexpr uint32_t kBlockTextName = 0x600;
constexpr uint32_t kBlockImageName = 0x620;
constexpr uint32_t kMaxLineChars = 0x2F0;  // the block's text, up to the slot names
// FString: +0 its characters, +4 count with the terminator, +8 capacity
constexpr uint32_t kFStringCount = 4;
constexpr uint32_t kFStringCapacity = 8;
constexpr uint32_t kMaxActionChars = 0x100;

// keys of one letter run together ("WASD") unless split
std::string Keys(std::initializer_list<const char*> ids, bool split = false) {
    size_t count = 0;
    const input::Action* actions = input::Actions(&count);
    std::string out;
    bool single = true;
    std::string keys[4];
    size_t n = 0;
    for (const char* id : ids)
        for (size_t i = 0; i < count; i++)
            if (std::string_view(actions[i].id) == id) {
                std::string k = input::Binding(i);
                k = k.substr(0, k.find(','));
                if (k == "Escape") k = "Esc";
                for (char& c : k) c = char(std::toupper(uint8_t(c)));
                if (k.empty()) return {};
                single &= k.size() == 1;
                keys[n++] = k;
            }
    for (size_t i = 0; i < n; i++) out += (i && (split || !single) ? "/" : "") + keys[i];
    return out;
}

std::string Label(uint32_t material_id, bool menu, uint32_t page_id) {
    const Icon* icon = std::find_if(std::begin(kIcons), std::end(kIcons),
                                    [&](const Icon& i) { return i.id == material_id; });
    if (icon == std::end(kIcons)) return {};
    // the sticky camera's page: gas on fire, noise on alt fire, strafing switches cameras (input.cpp's camera remap),
    // forward and back zoom
    bool camera = page_id == game::magma::Id("P_Camera");
    // the OPSAT map: strafing steps zones (input.cpp's map remap), forward and back zoom
    bool map = page_id == game::magma::Id("P_Map");
    switch (icon->code) {
        case A:
            if (menu) return "ENTER";
            return Keys({camera ? "fire" : "interact"});
        case B:
            if (!menu) return Keys({"crouch"});
            if (page_id == game::magma::Id("P_Pause")) return "BACKSPACE";
            return "ESC";
        case X:
            if (page_id == kNameEntry) return "BACKSPACE";
            return Keys({camera ? "alt_fire" : "scope"});
        case Y: return Keys({"jump"});
        case LT: return Keys({camera || map ? "strafe_left" : "alt_fire"});
        case RT: return Keys({camera || map ? "strafe_right" : "fire"});
        case White: return Keys({"quick_inventory"});
        case Black: return Keys({"sidekick"});
        case Start: return page_id == game::magma::Id("P_Start") ? "ENTER" : Keys({"pause"});
        case Back: return Keys({"opsat"});
        case Up: return menu ? "UP" : Keys({"goggles"});
        case Down: return menu ? "DOWN" : Keys({"switch_camera"});
        case Left: return menu ? "LEFT" : Keys({"previous_vision"});
        case Right: return menu ? "RIGHT" : Keys({"next_vision"});
        case Move:
            return camera || map ? Keys({"move_forward", "move_back"}, true)
                          : Keys({"move_forward", "strafe_left", "move_back", "strafe_right"});
        case Look: return "MOUSE";
        case LeftClick: return Keys({"back_to_wall"});
        case RightClick: return Keys({"zoom"});
    }
    return {};
}

bool Block() {
    if (!g_block) g_block = mem::AllocSystem(kBlockSize);
    return g_block != 0;
}

std::u16string ReadText(uint32_t text) {
    uint32_t string = text + kTextString;
    uint32_t length = std::min<uint32_t>(R32(string + kStringLength), kMaxReadChars);
    uint32_t data = R32(string + kStringCapacity) >= kStringInline ? R32(string + kStringData) : string + kStringData;
    std::u16string out;
    for (uint32_t i = 0; data && i < length; i++) out += char16_t(R16(data + i * 2));
    return out;
}

void WriteText(X86Context& ctx, uint32_t text, const std::u16string& line) {
    size_t n = std::min<size_t>(line.size(), kMaxLineChars);
    for (size_t i = 0; i < n; i++) W16(g_block + kBlockText + uint32_t(i) * 2, uint16_t(line[i]));
    W16(g_block + kBlockText + uint32_t(n) * 2, 0);
    ctx.ecx = text + kTextString;
    CallGuest(ctx, kSetText, {g_block + kBlockText});
}

uint32_t Ansi(uint32_t at, std::string_view text) {
    for (size_t i = 0; i < text.size(); i++) G<char>(at + uint32_t(i)) = text[i];
    G<char>(at + uint32_t(text.size())) = 0;
    return at;
}

// captions moved left to start where their icon did: their own left and the one given, put back for the pad
struct Moved {
    int16_t left;
    int16_t moved;
};
std::unordered_map<uint32_t, Moved> g_moved;  // by text widget (game thread)

// false past the last slot. On the keyboard the caption leads with the key and starts where the icon did; on the pad
// a caption moved for the keyboard goes back
bool Slot(X86Context& ctx, uint32_t box, uint32_t text_name, uint32_t image_name, bool menu, uint32_t page_id,
          bool keyboard) {
    uint32_t ti = CallGuest(ctx, kFind, {box, text_name});
    uint32_t ii = CallGuest(ctx, kFind, {box, image_name});
    if (!ii) return false;
    uint32_t text = ti ? R32(ti + kInstanceWidget) : 0;
    uint32_t image = R32(ii + kInstanceWidget);
    if (!keyboard) {
        auto it = text ? g_moved.find(text) : g_moved.end();
        if (it != g_moved.end()) {
            if (int16_t(R16(text + kRect)) == it->second.moved) W16(text + kRect, uint16_t(it->second.left));
            g_moved.erase(it);
        }
        return true;
    }
    uint32_t material = image ? R32(image + kImageMaterial) : 0;
    if (!text || !material) return true;
    std::string key = Label(R32(material + kMaterialId), menu, page_id);
    if (key.empty()) return true;
    std::u16string line = u"(";
    for (char c : key) line += char16_t(uint8_t(c));
    line += u") ";
    uint32_t string = text + kTextString;
    uint32_t length = std::min<uint32_t>(R32(string + kStringLength), kMaxReadChars);
    uint32_t data = R32(string + kStringCapacity) >= kStringInline ? R32(string + kStringData) : string + kStringData;
    for (uint32_t i = 0; data && i < length; i++) line += char16_t(R16(data + i * 2));
    size_t n = std::min<size_t>(line.size(), kMaxLineChars);
    for (size_t i = 0; i < n; i++) W16(g_block + kBlockText + uint32_t(i) * 2, uint16_t(line[i]));
    W16(g_block + kBlockText + uint32_t(n) * 2, 0);
    W32(image + kImageMaterial, 0);
    ctx.ecx = text + kTextString;
    CallGuest(ctx, kSetText, {g_block + kBlockText});
    int16_t left = int16_t(R16(text + kRect));
    int16_t icon = int16_t(R16(image + kRect));
    if (icon < left) {
        if (!g_moved.contains(text)) g_moved[text] = {left, icon};
        W16(text + kRect, uint16_t(icon));
    }
    return true;
}

constexpr int kMaxSlots = 16;

void Relabel(X86Context& ctx, uint32_t page, uint32_t type, bool keyboard) {
    if (type > 2 || !Block()) return;
    uint32_t instance = CallGuest(ctx, kFind, {page, kPanels[type]});
    uint32_t panel = instance ? R32(instance + kInstanceWidget) : 0;
    uint32_t box = panel ? R32(panel + kChildren) : 0;
    if (!box) return;
    bool menu = R32(page + kPageLayer) != kHudLayer;
    uint32_t page_id = R32(page + kPageId);
    if (type == 2) {
        for (uint32_t name : kBarButtons) {
            uint32_t button = CallGuest(ctx, kFind, {box, name});
            uint32_t element = button ? R32(button + kInstanceWidget) : 0;
            uint32_t inner = element ? R32(element + kChildren) : 0;
            if (inner) Slot(ctx, inner, kBarText, kBarImage, menu, page_id, keyboard);
        }
        return;
    }
    for (int i = 0; i < kMaxSlots; i++) {
        uint32_t text = Ansi(g_block + kBlockTextName, std::to_string(i) + "_Text");
        uint32_t image = Ansi(g_block + kBlockImageName, std::to_string(i) + "_Image");
        if (!Slot(ctx, box, text, image, menu, page_id, keyboard)) break;
    }
}

// the last legends set, set again when the device changes
struct Bar {
    uint32_t owner = 0;
    uint32_t page = 0;
    uint32_t type = 0;
    std::u16string actions;
};
Bar g_bars[8];  // game thread
size_t g_next = 0;
int g_keyboard = -1;

void Remember(uint32_t owner, uint32_t page, uint32_t fstring, uint32_t type) {
    uint32_t data = R32(fstring);
    uint32_t chars = std::min<uint32_t>(R32(fstring + kFStringCount), kMaxActionChars);
    std::u16string actions;
    for (uint32_t i = 0; data && i + 1 < chars; i++) actions += char16_t(R16(data + i * 2));
    for (Bar& bar : g_bars)
        if (bar.page == page && bar.type == type) {
            bar = {owner, page, type, actions};
            return;
        }
    g_bars[g_next++ % std::size(g_bars)] = {owner, page, type, actions};
}

// the actions without Y's pair, in the block's FString
uint32_t WithoutY(uint32_t fstring) {
    uint32_t data = R32(fstring);
    uint32_t chars = std::min<uint32_t>(R32(fstring + kFStringCount), kMaxActionChars);
    std::u16string actions;
    std::u16string out;
    for (uint32_t i = 0; data && i + 1 < chars; i++) actions += char16_t(R16(data + i * 2));
    for (size_t at = 0; at < actions.size();) {
        size_t comma = std::min(actions.find(u',', at), actions.size());
        size_t end = std::min(actions.find(u',', comma + 1), actions.size());
        if (actions.compare(at, comma - at, u"Y") != 0)
            out += (out.empty() ? u"" : u",") + actions.substr(at, end - at);
        at = end + 1;
    }
    size_t n = std::min<size_t>(out.size(), kMaxLineChars);
    for (size_t i = 0; i < n; i++) W16(g_block + kBlockText + uint32_t(i) * 2, uint16_t(out[i]));
    W16(g_block + kBlockText + uint32_t(n) * 2, 0);
    W32(g_block, g_block + kBlockText);
    W32(g_block + kFStringCount, uint32_t(n + 1));
    W32(g_block + kFStringCapacity, uint32_t(n + 1));
    return g_block;
}

uint32_t Child(X86Context& ctx, uint32_t parent, uint32_t id) {
    ctx.ecx = parent;
    return CallGuest(ctx, kFindId, {id});
}

uint32_t g_combo_name = 0;  // the last combo row name drawn, its text widget (game thread)
// the row being drawn: how far right each slot and arrow goes, past the key names wider than their icon before it,
// and a wide name's glow (image: both its sides), to its middle
struct Shift {
    uint32_t instance;
    int dx;
    bool image;
};
Shift g_shifts[16];
size_t g_shift_count = 0;

std::string ComboKey(uint32_t icon) {
    uint32_t material = R32(icon + kImageMaterial);
    return material ? Label(R32(material + kMaterialId), false, kHud) : std::string();
}

// how far a slot's key name runs past its icon; a wide name's glow is shifted to its middle
int SlotOverflow(X86Context& ctx, uint32_t widget) {
    uint32_t slot = R32(widget + kChildren);
    uint32_t icon_instance = slot ? Child(ctx, slot, kComboIcon) : 0;
    uint32_t icon = icon_instance ? R32(icon_instance + kInstanceWidget) : 0;
    if (!icon) return 0;
    int width = int16_t(R16(icon + kRectRight)) - int16_t(R16(icon + kRect));
    int wide = int(ComboKey(icon).size()) * kCharWidth - width;
    uint32_t glow = wide > 0 ? Child(ctx, slot, kComboGlow) : 0;
    if (glow && g_shift_count < std::size(g_shifts)) g_shifts[g_shift_count++] = {glow, wide / 2, true};
    return wide > 0 ? wide + kNameGap : 0;
}

void ComboLayout(X86Context& ctx, uint32_t row) {
    g_shift_count = 0;
    uint32_t box = R32(row + kChildren);
    uint32_t begin = box ? R32(box + kBoxBegin) : 0;
    uint32_t end = box ? R32(box + kBoxEnd) : 0;
    if (!begin || end < begin || end - begin > 4 * 16) return;
    struct Item {
        uint32_t instance;
        int x;
        int overflow;
    };
    Item items[16];
    size_t n = 0;
    for (uint32_t it = begin; it < end; it += 4) {
        uint32_t instance = R32(it);
        uint32_t widget = instance ? R32(instance + kInstanceWidget) : 0;
        uint32_t id = instance ? R32(instance + kInstanceId) : 0;
        if (!widget || !(G<uint8_t>(instance + kVisible) & 1) || id == kComboName) continue;
        int overflow = 0;
        if (std::find(std::begin(kComboArrows), std::end(kComboArrows), id) == std::end(kComboArrows))
            overflow = SlotOverflow(ctx, widget);
        items[n++] = {instance, int16_t(R16(widget + kRect)), overflow};
    }
    std::sort(items, items + n, [](const Item& a, const Item& b) { return a.x < b.x; });
    int dx = 0;
    for (size_t i = 0; i < n; i++) {
        if (dx && g_shift_count < std::size(g_shifts)) g_shifts[g_shift_count++] = {items[i].instance, dx, false};
        dx += items[i].overflow;
    }
}

// what a relabelled prompt was: its icon and the game's text, and the text put in its place
struct Held {
    uint32_t image = 0;
    uint32_t material = 0;
    std::u16string text;
    std::u16string shown;
    bool moved = false;  // the caption's side next to the icon, moved to the icon's far side
    int16_t side = 0;
    int16_t placed = 0;
};
Held g_held[std::size(kEventPrompts)];  // game thread

// a caption right aligned against its icon, some with more text after the icon: the message box and the mission's
// end "Press (A) To Continue", the training video's "Skip Video (start)" and its "Press (button)"
struct Hint {
    uint32_t text;
    uint32_t image;
    uint32_t after;
};
constexpr Hint kHints[] = {
    {game::magma::Id("L_Press"), game::magma::Id("I_Button"), game::magma::Id("L_Continue")},
    {game::magma::Id("text_skip"), game::magma::Id("bouton_skip"), 0},
    {game::magma::Id("text"), game::magma::Id("bouton"), 0},
};
// the icon as it last came to draw: its key and sides
struct Seen {
    uint32_t tick = 0;
    std::string key;
    int l = 0;
    int r = 0;
};
Seen g_seen[std::size(kHints)];  // game thread
uint32_t g_tick = 3;  // past a hint never seen
bool g_hinting = false;

// keyboard legends while the keyboard is in use; in split screen only in the keyboard's player's menus and HUD
bool Keyboard(uint32_t page) {
    uint32_t player = game::KeyboardPlayer();
    uint32_t owner = player && page ? game::StackOwner(page) : 0;
    return input::KeyboardActive() && (!owner || owner == player);
}
constexpr int kTextChar2 = 15;  // Text_16, twice a character's width in page units
// a caption's end to its icon, between words
constexpr int kIconGap = 4;
constexpr int kWordGap = 7;

// one prompt of the event bar, relabelled on the keyboard and given back on the pad
void EventPrompt(X86Context& ctx, uint32_t box, size_t i, bool keyboard) {
    const Prompt& p = kEventPrompts[i];
    Held& h = g_held[i];
    uint32_t ti = box ? Child(ctx, box, p.text) : 0;
    uint32_t ii = box ? Child(ctx, box, p.image) : 0;
    uint32_t text = ti ? R32(ti + kInstanceWidget) : 0;
    uint32_t image = ii ? R32(ii + kInstanceWidget) : 0;
    if (!text || !image || !Block()) {
        h = {};
        return;
    }
    if (image != h.image) h = {image};
    std::u16string now = ReadText(text);
    uint32_t material = R32(image + kImageMaterial);
    // the caption's side against the icon
    uint32_t edge = p.before ? kRect : kRectRight;
    if (!keyboard) {
        // back on the pad: the icon, the game's text and the caption's side return
        if (!h.material) return;
        if (!material) W32(image + kImageMaterial, h.material);
        if (now == h.shown) WriteText(ctx, text, h.text);
        if (h.moved && int16_t(R16(text + edge)) == h.placed) W16(text + edge, uint16_t(h.side));
        h.material = 0;
        h.moved = false;
        return;
    }
    if (!material && (!h.material || now == h.shown)) return;
    // a new icon or a new text from the game is labelled again
    uint32_t icon = material ? material : h.material;
    std::string key = Label(R32(icon + kMaterialId), false, kHud);
    if (key.empty()) return;
    std::u16string label = u"(";
    for (char c : key) label += char16_t(uint8_t(c));
    label += u")";
    h.text = h.material && now == h.shown ? h.text : now;
    h.shown = p.before ? label + u" " + h.text : h.text + u" " + label;
    h.material = icon;
    W32(image + kImageMaterial, 0);
    WriteText(ctx, text, h.shown);
    // the caption takes the icon's place too: it ends where the icon did, or starts there
    if (!h.moved) {
        h.side = int16_t(R16(text + edge));
        h.placed = int16_t(R16(image + edge));
        h.moved = true;
        W16(text + edge, uint16_t(h.placed));
    }
}

}  // namespace

namespace game {

// text.cpp
std::string KeyNames(std::initializer_list<const char*> ids) { return Keys(ids); }

// hud.cpp, the bar's element draw
void EventPrompts(X86Context& ctx, uint32_t bar) {
    bool keyboard = Keyboard(game::DrawingPage());
    uint32_t element = bar ? R32(bar + kInstanceWidget) : 0;
    uint32_t box = element ? R32(element + kChildren) : 0;
    uint32_t skip = box ? Child(ctx, box, kEventPrompts[0].text) : 0;
    uint32_t skip_text = skip ? R32(skip + kInstanceWidget) : 0;
    bool skippable = skip_text && (G<uint8_t>(skip + kVisible) & 1) && !ReadText(skip_text).empty();
    if (skippable) input::SkipPromptShown();
    for (size_t i = 0; i < std::size(kEventPrompts); i++) EventPrompt(ctx, box, i, keyboard);
}

// a slot or arrow of the row drawn moved right: false when the instance is not shifted
static bool DrawShifted(X86Context& ctx, uint8_t* base, uint32_t instance, uint32_t widget) {
    for (size_t i = 0; i < g_shift_count && widget; i++) {
        if (g_shifts[i].instance != instance) continue;
        uint16_t x = R16(widget + kRect);
        uint16_t right = R16(widget + kRectRight);
        W16(widget + kRect, uint16_t(int16_t(x) + g_shifts[i].dx));
        if (g_shifts[i].image) W16(widget + kRectRight, uint16_t(int16_t(right) + g_shifts[i].dx));
        __imp__sub_002C64E0(ctx, base);
        W16(widget + kRect, x);
        W16(widget + kRectRight, right);
        return true;
    }
    return false;
}

// the row name's widget moved into the icon's box, w wide
static void PlaceKeyName(uint32_t name, uint32_t widget, int w) {
    int l = int16_t(R16(widget + kRect));
    int r = int16_t(R16(widget + kRectRight));
    int t = int16_t(R16(widget + kRectTop));
    int b = int16_t(R16(widget + kRectBottom));
    int h = int16_t(R16(name + kRectBottom)) - int16_t(R16(name + kRectTop));
    int x = w > r - l ? l : (l + r - w) / 2;
    int y = (t + b - h) / 2 - kNameRaise;
    W16(name + kRect, uint16_t(int16_t(x)));
    W16(name + kRectRight, uint16_t(int16_t(x + w)));
    W16(name + kRectTop, uint16_t(int16_t(y)));
    W16(name + kRectBottom, uint16_t(int16_t(y + h)));
}

// the icon drawn without its image, then its key's name by the row name's widget
static void DrawComboKey(X86Context& ctx, uint8_t* base, uint32_t widget, uint32_t material, const std::string& key) {
    W32(widget + kImageMaterial, 0);
    __imp__sub_002C64E0(ctx, base);
    W32(widget + kImageMaterial, material);
    uint32_t name = g_combo_name;
    uint32_t rect[2] = {R32(name + kRect), R32(name + kRectTop)};
    PlaceKeyName(name, widget, int(key.size()) * kCharWidth);
    std::u16string text = ReadText(name);
    std::u16string line;
    for (char c : key) line += char16_t(uint8_t(c));
    WriteText(ctx, name, line);
    ctx.ecx = name;
    CallGuest(ctx, R32(R32(name) + kDraw), {});
    WriteText(ctx, name, text);
    W32(name + kRect, rect[0]);
    W32(name + kRectTop, rect[1]);
}

// hud.cpp, an element about to draw inside a placed area. On the keyboard a combo icon draws without its image and
// its key's name goes in its place, drawn by the row name's widget moved into the icon's box; a name wider than the
// icon starts at its left and moves the rest of the row right: true when drawn here.
bool ComboElement(X86Context& ctx, uint8_t* base, uint32_t instance) {
    uint32_t id = R32(instance + kInstanceId);
    uint32_t widget = R32(instance + kInstanceWidget);
    if (id == kComboName) g_combo_name = widget;
    if (std::find(std::begin(kComboRows), std::end(kComboRows), id) != std::end(kComboRows)) {
        g_shift_count = 0;
        if (widget && Keyboard(game::DrawingPage())) ComboLayout(ctx, widget);
        ctx.ecx = instance;  // the row still draws, and the calls above left their own this
        return false;
    }
    if (DrawShifted(ctx, base, instance, widget)) return true;
    if (id != kComboIcon || !widget || !(G<uint8_t>(instance + kVisible) & 1) || !g_combo_name ||
        !Keyboard(game::DrawingPage()) || !Block())
        return false;
    uint32_t material = R32(widget + kImageMaterial);
    std::string key = ComboKey(widget);
    if (key.empty()) return false;
    DrawComboKey(ctx, base, widget, material, key);
    return true;
}

// hud.cpp, any element about to draw. On the keyboard a text naming the pad's buttons draws with the bound keys
// in their place, with Xbox One prompts on the pad with LB and RB for the White and Black buttons, given to the draw
// the way a list gives a row its text, so what is kept follows the device: true when drawn here.
bool WordingElement(X86Context& ctx, uint8_t* base, uint32_t instance) {
    static bool drawing = false;
    static uint32_t buffer = 0;
    uint32_t widget = R32(instance + kInstanceWidget);
    uint32_t screen = R32(kScreen);
    if (drawing || !widget || !screen || R32(widget) != kTextWidget || !(G<uint8_t>(instance + kVisible) & 1))
        return false;
    bool keyboard = Keyboard(game::DrawingPage());
    if (!keyboard && app::Get().button_prompts != app::kPromptsXboxOne) return false;
    uint32_t given = R32(screen + kGivenText);
    if (!given && R32(widget + kTextString + kStringLength) > kMaxReadChars) return false;
    std::u16string text = given ? std::u16string() : ReadText(widget);
    for (uint32_t i = 0; given && R16(given + i * 2); i++) {
        if (i == kMaxWording) return false;
        text += char16_t(R16(given + i * 2));
    }
    std::u16string keys = keyboard ? KeyWording(text) : XboxOneWording(text);
    if (keys == text || keys.size() > kMaxWording) return false;
    if (!buffer) buffer = mem::AllocSystem((kMaxWording + 1) * 2);
    if (!buffer) return false;
    for (size_t i = 0; i <= keys.size(); i++)
        W16(buffer + uint32_t(i) * 2, i < keys.size() ? uint16_t(keys[i]) : 0);
    W32(screen + kGivenText, buffer);
    drawing = true;
    ctx.ecx = instance;
    sub_002C64E0(ctx, base);
    drawing = false;
    W32(screen + kGivenText, given);
    return true;
}

// a hint's caption drawn with its key, or the text after its icon closed up to it: false when it has no text
static bool DrawHintText(X86Context& ctx, uint8_t* base, uint32_t instance, uint32_t widget, const Hint& h,
                         const Seen& s, bool after) {
    std::u16string text = ReadText(widget);
    if (text.empty()) return false;
    int w = int(s.key.size() + 3) * kTextChar2 / 2;
    int grow = h.after ? (w - (s.r - s.l)) / 2 : 0;
    int end = h.after ? s.l - kIconGap - grow + w : s.r;  // of the caption with its key
    int l = int16_t(R16(widget + kRect));
    int r = int16_t(R16(widget + kRectRight));
    uint32_t sides = R32(widget + kRect);
    W16(widget + kRect, uint16_t(int16_t(after ? end + kWordGap : l - w)));
    W16(widget + kRectRight, uint16_t(int16_t(after ? r + end + kWordGap - l : end)));
    if (!after) {
        std::u16string line = text + u" (";
        for (char c : s.key) line += char16_t(uint8_t(c));
        WriteText(ctx, widget, line + u")");
    }
    g_hinting = true;
    ctx.ecx = instance;
    sub_002C64E0(ctx, base);
    g_hinting = false;
    if (!after) WriteText(ctx, widget, text);
    W32(widget + kRect, sides);
    return true;
}

// hud.cpp, any element about to draw. On the keyboard a hint's icon goes and its key joins the caption, which
// ends where the icon did; with text after the icon the two close up around the key, centred as before: true when
// handled here.
bool HintElement(X86Context& ctx, uint8_t* base, uint32_t instance) {
    uint32_t id = R32(instance + kInstanceId);
    uint32_t widget = R32(instance + kInstanceWidget);
    if (g_hinting || !widget || !(G<uint8_t>(instance + kVisible) & 1) || !Keyboard(game::DrawingPage()))
        return false;
    for (size_t i = 0; i < std::size(kHints); i++) {
        const Hint& h = kHints[i];
        Seen& s = g_seen[i];
        if (id == h.image) {
            uint32_t material = R32(widget + kImageMaterial);
            std::string key = material ? Label(R32(material + kMaterialId), input::InMenu(), 0) : std::string();
            if (key.empty()) return false;
            s = {g_tick, key, int16_t(R16(widget + kRect)), int16_t(R16(widget + kRectRight))};
            ctx.esp += 4;  // the icon is not drawn
            return true;
        }
        bool after = h.after && id == h.after;
        if ((id != h.text && !after) || g_tick - s.tick > 2 || !Block()) continue;
        return DrawHintText(ctx, base, instance, widget, h, s, after);
    }
    return false;
}

// game thread, every input update
void PromptsTick(X86Context& ctx) {
    g_tick++;
    int keyboard = input::KeyboardActive();
    if (keyboard == g_keyboard) return;
    bool first = g_keyboard < 0;
    g_keyboard = keyboard;
    if (first || !Block()) return;
    for (const Bar& bar : g_bars) {
        if (!bar.page || !OnStack(bar.page)) continue;
        size_t n = std::min<size_t>(bar.actions.size(), kMaxLineChars);
        for (size_t i = 0; i < n; i++) W16(g_block + kBlockText + uint32_t(i) * 2, uint16_t(bar.actions[i]));
        W16(g_block + kBlockText + uint32_t(n) * 2, 0);
        W32(g_block, g_block + kBlockText);
        W32(g_block + kFStringCount, uint32_t(n + 1));
        W32(g_block + kFStringCapacity, uint32_t(n + 1));
        ctx.ecx = bar.owner;
        CallGuest(ctx, kSetNavigation, {bar.page, g_block, bar.type});
    }
}

}  // namespace game

// SetNavigation: remembered for a device change, and relabelled for the keyboard
X86_EXTERN_FUNC(__imp__sub_002915C1);
X86_FUNC(sub_002915C1) {
    uint32_t owner = ctx.ecx;
    uint32_t page = R32(ctx.esp + 4);
    uint32_t actions = R32(ctx.esp + 8);
    uint32_t type = R32(ctx.esp + 12);
    if (page && actions && actions != g_block) Remember(owner, page, actions, type);
    if (page && actions && R32(page + kPageId) == kNameEntry && Keyboard(page) && Block())
        W32(ctx.esp + 8, WithoutY(actions));
    __imp__sub_002915C1(ctx, base);
    if (!page || (!Keyboard(page) && g_moved.empty())) return;
    uint32_t eax = ctx.eax;
    Relabel(ctx, page, type, Keyboard(page));
    ctx.eax = eax;
}
