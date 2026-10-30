// Game text replaced by key: Xbox wording for the PC, and System Link called LAN everywhere the game shows it (the
// co-op button, its page titles and the lobby all read [COMMON] MainTitleLan). The other languages get theirs from
// app::Translation, keyed by the English text.
// The storage messages never show while the hard disk reports its fixed free space; their "Select OK to free %i
// blocks" tail went to the dashboard, so it is dropped.
// On keyboard and mouse the English tutorial text names the bound keys in place of the pad's buttons, as it is
// drawn (prompts.cpp), so it follows the device. With Xbox One button prompts the pad's text, in every language, calls
// the White and Black buttons LB and RB, the bumpers that press them.
#include <algorithm>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "app/translations.h"
#include "xdk/hle.h"

using namespace kernel;

namespace game {
std::string KeyNames(std::initializer_list<const char*> ids);  // prompts.cpp
std::u16string KeyWording(std::u16string text);
std::u16string XboxOneWording(std::u16string text);
}

namespace {

// Localize(section, key, package, ...), cdecl, returns one of its own TCHAR buffers (0x400 characters)
struct Text {
    std::u16string_view key, text, section;
};
constexpr Text kTexts[] = {
    {u"MainTitleLan", u"LAN"},
    {u"SaveWarning", u"Please don't close the game or turn off your PC."},
    {u"ProfileErrorCreateMsg", u"Your PC doesn't have enough free storage to save a profile."},
    {u"ErrorSavingMsg", u"Your PC doesn't have enough free storage to save the checkpoint."},
    {u"SavedMissingSpaceMsg", u"Your PC doesn't have enough free storage to save the checkpoints."},
    // the game has these two the wrong way round
    {u"HowToUseMe", u"USE: Press right on the D-pad.", u"THERMO"},
    {u"HowToUseMe", u"USE: Press left on the D-pad.", u"NIGHTVISION"},
};

// applied in order. {action} is its key, @ is press or click by the key after it
struct Swap {
    std::string_view from, to;
};
constexpr Swap kSwaps[] = {
    {"press X to make noise or A to release gas", "@ {alt_fire} to make noise or {fire} to release gas"},
    {"press START to begin", "press (ENTER) to begin"},
    {"press A to continue", "press (ENTER) to continue"},
    {"trigger button", "trigger"},
    {"the left trigger", "left trigger"},
    {"the right trigger", "right trigger"},
    {"pull and hold left trigger", "@ and hold {alt_fire}"},
    {"pull left trigger", "@ {alt_fire}"},
    {"pull right trigger", "@ {fire}"},
    {"left trigger", "{alt_fire}"},
    {"right trigger", "{fire}"},
    {"hold the trigger", "hold {alt_fire}"},
    {"the triggers", "{alt_fire} and {fire}"},
    {"clicking on the left thumbstick", "@ing {back_to_wall}"},
    {"clicking the left thumbstick", "@ing {back_to_wall}"},
    {"click on the right thumbstick", "@ {zoom}"},
    {"click the right thumbstick", "@ {zoom}"},
    {"click right thumbstick", "@ {zoom}"},
    {"rotate the left thumbstick", "press {move}"},
    {"move the left thumbstick around its periphery", "press {move}"},
    {"left thumbstick", "{move}"},
    {"press left on the D-pad", "@ {previous_vision}"},
    {"press right on the D-pad", "@ {next_vision}"},
    {"press on the left D-pad", "@ {previous_vision}"},
    {"press up on the D-pad", "@ {goggles}"},
    {"press up on the directional pad", "@ {goggles}"},
    {"press on the Y button", "@ {jump}"},
    {"the BLACK button to whistle", "{whistle} to whistle"},
    {"the BLACK button", "{sidekick}"},
    {"the coop button", "{sidekick}"},
    {"coop button", "{sidekick}"},
    {"the WHITE button", "{quick_inventory}"},
    {"the START button", "{pause}"},
    {"the BACK button", "{opsat}"},
    {"the A button", "{interact}"},
    {"the action button", "{interact}"},
    {"the jump button", "{jump}"},
    {"the primary or secondary fire buttons", "{fire} or {alt_fire}"},
    {"press A", "press {interact}"},
    {"pressing A", "pressing {interact}"},
    {"hold B", "hold {crouch}"},
    {"press Y", "press {jump}"},
    {"X to equip", "{scope} to equip"},
};

// the White and Black buttons' colour words in each language's phrase for them, the only place the game writes them
// in capitals; Spanish's botón is past ASCII, so its colour words stand alone
constexpr Swap kXboxOneSwaps[] = {
    {"WHITE button", "LB button"},     {"BLACK button", "RB button"},       // int
    {"touche BLANCHE", "touche LB"},   {"touche NOIRE", "touche RB"},       // fra
    {"WEISSE Taste", "LB-Taste"},      {"SCHWARZE Taste", "RB-Taste"},      // deu
    {"pulsante BIANCO", "pulsante LB"}, {"pulsante NERO", "pulsante RB"},  // ita
    {"BLANCO", "LB"},                  {"NEGRO", "RB"},                     // esp
};

// letters in either case
bool Equals(uint32_t text, std::u16string_view want) {
    if (!text) return false;
    for (size_t i = 0; i < want.size(); i++)
        if ((R16(text + uint32_t(i) * 2) | 0x20) != (want[i] | 0x20)) return false;
    return !R16(text + uint32_t(want.size()) * 2);
}

bool Letter(char16_t c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; }

// empty when a key is unbound; caps for a text in capitals
std::u16string Expand(std::string_view to, bool upper, bool caps) {
    std::u16string out;
    size_t verb = std::u16string::npos;
    for (size_t i = 0; i < to.size(); i++) {
        if (to[i] == '@') {
            verb = out.size();
        } else if (to[i] != '{') {
            out += char16_t(to[i]);
        } else {
            size_t end = to.find('}', i);
            std::string id(to.substr(i + 1, end - i - 1));
            std::string key = id == "move" ? game::KeyNames({"move_forward", "strafe_left", "move_back", "strafe_right"})
                                           : game::KeyNames({id.c_str()});
            if (key.empty()) return {};
            if (verb != std::u16string::npos) out.insert(verb, key.ends_with("MB") || key.starts_with("MOUSE") ? u"click" : u"press");
            verb = std::u16string::npos;
            out += u'(' + std::u16string(key.begin(), key.end()) + u')';
            i = end;
        }
    }
    for (size_t i = 0; i < (caps ? out.size() : upper); i++)
        if (out[i] >= 'a' && out[i] <= 'z') out[i] -= 0x20;
    return out;
}

// Whole words; small letters match capitals too, as the message box shows its text
std::u16string Reword(std::u16string text, std::span<const Swap> swaps) {
    bool caps = std::none_of(text.begin(), text.end(), [](char16_t c) { return c >= 'a' && c <= 'z'; });
    auto same = [](char p, char16_t c) { return c == char16_t(p) || (p >= 'a' && p <= 'z' && c == char16_t(p - 0x20)); };
    for (const Swap& s : swaps)
        for (size_t at = 0; at + s.from.size() <= text.size(); at++) {
            size_t end = at + s.from.size();
            if ((at && Letter(text[at - 1])) || (end < text.size() && Letter(text[end]))) continue;
            if (!std::equal(s.from.begin(), s.from.end(), text.begin() + at, same)) continue;
            std::u16string to = Expand(s.to, text[at] < 'a', caps);
            if (to.empty()) continue;
            text.replace(at, s.from.size(), to);
            at += to.size() - 1;
        }
    return text;
}

}  // namespace

// prompts.cpp, a text about to draw on the keyboard
std::u16string game::KeyWording(std::u16string text) { return Reword(std::move(text), kSwaps); }

// prompts.cpp, a text about to draw on the pad with Xbox One prompts
std::u16string game::XboxOneWording(std::u16string text) { return Reword(std::move(text), kXboxOneSwaps); }

X86_EXTERN_FUNC(__imp__sub_00070FC9);
X86_FUNC(sub_00070FC9) {
    uint32_t section = R32(ctx.esp + 4), key = R32(ctx.esp + 8);
    const Text* found = nullptr;
    for (auto& t : kTexts)
        if (Equals(key, t.key) && (t.section.empty() || Equals(section, t.section))) found = &t;
    __imp__sub_00070FC9(ctx, base);
    if (!found || !ctx.eax) return;
    std::u16string_view text = found->text;
    if (const char16_t* local = app::Translation(std::string(text.begin(), text.end()))) text = local;
    for (size_t i = 0; i <= text.size(); i++) W16(ctx.eax + uint32_t(i) * 2, i < text.size() ? text[i] : 0);
}
