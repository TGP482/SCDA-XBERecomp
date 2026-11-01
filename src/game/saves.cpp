// Allow loading authentic Xbox console and xemu saves without manual re-signing.
//
// On an authentic Xbox console, saves (profile.sav, single*.sav, multi*.sav) are signed
// with an HMAC-SHA1 calculation keyed by the console's unique XboxSignatureKey and XboxHDKey.
// In the static recompilation, these kernel keys are fixed to zeroes.
//
// When loading saves transferred from a physical Xbox or xemu, the file manager's save
// verification routine (vtable[0x54] / sub_0003F683) computes the HMAC with the zero key
// and compares it against the console-signed trailing 20 bytes via memcmp. The mismatch causes
// sub_0003F683 to return 0, prompting "[PROFILES] ProfileErrorLoadMsg" ("This profile appears
// to be damaged and cannot be used").
//
// sub_0003F683 first verifies file existence via vtable[0x4C], opens the file via vtable[0x08],
// and checks that its size is at least 20 bytes. If any of those checks fail (e.g. an empty save
// slot), sub_0003F683 exits immediately without calling XCalculateSignatureBegin (sub_000534B8).
// Only when the file exists and is readable does it call XCalculateSignatureBegin to compute
// the digest.
//
// By tracking whether XCalculateSignatureBegin was called during sub_0003F683, we distinguish
// missing/corrupted files (which must return 0) from authentic console saves whose signature
// simply used the console's key (which we accept by returning 1).
#include <string>

#include "xdk/hle.h"

using namespace kernel;

namespace {

thread_local bool g_verifying_save = false;
thread_local bool g_save_signature_begun = false;

std::string ReadPath(uint32_t va) {
    if (!va) return {};
    // Check for UTF-16: non-zero character followed by null byte
    if (G<uint8_t>(va) != 0 && G<uint8_t>(va + 1) == 0 &&
        G<uint8_t>(va + 2) != 0 && G<uint8_t>(va + 3) == 0) {
        std::string s;
        for (uint32_t i = 0; i < 512; i += 2) {
            char16_t c = G<char16_t>(va + i);
            if (!c) break;
            s += (c < 128) ? char(c) : '?';
        }
        return s;
    }
    return ReadCString(va);
}

}  // namespace

// XCalculateSignatureBegin
X86_EXTERN_FUNC(__imp__sub_000534B8);
X86_FUNC(sub_000534B8) {
    if (g_verifying_save) {
        g_save_signature_begun = true;
    }
    __imp__sub_000534B8(ctx, base);
}

// CFileManager::VerifySave (vtable[0x54]), thiscall(this, path, unk, flags), ret 12
X86_EXTERN_FUNC(__imp__sub_0003F683);
X86_FUNC(sub_0003F683) {
    std::string path = ReadPath(R32(ctx.esp + 4));

    g_verifying_save = true;
    g_save_signature_begun = false;
    __imp__sub_0003F683(ctx, base);
    g_verifying_save = false;

    // If verification failed but the signature calculation ran, the save file exists and
    // is readable, but was signed with an authentic Xbox console key instead of the recomp's
    // zero key. Accept it as valid.
    if (ctx.eax == 0 && g_save_signature_begun) {
        LOGR("saves: accepting authentic Xbox / xemu save signature for %s", path.c_str());
        ctx.eax = 1;
    }
}
