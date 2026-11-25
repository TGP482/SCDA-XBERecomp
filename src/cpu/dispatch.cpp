#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

#ifdef _WIN32
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#ifdef _WIN32
// the C runtime functions the game code DLL calls, passed on to the DLLs that have them
#pragma comment(linker, "/EXPORT:memcpy=vcruntime140.memcpy")
#pragma comment(linker, "/EXPORT:memmove=vcruntime140.memmove")
#pragma comment(linker, "/EXPORT:memset=vcruntime140.memset")
#pragma comment(linker, "/EXPORT:__CxxFrameHandler3=vcruntime140.__CxxFrameHandler3")
#pragma comment(linker, "/EXPORT:_CxxThrowException=vcruntime140._CxxThrowException")
#pragma comment(linker, "/EXPORT:sin=ucrtbase.sin")
#pragma comment(linker, "/EXPORT:cos=ucrtbase.cos")
#pragma comment(linker, "/EXPORT:tan=ucrtbase.tan")
#pragma comment(linker, "/EXPORT:atan2=ucrtbase.atan2")
#pragma comment(linker, "/EXPORT:fmod=ucrtbase.fmod")
#pragma comment(linker, "/EXPORT:sqrt=ucrtbase.sqrt")
#pragma comment(linker, "/EXPORT:sqrtf=ucrtbase.sqrtf")
#pragma comment(linker, "/EXPORT:log2=ucrtbase.log2")
#pragma comment(linker, "/EXPORT:exp2=ucrtbase.exp2")
#pragma comment(linker, "/EXPORT:ldexp=ucrtbase.ldexp")
#pragma comment(linker, "/EXPORT:frexp=ucrtbase.frexp")
#endif

namespace {

std::vector<X86Func*> g_table;  // one slot per code byte
std::vector<bool> g_continuation;
uint32_t g_lo = 0, g_hi = 0;
// the recompiled code's tables, exported by the game code DLL
const X86FuncMapping* g_mappings = nullptr;
const uint32_t* g_continuations = nullptr;
uint32_t g_entry = 0;

struct Export {
    const char* name = nullptr;
    kernel::KernelFn fn = nullptr;
    uint32_t data = 0;
};
Export g_exports[400];

}  // namespace

// synthetic guest addresses for kernel functions reached through a pointer
constexpr uint32_t kKernelThunkBase = 0xFE000000;

namespace dispatch {

// the game code DLL and its tables; data comes by name, as the delay loaded imports carry only functions
bool Load(const std::filesystem::path& dll) {
#ifdef _WIN32
    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) return false;
    auto get = [m](const char* name) { return reinterpret_cast<const void*>(GetProcAddress(m, name)); };
#else
    // global: the executable's calls into it (x86_import) find its symbols by name; now: a missing one fails here
    void* m = dlopen(dll.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!m) {
        LOGE("dispatch: %s", dlerror());
        return false;
    }
    auto get = [m](const char* name) { return static_cast<const void*>(dlsym(m, name)); };
#endif
    g_mappings = static_cast<const X86FuncMapping*>(get("X86FuncMappings"));
    g_continuations = static_cast<const uint32_t*>(get("X86Continuations"));
    auto* lo = static_cast<const uint32_t*>(get("X86CodeBase"));
    auto* hi = static_cast<const uint32_t*>(get("X86CodeEnd"));
    auto* entry = static_cast<const uint32_t*>(get("X86EntryPoint"));
    if (!g_mappings || !g_continuations || !lo || !hi || !entry) return false;
    g_lo = *lo;
    g_hi = *hi;
    g_entry = *entry;
    return true;
}

uint32_t EntryPoint() { return g_entry; }

void Init() {
    g_table.assign(g_hi - g_lo, nullptr);
    size_t n = 0;
    for (const X86FuncMapping* m = g_mappings; m->host; m++, n++)
        if (m->guest >= g_lo && m->guest < g_hi) g_table[m->guest - g_lo] = m->host;
    g_continuation.assign(g_hi - g_lo, false);
    for (const uint32_t* c = g_continuations; *c; c++)
        if (*c >= g_lo && *c < g_hi) g_continuation[*c - g_lo] = true;
    LOGI("dispatch: %zu functions over %08X..%08X", n, g_lo, g_hi);
}

X86Func* Find(uint32_t guest) {
    if (guest >= g_lo && guest < g_hi) return g_table[guest - g_lo];
    return nullptr;
}

// host code the guest may call through a pointer gets an address in this range
constexpr uint32_t kHostThunkBase = 0xFD000000;
static std::vector<X86Func*> g_host_thunks;
static std::mutex g_host_lock;

uint32_t HostThunk(X86Func* fn) {
    std::lock_guard<std::mutex> lock(g_host_lock);
    for (size_t i = 0; i < g_host_thunks.size(); i++)
        if (g_host_thunks[i] == fn) return kHostThunkBase + uint32_t(i) * 4;
    g_host_thunks.push_back(fn);
    return kHostThunkBase + uint32_t(g_host_thunks.size() - 1) * 4;
}

uint32_t KernelThunkAddress(uint32_t ordinal) { return kKernelThunkBase + ordinal * 4; }

}  // namespace dispatch

void x86_icall(X86Context& ctx, uint8_t* base, uint32_t target) {
    ctx.last_icall = target;
    if (target >= g_lo && target < g_hi && g_continuation[target - g_lo]) throw X86Resume{target, ctx.esp};
    if (X86Func* f = dispatch::Find(target)) {
        f(ctx, base);
        return;
    }
    if (target >= kKernelThunkBase && target < kKernelThunkBase + 400 * 4) {
        x86_kernel(ctx, base, (target - kKernelThunkBase) / 4);
        return;
    }
    if (target >= dispatch::kHostThunkBase) {
        size_t i = (target - dispatch::kHostThunkBase) / 4;
        X86Func* f = nullptr;
        {
            std::lock_guard<std::mutex> lock(dispatch::g_host_lock);
            if (i < dispatch::g_host_thunks.size()) f = dispatch::g_host_thunks[i];
        }
        if (f) {
            f(ctx, base);
            return;
        }
    }
    uint32_t ret = kernel::R32(ctx.esp);
    LOGE("indirect call to %08X has no recompiled function (return address %08X, esp %08X)", target, ret, ctx.esp);
    LOGE("add 0x%X to [functions] in the image's config/*.toml and rebuild", target);
    x86_trap(ctx, ret, "indirect call to unknown code");
}

void x86_resume(X86Context& ctx, uint8_t* base, const X86Resume& r) {
    ctx.esp = r.esp;
    dispatch::Find(r.target)(ctx, base);
}

void x86_kernel(X86Context& ctx, uint8_t* base, uint32_t ordinal) {
    if (ordinal < 400 && g_exports[ordinal].fn) {
        g_exports[ordinal].fn(ctx, base);
        return;
    }
    LOGE("kernel export %u (%s) is not implemented, return address %08X", ordinal,
         ordinal < 400 && g_exports[ordinal].name ? g_exports[ordinal].name : "?", kernel::R32(ctx.esp));
    x86_trap(ctx, kernel::R32(ctx.esp), "unimplemented kernel export");
}

namespace kernel {

void RegisterExport(uint32_t ordinal, const char* name, KernelFn fn) {
    g_exports[ordinal].name = name;
    g_exports[ordinal].fn = fn;
}

static uint32_t g_kdata_next = mem::kKernelDataBase + 0x1000;

uint32_t RegisterData(uint32_t ordinal, const char* name, uint32_t size) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (g_exports[ordinal].data) return g_exports[ordinal].data;
    uint32_t a = (g_kdata_next + 15) & ~15u;
    g_kdata_next = a + ((size + 15) & ~15u);
    g_exports[ordinal].name = name;
    g_exports[ordinal].data = a;
    return a;
}

uint32_t DataAddress(uint32_t ordinal) { return ordinal < 400 ? g_exports[ordinal].data : 0; }
const char* ExportName(uint32_t ordinal) { return ordinal < 400 && g_exports[ordinal].name ? g_exports[ordinal].name : "?"; }

uint32_t CallGuest(X86Context& ctx, uint32_t fn, std::initializer_list<uint32_t> args) {
    // the D3D and DirectSound hooks call this several times a draw: no heap, only the registers put back are kept
    struct {
        uint32_t eax, ecx, edx, ebx, esi, edi, ebp;
    } saved{ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esi, ctx.edi, ctx.ebp};
    uint32_t esp = ctx.esp;
    for (const uint32_t* p = args.end(); p != args.begin();) {
        ctx.esp -= 4;
        W32(ctx.esp, *--p);
    }
    ctx.esp -= 4;
    W32(ctx.esp, 0xFFFFFFF0u);  // never used as a target, calls return through the host stack
    x86_icall(ctx, g_base, fn);
    uint32_t result = ctx.eax;
    ctx.eax = saved.eax;
    ctx.ecx = saved.ecx;
    ctx.edx = saved.edx;
    ctx.ebx = saved.ebx;
    ctx.esi = saved.esi;
    ctx.edi = saved.edi;
    ctx.ebp = saved.ebp;
    ctx.esp = esp;
    return result;
}

}  // namespace kernel

namespace kernel {
// return addresses found on the guest stack, the cheapest way to place a guest event
void LogGuestStack(X86Context& ctx, int words) {
    LOGE("  eax %08X ecx %08X edx %08X ebx %08X esp %08X ebp %08X esi %08X edi %08X", ctx.eax, ctx.ecx, ctx.edx, ctx.ebx,
         ctx.esp, ctx.ebp, ctx.esi, ctx.edi);
    for (int i = 0; i < words; i++) {
        uint32_t a = ctx.esp + uint32_t(i) * 4;
        if (!mem::IsCommitted(a)) break;
        uint32_t v = R32(a);
        if (v >= g_lo && v < g_hi) LOGE("  [esp+%03X] %08X", i * 4, v);
    }
}
}  // namespace kernel

void x86_trap(X86Context& ctx, uint32_t va, const char* what) {
    LOGE("guest trap at %08X: %s", va, what);
    kernel::LogGuestStack(ctx, 64);
    logx::Flush();
#ifdef _WIN32
    __debugbreak();
#endif
    std::abort();
}

void x86_priv(X86Context& ctx, uint32_t va, const char* what) {
    static std::mutex m;
    static std::unordered_set<uint32_t> seen;
    std::lock_guard<std::mutex> lock(m);
    if (seen.insert(va).second) LOGW("privileged instruction %s at %08X ignored", what, va);
    (void)ctx;
}

// the guest measured time against a 733.33 MHz time stamp counter
uint64_t x86_rdtsc() {
    using namespace std::chrono;
    static const auto start = kernel::GuestNow();
    auto ns = duration_cast<nanoseconds>(kernel::GuestNow() - start).count();
    uint64_t sec = uint64_t(ns) / 1000000000ull, rem = uint64_t(ns) % 1000000000ull;
    return sec * 733333333ull + rem * 733333333ull / 1000000000ull;
}

void x86_cpuid(X86Context& ctx) {
    switch (ctx.eax) {
        case 0:
            ctx.eax = 2;
            ctx.ebx = 0x756E6547;  // GenuineIntel
            ctx.edx = 0x49656E69;
            ctx.ecx = 0x6C65746E;
            break;
        case 1:
            ctx.eax = 0x0000068A;  // family 6 model 8, Pentium III Coppermine
            ctx.ebx = 0;
            ctx.ecx = 0;
            ctx.edx = 0x0383F9FF;  // fpu tsc cx8 cmov mmx fxsr sse
            break;
        default:
            ctx.eax = ctx.ebx = ctx.ecx = ctx.edx = 0;
            break;
    }
}

#ifndef _WIN32
// the executable's calls into the game code .so: its symbols, or an override the executable exports, by name
void* x86_import(const char* name) {
    void* p = dlsym(RTLD_DEFAULT, name);
    if (!p) {
        LOGE("dispatch: the game code has no %s", name);
        logx::Flush();
        std::abort();
    }
    return p;
}
#endif
