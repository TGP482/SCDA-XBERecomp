#include "kernel/memory.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "core/log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace mem {

static uint8_t* g_base = nullptr;
static std::recursive_mutex g_lock;

uint8_t* Base() { return g_base; }

// physical memory shows at 0x80000000 and again at 0xF0000000 (write combined view)
static bool InPhysical(uint32_t va) {
    return (va >= kContiguousBase && va < kContiguousBase + kPhysicalSize) ||
           (va >= kWriteCombineBase && va < kWriteCombineBase + kPhysicalSize);
}

bool Init() {
    const uint64_t size = 1ull << 32;
#ifdef _WIN32
    HANDLE ram = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kPhysicalSize, nullptr);
    // a fixed base keeps crash dumps comparable between runs
    for (uint64_t hint : {0x100000000ull, 0x200000000ull, 0x300000000ull, 0ull}) {
        uint8_t* b = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(hint), size, MEM_RESERVE, PAGE_NOACCESS));
        if (!b) continue;
        VirtualFree(b, 0, MEM_RELEASE);
        // reserve around the two physical windows, then map the same memory into both
        const uint64_t cuts[][2] = {{0, kContiguousBase},
                                    {kContiguousBase + kPhysicalSize, kWriteCombineBase},
                                    {kWriteCombineBase + kPhysicalSize, size}};
        bool ok = ram != nullptr;
        for (auto& c : cuts)
            ok = ok && VirtualAlloc(b + c[0], size_t(c[1] - c[0]), MEM_RESERVE, PAGE_NOACCESS) == b + c[0];
        ok = ok && MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, kPhysicalSize, b + kContiguousBase) == b + kContiguousBase;
        ok = ok && MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, kPhysicalSize, b + kWriteCombineBase) == b + kWriteCombineBase;
        if (ok) {
            g_base = b;
            break;
        }
        for (auto& c : cuts) VirtualFree(b + c[0], 0, MEM_RELEASE);
        UnmapViewOfFile(b + kContiguousBase);
        UnmapViewOfFile(b + kWriteCombineBase);
    }
#else
    void* p = mmap(reinterpret_cast<void*>(0x100000000ull), size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    g_base = p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
    int fd = g_base ? memfd_create("ram", 0) : -1;
    if (fd < 0 || ftruncate(fd, kPhysicalSize) ||
        mmap(g_base + kContiguousBase, kPhysicalSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED ||
        mmap(g_base + kWriteCombineBase, kPhysicalSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED)
        g_base = nullptr;
#endif
    if (!g_base) {
        LOGE("memory: could not reserve the 4 GB guest space");
        return false;
    }
    LOGI("memory: guest space at %p", static_cast<void*>(g_base));
    return true;
}

// page commit map, one byte per 4 KB page
static std::vector<uint8_t> g_committed(0x100000, 0);

bool Commit(uint32_t va, uint32_t size) {
    if (!size || InPhysical(va)) return true;
    uint32_t first = va & ~(kPage - 1);
    uint64_t end = (uint64_t(va) + size + kPage - 1) & ~uint64_t(kPage - 1);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
#ifdef _WIN32
    if (!VirtualAlloc(g_base + first, size_t(end - first), MEM_COMMIT, PAGE_READWRITE)) {
        LOGE("memory: commit %08X+%X failed", first, uint32_t(end - first));
        return false;
    }
#else
    if (mprotect(g_base + first, size_t(end - first), PROT_READ | PROT_WRITE)) {
        LOGE("memory: commit %08X+%X failed", first, uint32_t(end - first));
        return false;
    }
#endif
    for (uint64_t p = first; p < end; p += kPage) g_committed[size_t(p >> 12)] = 1;
    return true;
}

void Decommit(uint32_t va, uint32_t size) {
    if (InPhysical(va)) return;
    uint32_t first = va & ~(kPage - 1);
    uint64_t end = (uint64_t(va) + size + kPage - 1) & ~uint64_t(kPage - 1);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
#ifdef _WIN32
    VirtualFree(g_base + first, size_t(end - first), MEM_DECOMMIT);
#else
    madvise(g_base + first, size_t(end - first), MADV_DONTNEED);
    mprotect(g_base + first, size_t(end - first), PROT_NONE);
#endif
    for (uint64_t p = first; p < end; p += kPage) g_committed[size_t(p >> 12)] = 0;
}

bool IsCommitted(uint32_t va) { return InPhysical(va) || g_committed[va >> 12] != 0; }

// first fit allocator over a guest range
class Ranges {
public:
    void Init(uint32_t start, uint32_t size) { free_[start] = size; }

    void Remove(uint32_t start, uint32_t size) {
        // carve a fixed range out of the free list
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            uint32_t fs = it->first, fz = it->second;
            if (start >= fs && uint64_t(start) + size <= uint64_t(fs) + fz) {
                free_.erase(it);
                if (start > fs) free_[fs] = start - fs;
                if (uint64_t(start) + size < uint64_t(fs) + fz) free_[start + size] = uint32_t(uint64_t(fs) + fz - start - size);
                return;
            }
        }
    }

    uint32_t Alloc(uint32_t size, uint32_t align, uint32_t lo, uint32_t hi, bool top_down) {
        if (!align) align = 1;
        if (top_down) {
            for (auto it = free_.rbegin(); it != free_.rend(); ++it) {
                uint64_t fs = it->first, fe = fs + it->second;
                uint64_t e = std::min<uint64_t>(fe, uint64_t(hi) + 1);
                if (e < size) continue;
                uint64_t a = (e - size) & ~uint64_t(align - 1);
                if (a < fs || a < lo) continue;
                Take(uint32_t(a), size);
                return uint32_t(a);
            }
            return 0;
        }
        for (auto& [fs, fz] : free_) {
            uint64_t a = std::max<uint64_t>(fs, lo);
            a = (a + align - 1) & ~uint64_t(align - 1);
            if (a + size <= uint64_t(fs) + fz && a + size - 1 <= hi) {
                Take(uint32_t(a), size);
                return uint32_t(a);
            }
        }
        return 0;
    }

    bool IsFree(uint32_t start, uint32_t size) const {
        auto it = free_.upper_bound(start);
        if (it == free_.begin()) return false;
        --it;
        return uint64_t(start) + size <= uint64_t(it->first) + it->second;
    }

    bool Free(uint32_t a, uint32_t* size_out = nullptr) {
        auto it = used_.find(a);
        if (it == used_.end()) return false;
        uint32_t size = it->second;
        used_.erase(it);
        if (size_out) *size_out = size;
        Release(a, size);
        return true;
    }

    void Release(uint32_t a, uint32_t size) {
        auto n = free_.emplace(a, size).first;
        auto next = std::next(n);
        if (next != free_.end() && n->first + n->second == next->first) {
            n->second += next->second;
            free_.erase(next);
        }
        if (n != free_.begin()) {
            auto prev = std::prev(n);
            if (prev->first + prev->second == n->first) {
                prev->second += n->second;
                free_.erase(n);
            }
        }
    }

    void Forget(uint32_t a) { used_.erase(a); }

    uint32_t SizeOf(uint32_t a) const {
        auto it = used_.find(a);
        return it == used_.end() ? 0 : it->second;
    }

    uint64_t Used() const {
        uint64_t n = 0;
        for (auto& u : used_) n += u.second;
        return n;
    }

private:
    void Take(uint32_t a, uint32_t size) {
        Remove(a, size);
        used_[a] = size;
    }
    std::map<uint32_t, uint32_t> free_;
    std::map<uint32_t, uint32_t> used_;
};

static Ranges g_contig, g_system, g_stacks;
static std::map<uint32_t, uint32_t> g_stack_tops;
static bool g_ranges_ready = false;

struct Reservation {
    uint32_t size;
    uint32_t protect;
    std::vector<uint8_t> commit;  // per page
};
static std::map<uint32_t, Reservation> g_reserved;
static Ranges g_user;

static void EnsureRanges() {
    if (g_ranges_ready) return;
    g_ranges_ready = true;
    g_contig.Init(kContiguousBase + kKernelDataSize, kPhysicalSize - kKernelDataSize);
    g_system.Init(kSystemBase, kSystemSize);
    g_stacks.Init(kStackBase, kStackSize);
    g_user.Init(kUserLow, kUserHigh - kUserLow);
    Commit(kKernelDataBase, kKernelDataSize);
}

void ReserveImage(uint32_t base, uint32_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    EnsureRanges();
    uint32_t b = base & ~0xFFFFu;
    uint32_t e = (base + size + 0xFFFF) & ~0xFFFFu;
    g_user.Remove(b, e - b);
    Reservation r{e - b, 4, std::vector<uint8_t>((e - b) / kPage, 1)};  // 4 is PAGE_READWRITE
    g_reserved[b] = std::move(r);
    Commit(b, e - b);
}

static Reservation* FindReservation(uint32_t addr, uint32_t* start) {
    auto it = g_reserved.upper_bound(addr);
    if (it == g_reserved.begin()) return nullptr;
    --it;
    if (addr >= it->first + it->second.size) return nullptr;
    *start = it->first;
    return &it->second;
}

uint32_t AllocateVirtual(uint32_t& base, uint32_t& size, uint32_t type, uint32_t protect) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    EnsureRanges();
    if (!size) return 0xC000000D;  // STATUS_INVALID_PARAMETER
    bool reserve = type & kMemReserve;
    bool commit = type & kMemCommit;
    if (!base && commit) reserve = true;
    if (reserve) {
        uint32_t sz = (size + 0xFFFF) & ~0xFFFFu;
        uint32_t a;
        if (base) {
            a = base & ~0xFFFFu;
            sz = ((base + size + 0xFFFF) & ~0xFFFFu) - a;
            if (!g_user.IsFree(a, sz)) return 0xC0000018;  // STATUS_CONFLICTING_ADDRESSES
            g_user.Remove(a, sz);
        } else {
            a = g_user.Alloc(sz, 0x10000, kUserLow, kUserHigh - 1, type & kMemTopDown);
            if (!a) return 0xC0000017;  // STATUS_NO_MEMORY
            g_user.Forget(a);
        }
        g_reserved[a] = Reservation{sz, protect, std::vector<uint8_t>(sz / kPage, 0)};
        base = a;
        size = sz;
    }
    if (commit) {
        uint32_t start;
        Reservation* r = FindReservation(base, &start);
        if (!r) return 0xC0000045;  // STATUS_INVALID_PAGE_PROTECTION stands in for not reserved
        uint32_t first = base & ~(kPage - 1);
        uint32_t end = (base + size + kPage - 1) & ~(kPage - 1);
        if (end > start + r->size) return 0xC0000018;  // STATUS_CONFLICTING_ADDRESSES
        Commit(first, end - first);
        for (uint32_t p = first; p < end; p += kPage) r->commit[(p - start) / kPage] = 1;
        base = first;
        size = end - first;
    }
    return 0;
}

uint32_t FreeVirtual(uint32_t& base, uint32_t& size, uint32_t type) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t start;
    Reservation* r = FindReservation(base, &start);
    if (!r) return 0xC000009F;  // STATUS_MEMORY_NOT_ALLOCATED
    if (type & kMemRelease) {
        uint32_t sz = r->size;
        Decommit(start, sz);
        g_reserved.erase(start);
        g_user.Release(start, sz);
        base = start;
        size = sz;
        return 0;
    }
    uint32_t first = base & ~(kPage - 1);
    uint32_t end = size ? (base + size + kPage - 1) & ~(kPage - 1) : start + r->size;
    Decommit(first, end - first);
    for (uint32_t p = first; p < end; p += kPage) r->commit[(p - start) / kPage] = 0;
    base = first;
    size = end - first;
    return 0;
}

bool QueryVirtual(uint32_t addr, RegionInfo& out) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t start;
    Reservation* r = FindReservation(addr, &start);
    std::memset(&out, 0, sizeof(out));
    if (!r) {
        out.base = addr & ~(kPage - 1);
        auto it = g_reserved.upper_bound(addr);
        uint32_t next = it == g_reserved.end() ? kUserHigh : it->first;
        out.size = next > out.base ? next - out.base : kPage;
        out.state = 0x10000;  // MEM_FREE
        out.protect = 1;      // PAGE_NOACCESS
        return addr < kUserHigh;
    }
    uint32_t page = (addr - start) / kPage;
    uint8_t c = r->commit[page];
    uint32_t n = page;
    while (n < r->commit.size() && r->commit[n] == c) n++;
    out.base = start + page * kPage;
    out.alloc_base = start;
    out.alloc_protect = r->protect;
    out.size = (n - page) * kPage;
    out.state = c ? kMemCommit : kMemReserve;
    out.protect = c ? r->protect : 0;
    out.type = 0x20000;  // MEM_PRIVATE
    return true;
}

uint32_t AllocContiguous(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t align) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    EnsureRanges();
    size = (size + kPage - 1) & ~(kPage - 1);
    if (!size) size = kPage;
    align = std::max<uint32_t>(align, kPage);
    uint32_t lo = kContiguousBase | (lowest & (kPhysicalSize - 1));
    uint32_t hi = highest >= kPhysicalSize - 1 ? kContiguousBase + kPhysicalSize - 1 : kContiguousBase | highest;
    uint32_t a = g_contig.Alloc(size, align, lo, hi, false);
    if (!a) {
        LOGE("memory: contiguous allocation of %X bytes failed (%X..%X align %X), %llu KB in use", size, lowest, highest, align,
             (unsigned long long)(g_contig.Used() >> 10));
        return 0;
    }
    Commit(a, size);
    std::memset(g_base + a, 0, size);
    return a;
}

void FreeContiguous(uint32_t va) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t size;
    if (g_contig.Free(va, &size)) Decommit(va, size);
}

uint32_t AllocSystem(uint32_t size, uint32_t align) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    EnsureRanges();
    size = (size + 15) & ~15u;
    if (!size) size = 16;
    uint32_t a = g_system.Alloc(size, std::max<uint32_t>(align, 16), kSystemBase, kSystemBase + kSystemSize - 1, false);
    if (!a) return 0;
    Commit(a, size);
    std::memset(g_base + a, 0, size);
    return a;
}

void FreeSystem(uint32_t va) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_system.Free(va);
}

uint32_t AllocStack(uint32_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    EnsureRanges();
    size = (size + 0xFFFF) & ~0xFFFFu;
    // one uncommitted guard page below the stack
    uint32_t a = g_stacks.Alloc(size + 0x10000, 0x10000, kStackBase, kStackBase + kStackSize - 1, false);
    if (!a) return 0;
    Commit(a + 0x10000, size);
    g_stack_tops[a + 0x10000 + size] = a;
    return a + 0x10000 + size;
}

void FreeStack(uint32_t top) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_stack_tops.find(top);
    if (it == g_stack_tops.end()) return;
    uint32_t size;
    if (g_stacks.Free(it->second, &size)) Decommit(it->second, size);
    g_stack_tops.erase(it);
}

uint32_t AllocationSize(uint32_t va) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (uint32_t s = g_contig.SizeOf(va)) return s;
    if (uint32_t s = g_system.SizeOf(va)) return s;
    uint32_t start;
    if (Reservation* r = FindReservation(va, &start)) return r->size - (va - start);
    return 0;
}

uint32_t PhysicalFromVirtual(uint32_t va) {
    if (InPhysical(va)) return va & (kPhysicalSize - 1);
    return va;
}

uint32_t VirtualFromPhysical(uint32_t pa) {
    if (pa < kPhysicalSize) return kContiguousBase | pa;
    return pa;
}

Stats Query() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Stats s{};
    s.total_pages = kPhysicalSize / kPage;
    uint64_t vc = 0;
    for (auto& [a, r] : g_reserved)
        for (uint8_t c : r.commit) vc += c;
    s.virtual_committed = uint32_t(vc);
    s.contiguous_used = uint32_t(g_contig.Used() / kPage);
    s.system_used = uint32_t(g_system.Used() / kPage);
    uint64_t used = vc + s.contiguous_used + s.system_used;
    s.available_pages = used >= s.total_pages ? 0 : uint32_t(s.total_pages - used);
    return s;
}

}  // namespace mem
