#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "x86_context.h"

typedef uint16_t __attribute__((aligned(1), may_alias)) x86_u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) x86_u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) x86_u64u;
typedef float __attribute__((aligned(1), may_alias)) x86_f32u;
typedef double __attribute__((aligned(1), may_alias)) x86_f64u;

#define X86_MEM8(a) (*(uint8_t*)(base + (uint32_t)(a)))
#define X86_MEM16(a) (*(x86_u16u*)(base + (uint32_t)(a)))
#define X86_MEM32(a) (*(x86_u32u*)(base + (uint32_t)(a)))
#define X86_MEM64(a) (*(x86_u64u*)(base + (uint32_t)(a)))
#define X86_MEMF32(a) (*(x86_f32u*)(base + (uint32_t)(a)))
#define X86_MEMF64(a) (*(x86_f64u*)(base + (uint32_t)(a)))
#define X86_PTR(T, a) ((T*)(base + (uint32_t)(a)))

// The recompiled code is built on the player's PC into a DLL beside the executable (app/game_code.cpp). The executable
// exports the runtime and its overrides, which the DLL imports; a function with no override links to its body.
#if defined(_WIN32)
#ifdef X86_GAME_CODE
#define X86_HOST_API __declspec(dllimport)
#define X86_HOST_EXPORT
#define X86_WEAK_FUNC(name, impl) __pragma(comment(linker, "/alternatename:" #name "=" #impl))
// the DLL links without a C runtime: the two symbols clang expects from one
extern "C" __attribute__((used)) inline int _fltused = 0;
#pragma comment(linker, "/alternatename:??_7type_info@@6B@=_fltused")  // catch clauses only compare type names
#else
#define X86_HOST_API __declspec(dllexport)
#define X86_HOST_EXPORT __declspec(dllexport)
#endif
#define X86_FUNC(name) extern "C" X86_HOST_EXPORT void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_EXTERN_FUNC(name) extern "C" void name(X86Context& __restrict ctx, uint8_t* base)
#else
// ELF: the game code is <exe>.so, opened with dlopen. Each sub_X is a weak alias of its body, so an override the
// executable exports (default visibility, -rdynamic) takes its place when the dynamic linker binds the .so.
#define X86_HOST_API __attribute__((visibility("default")))
#define X86_HOST_EXPORT __attribute__((visibility("default")))
#define X86_FUNC(name) extern "C" X86_HOST_EXPORT void name(X86Context& __restrict ctx, uint8_t* base)
#ifdef X86_GAME_CODE
#define X86_WEAK_FUNC(name, impl) \
    extern "C" X86_HOST_EXPORT void name(X86Context& __restrict ctx, uint8_t* base) __attribute__((weak, alias(#impl)));
#define X86_EXTERN_FUNC(name) extern "C" void name(X86Context& __restrict ctx, uint8_t* base)
#else
// the executable has no link time tie to the .so, which the first start may only now build: what it calls in it
// (the __imp__ bodies its overrides fall back to) is looked up by name on first use
extern "C" X86_HOST_API void* x86_import(const char* name);
#define X86_IMPORT_NAME_(x) #x
#define X86_IMPORT_NAME(x) X86_IMPORT_NAME_(x)  // a name made by a macro (GUEST_ORIG) expanded first
#define X86_EXTERN_FUNC(name)                                                                                 \
    [[maybe_unused]] static inline void name(X86Context& __restrict ctx, uint8_t* base) {                     \
        static auto* const fn = reinterpret_cast<void (*)(X86Context&, uint8_t*)>(x86_import(X86_IMPORT_NAME(name))); \
        fn(ctx, base);                                                                                        \
    }                                                                                                         \
    static_assert(true)
#endif
#endif

#define X86_PUSH32(v)            \
    do {                         \
        uint32_t _v = (v);       \
        ctx.esp -= 4;            \
        X86_MEM32(ctx.esp) = _v; \
    } while (0)

#define X86_SAVE_FLAGS() (ctx.cf = cf, ctx.zf = zf, ctx.sf = sf, ctx.of = of, ctx.pf = pf)
#define X86_LOAD_FLAGS() uint32_t cf = ctx.cf, zf = ctx.zf, sf = ctx.sf, of = ctx.of, pf = ctx.pf
#define X86_BARRIER() __asm__ volatile("" ::: "memory")
#define X86_PARITY(r) ((uint32_t)(__builtin_parity((uint32_t)(r)&0xFF) ^ 1))

#define X87_ST(i) ctx.st[(ctx.top + (i)) & 7]

// runtime services, implemented in src/cpu/dispatch.cpp
X86_HOST_API void x86_icall(X86Context& ctx, uint8_t* base, uint32_t target);
// a jump to a C++ catch continuation unwinds the host stack to the frame that resumes there (esp as the CRT set it)
struct X86Resume {
    uint32_t target, esp;
};
X86_HOST_API void x86_resume(X86Context& ctx, uint8_t* base, const X86Resume& r);
X86_HOST_API void x86_kernel(X86Context& ctx, uint8_t* base, uint32_t ordinal);
[[noreturn]] X86_HOST_API void x86_trap(X86Context& ctx, uint32_t va, const char* what);
X86_HOST_API void x86_priv(X86Context& ctx, uint32_t va, const char* what);
X86_HOST_API uint64_t x86_rdtsc();
X86_HOST_API void x86_cpuid(X86Context& ctx);

inline void x87_push(X86Context& ctx, double v) {
    ctx.top = (ctx.top - 1) & 7;
    ctx.st[ctx.top] = v;
}
inline void x87_pop(X86Context& ctx) { ctx.top = (ctx.top + 1) & 7; }

// C0 bit 8, C2 bit 10, C3 bit 14
inline void x87_compare(X86Context& ctx, double a, double b) {
    uint16_t c;
    if (std::isnan(a) || std::isnan(b)) c = 0x4500;
    else if (a > b) c = 0;
    else if (a < b) c = 0x0100;
    else c = 0x4000;
    ctx.fsw = uint16_t((ctx.fsw & ~0x4500) | c);
}

// TOP lives in status word bits 11 to 13
inline uint16_t x87_status(X86Context& ctx) { return uint16_t((ctx.fsw & ~0x3800) | ((ctx.top & 7) << 11)); }

inline double x87_round(X86Context& ctx, double v) {
    switch ((ctx.fcw >> 10) & 3) {  // control word RC field
        case 0: return std::nearbyint(v);
        case 1: return std::floor(v);
        case 2: return std::ceil(v);
        default: return std::trunc(v);
    }
}

// out of range gives the integer indefinite value, as the FPU does
inline int32_t x87_to_i32(X86Context& ctx, double v) {
    double r = x87_round(ctx, v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return INT32_MIN;
    return int32_t(r);
}
inline int16_t x87_to_i16(X86Context& ctx, double v) {
    double r = x87_round(ctx, v);
    if (!(r >= -32768.0 && r <= 32767.0)) return INT16_MIN;
    return int16_t(r);
}
inline int64_t x87_to_i64(X86Context& ctx, double v) {
    double r = x87_round(ctx, v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return INT64_MIN;
    return int64_t(r);
}

inline double x87_load80(const uint8_t* p) {
    uint64_t mant;
    uint16_t se;
    std::memcpy(&mant, p, 8);
    std::memcpy(&se, p + 8, 2);
    int sign = se >> 15;
    int exp = se & 0x7FFF;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else v = std::ldexp(double(mant), exp - 16383 - 63);
    return sign ? -v : v;
}

inline void x87_store80(uint8_t* p, double v) {
    uint64_t mant = 0;
    uint16_t se = std::signbit(v) ? 0x8000 : 0;
    double a = std::fabs(v);
    if (std::isnan(v)) {
        se |= 0x7FFF;
        mant = 0xC000000000000000ull;
    } else if (std::isinf(v)) {
        se |= 0x7FFF;
        mant = 0x8000000000000000ull;
    } else if (a != 0.0) {
        int e;
        double m = std::frexp(a, &e);  // a = m * 2^e, m in [0.5,1)
        mant = uint64_t(std::ldexp(m, 64));
        se |= uint16_t(e - 1 + 16383);
    }
    std::memcpy(p, &mant, 8);
    std::memcpy(p + 8, &se, 2);
}

inline uint32_t x87_examine(X86Context& ctx) {
    // FXAM class into C3 C2 C0, C1 = sign
    double v = X87_ST(0);
    uint16_t c;
    if (std::isnan(v)) c = 0x0100;
    else if (std::isinf(v)) c = 0x0500;
    else if (v == 0.0) c = 0x4000;
    else if (std::fpclassify(v) == FP_SUBNORMAL) c = 0x4400;
    else c = 0x0400;
    if (std::signbit(v)) c |= 0x0200;
    ctx.fsw = uint16_t((ctx.fsw & ~0x4700) | c);
    return 0;
}

inline float sse_min(float a, float b) { return a < b ? a : b; }
inline float sse_max(float a, float b) { return a > b ? a : b; }

inline uint32_t sse_cmp(float a, float b, int pred) {
    bool r;
    switch (pred & 7) {
        case 0: r = a == b; break;
        case 1: r = a < b; break;
        case 2: r = a <= b; break;
        case 3: r = std::isnan(a) || std::isnan(b); break;
        case 4: r = !(a == b); break;
        case 5: r = !(a < b); break;
        case 6: r = !(a <= b); break;
        default: r = !(std::isnan(a) || std::isnan(b)); break;
    }
    return r ? 0xFFFFFFFFu : 0u;
}

inline int32_t sse_cvt(float v) {
    float r = std::nearbyint(v);
    if (!(r >= -2147483648.0f && r < 2147483648.0f)) return INT32_MIN;
    return int32_t(r);
}
inline int32_t sse_cvtt(float v) {
    if (!(v >= -2147483648.0f && v < 2147483648.0f)) return INT32_MIN;
    return int32_t(v);
}

template <typename T>
constexpr T sat(int64_t v) {
    constexpr int64_t lo = std::numeric_limits<T>::min(), hi = std::numeric_limits<T>::max();
    return T(v < lo ? lo : v > hi ? hi : v);
}
static_assert(sat<int16_t>(-300) == -300 && sat<int16_t>(40000) == 32767 && sat<int8_t>(-5) == -5);

// lane wise helpers on 64 bit MMX values
template <typename T, typename F>
inline uint64_t mmx_map2(uint64_t a, uint64_t b, F f) {
    constexpr int n = 8 / sizeof(T);
    T x[n], y[n], r[n];
    std::memcpy(x, &a, 8);
    std::memcpy(y, &b, 8);
    for (int i = 0; i < n; i++) r[i] = T(f(x[i], y[i]));
    uint64_t out;
    std::memcpy(&out, r, 8);
    return out;
}

// rep movs: element by element semantics, fast path when the ranges cannot interfere
inline void x86_rep_movs(X86Context& ctx, uint8_t* base, uint32_t w) {
    uint32_t n = ctx.ecx;
    if (!n) return;
    uint32_t bytes = n * w;
    if (!ctx.df && (ctx.edi - ctx.esi >= bytes || ctx.edi <= ctx.esi)) {
        std::memmove(base + ctx.edi, base + ctx.esi, bytes);
        ctx.esi += bytes;
        ctx.edi += bytes;
        ctx.ecx = 0;
        return;
    }
    int32_t d = ctx.df ? -int32_t(w) : int32_t(w);
    for (; n; n--) {
        std::memcpy(base + ctx.edi, base + ctx.esi, w);
        ctx.esi += d;
        ctx.edi += d;
    }
    ctx.ecx = 0;
}

inline void x86_rep_stos(X86Context& ctx, uint8_t* base, uint32_t w) {
    uint32_t n = ctx.ecx;
    if (!n) return;
    if (!ctx.df && w == 1) {
        std::memset(base + ctx.edi, uint8_t(ctx.eax), n);
        ctx.edi += n;
        ctx.ecx = 0;
        return;
    }
    int32_t d = ctx.df ? -int32_t(w) : int32_t(w);
    uint32_t v = ctx.eax;
    for (; n; n--) {
        std::memcpy(base + ctx.edi, &v, w);
        ctx.edi += d;
    }
    ctx.ecx = 0;
}
