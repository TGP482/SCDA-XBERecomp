// Exit on the game mode menu (P_OnlineOffline), as SCCT's PC version has: a fifth list item after Solo, Coop, Versus
// and Credits. Picking it opens the game's own OK / Cancel box, and OK closes the window. Back (B, Escape) on that
// page opens the same box instead of returning to the title screen; Cancel leaves the selection where it was.
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include "linux/win32_compat.h"
#endif

#include "app/app.h"
#include "app/translations.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// title menu (vtable 0x541CD8): +0x24 message boxes, +0x2758 the base its press handler is called on,
// +0x2788 P_OnlineOffline
constexpr uint32_t kPressBase = 0x2758, kGameModePage = 0x2788;
constexpr uint32_t kFindList = 0x1A60E6;   // cdecl (page, const char* name)
constexpr uint32_t kIsElement = 0x1A6090;  // cdecl (element, const char* name)
constexpr uint32_t kAddItem = 0x2C5160;    // thiscall list (const TCHAR* text, 0)
constexpr uint32_t kItemCount = 0x183DF9;  // thiscall list +0x5C, returns a short
constexpr uint32_t kListName = 0x541C28;   // "Lst_OfflineOnline"
constexpr uint32_t kIsPage = 0x28FA86;     // thiscall menu (const char* page name): the page is up
constexpr uint32_t kGameModeName = 0x541A98;  // "P_OnlineOffline"
constexpr uint32_t kListItems = 0x5C, kListSelection = 0x9C;  // items, selected index (short)
// message box (thiscall menu): id, FString* title, FString* text, 4, 1, listener, FString*, FString*, 0, 0, 0,
// FString*; the empty strings give the OK / Cancel buttons. A listener's vtable +0 is OnAccept(id), +4 OnCancel(id).
constexpr uint32_t kMessageBox = 0x29388A;
// the profile menu's listener vtable: its OnAccept (sub_001ACEAB) and OnCancel act only on ids 0x4A-0x4D
constexpr uint32_t kListenerVtable = 0x546EC8;
constexpr uint32_t kExitId = 0x7E;
// the select sound the handler's other items end with (00186ADF): sub_001838BD(menu) gives the object that plays it,
// its vtable +0x78 plays (sound, menu +4's +0x5C, 3, 0, 0, 0, 0), the sound at +0x2AC of sub_0017B720(menu)
constexpr uint32_t kSoundPlayer = 0x1838BD, kSoundOwner = 0x17B720;
constexpr uint32_t kPlay = 0x78, kSelectSound = 0x2AC, kMenuPlayer = 0x04, kPlayerSlot = 0x5C;

// block: +0 listener, +0x10 FStrings (title, text, empty), +0x40 text
uint32_t g_block = 0;
uint32_t g_listener = 0, g_title = 0, g_text = 0, g_empty = 0, g_item = 0;
uint32_t g_list = 0;   // the list the item went into
int g_exit_index = -1;
bool g_asking = false;  // the box is up: back reaches the page handler under it too

uint32_t Wide(uint32_t& at, std::u16string_view text) {
    uint32_t start = at;
    for (char16_t c : text) {
        W16(at, uint16_t(c));
        at += 2;
    }
    W16(at, 0);
    at = (at + 2 + 3) & ~3u;
    return start;
}

void FString(uint32_t fs, uint32_t data, uint32_t chars) {
    W32(fs, data);
    W32(fs + 4, data ? chars + 1 : 0);
    W32(fs + 8, data ? chars + 1 : 0);
}

bool Setup() {
    if (g_block) return true;
    uint32_t block = mem::AllocSystem(0x100);
    if (!block) return false;
    g_listener = block;
    W32(g_listener, kListenerVtable);
    g_title = block + 0x10, g_text = block + 0x1C, g_empty = block + 0x28;
    uint32_t at = block + 0x40;
    auto local = [](std::string_view english) {
        const char16_t* text = app::Translation(english);
        return text ? std::u16string(text) : std::u16string(english.begin(), english.end());
    };
    std::u16string title = local("Quit Game"), text = local("Are you sure you want to exit?"), item = local("Exit");
    FString(g_title, Wide(at, title), uint32_t(title.size()));
    FString(g_text, Wide(at, text), uint32_t(text.size()));
    FString(g_empty, 0, 0);
    g_item = Wide(at, item);
    g_block = block;
    return true;
}

void Ask(X86Context& ctx, uint32_t menu) {
    LOGI("exit: asking");
    g_asking = true;
    ctx.ecx = menu;
    if (uint32_t player = CallGuest(ctx, kSoundPlayer, {})) {
        uint32_t owner = CallGuest(ctx, kSoundOwner, {});
        ctx.ecx = player;
        CallGuest(ctx, R32(R32(player) + kPlay),
                  {R32(owner + kSelectSound), R32(R32(menu + kMenuPlayer) + kPlayerSlot), 3, 0, 0, 0, 0});
        ctx.ecx = menu;
    }
    CallGuest(ctx, kMessageBox, {kExitId, g_title, g_text, 4, 1, g_listener, g_empty, g_empty, 0, 0, 0, g_empty});
}

}  // namespace

// title menu page setup: fills the game mode list
X86_EXTERN_FUNC(__imp__sub_00185C2F);
X86_FUNC(sub_00185C2F) {
    uint32_t menu = ctx.ecx;
    __imp__sub_00185C2F(ctx, base);
    uint32_t page = R32(menu + kGameModePage);
    uint32_t list = page ? CallGuest(ctx, kFindList, {page, kListName}) : 0;
    if (list && list != g_list && Setup()) {
        ctx.ecx = list + kListItems;
        int count = int16_t(CallGuest(ctx, kItemCount, {}));
        ctx.ecx = list;
        CallGuest(ctx, kAddItem, {g_item, 0});
        g_list = list;
        g_exit_index = count;
        LOGI("exit: added to the game mode list as item %d", count);
    }
}

// title menu press handler (thiscall on its base at +0x2758, the pressed element, ret 4, returns handled): on the
// game mode list 0 is Solo, 1 Coop, 2 Versus and anything else Credits
X86_EXTERN_FUNC(__imp__sub_0018697A);
X86_FUNC(sub_0018697A) {
    uint32_t menu = ctx.ecx - kPressBase;
    uint32_t page = R32(menu + kGameModePage);
    uint32_t list = page ? CallGuest(ctx, kFindList, {page, kListName}) : 0;
    if (list && list == g_list && int16_t(R16(list + kListSelection)) == g_exit_index &&
        CallGuest(ctx, kIsElement, {R32(ctx.esp + 4), kListName})) {
        Ask(ctx, menu);
        ctx.eax = 1;
        ctx.esp += 8;  // ret 4
        return;
    }
    __imp__sub_0018697A(ctx, base);
}

// title menu back handler (thiscall on its base at +0x2758, one argument, ret 4, returns 0 when a page took it): on
// P_OnlineOffline it goes back to the title screen (sub_00183983)
X86_EXTERN_FUNC(__imp__sub_00186B5D);
X86_FUNC(sub_00186B5D) {
    uint32_t menu = ctx.ecx - kPressBase;
    ctx.ecx = menu;
    bool game_mode = g_block && CallGuest(ctx, kIsPage, {kGameModeName}) & 0xFF;
    ctx.ecx = menu + kPressBase;
    if (!game_mode) return __imp__sub_00186B5D(ctx, base);
    if (!g_asking) Ask(ctx, menu);
    ctx.eax = 0;
    ctx.esp += 8;  // ret 4
}

// OnAccept of the profile menu's listener, which the exit box borrows
X86_EXTERN_FUNC(__imp__sub_001ACEAB);
X86_FUNC(sub_001ACEAB) {
    if (g_listener && ctx.ecx == g_listener && R32(ctx.esp + 4) == kExitId) {
        LOGI("exit: closing the window");
        g_asking = false;
        PostMessageW(static_cast<HWND>(app::Window()), WM_CLOSE, 0, 0);
        ctx.esp += 8;  // ret 4
        return;
    }
    __imp__sub_001ACEAB(ctx, base);
}


// OnCancel of the same listener
X86_EXTERN_FUNC(__imp__sub_001AD175);
X86_FUNC(sub_001AD175) {
    if (g_listener && ctx.ecx == g_listener && R32(ctx.esp + 4) == kExitId) {
        LOGI("exit: cancelled");
        g_asking = false;
    }
    __imp__sub_001AD175(ctx, base);
}
