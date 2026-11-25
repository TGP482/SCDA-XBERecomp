#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <fstream>

#include "kernel/kernel.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

extern char** environ;
#endif
#include "xbe/xbe.h"
#include "xdk/hle.h"

namespace dispatch {
void Init();
uint32_t EntryPoint();
uint32_t KernelThunkAddress(uint32_t ordinal);
}  // namespace dispatch

namespace xnet {
void CloseSockets();  // xdk/xnet.cpp
bool CoopHostValues(uint32_t& rate, uint32_t& floor);
}

namespace kernel {

uint8_t* g_base = nullptr;
std::string g_launch_file;

namespace {
uint32_t g_saved_data = 0;
bool g_mark = false, g_kept_marked = false;
std::string g_picture, g_kept_picture;  // the last picture ShowScreen loaded, raw path string with its terminator
constexpr uint32_t kPersistMagic = 0x50535044;  // "DPSP"

std::string PersistFile() { return g_launch_file.substr(0, g_launch_file.find_last_of("/\\") + 1) + "display.bin"; }

// surface header (Common, Data, Lock, Format, Size) then the pixels at +0x40, as the XDK lays out its persisted copy
void LoadPersistedDisplay() {
    std::ifstream f(PersistFile(), std::ios::binary);
    uint32_t head[7] = {};
    if (!f.read(reinterpret_cast<char*>(head), sizeof head) || head[0] != kPersistMagic || !head[4] || head[4] > 0x800000 ||
        head[6] > 1040)
        return;
    uint32_t block = mem::AllocContiguous(0x40 + head[4], 0, 0xFFFFFFFF, 0x1000);
    if (!block || !f.read(static_cast<char*>(HostPtr(block + 0x40)), head[4])) return;
    g_kept_picture.resize(head[6]);
    if (!f.read(g_kept_picture.data(), head[6])) g_kept_picture.clear();
    W32(block, (head[1] & 0xFFFF0000) | 1);
    W32(block + 4, mem::PhysicalFromVirtual(block + 0x40));
    W32(block + 8, 0);
    W32(block + 0xC, head[2]);
    W32(block + 0x10, head[3]);
    g_saved_data = block;
    g_kept_marked = head[5] != 0;
    LOGI("boot: display kept from the previous run, %u bytes%s", head[4], g_kept_marked ? ", marked" : "");
}
}  // namespace

void SavePersistedDisplay(uint32_t common, uint32_t format, uint32_t size, const void* pixels, uint32_t bytes) {
    if (FILE* f = std::fopen(PersistFile().c_str(), "wb")) {
        uint32_t head[7] = {kPersistMagic, common, format, size, bytes, g_mark, uint32_t(g_picture.size())};
        std::fwrite(head, sizeof head, 1, f);
        std::fwrite(pixels, 1, bytes, f);
        std::fwrite(g_picture.data(), 1, g_picture.size(), f);
        std::fclose(f);
    }
}

void MarkPersistedDisplay() { g_mark = true; }
bool KeptDisplayMarked() { return g_kept_marked; }
void SetShownPicture(std::string path) { g_picture = std::move(path); }
const std::string& KeptPicture() { return g_kept_picture; }

uint32_t SavedDataAddress() { return g_saved_data; }
void SetSavedDataAddress(uint32_t surface) { g_saved_data = surface; }
const xbe::Image* g_image = nullptr;
static xbe::Image g_xbe;

bool Boot(std::vector<uint8_t> xbe, const std::string& xbe_name, const std::string& game_dir, const std::string& data_dir) {
    if (!mem::Init()) return false;
    g_base = mem::Base();

    std::string err;
    if (!g_xbe.Parse(std::move(xbe), err)) {
        LOGE("boot: %s: %s", xbe_name.c_str(), err.c_str());
        return false;
    }
    g_image = &g_xbe;
    const xbe::Image& img = g_xbe;
    if (img.entry != dispatch::EntryPoint()) {
        LOGE("boot: %s entry %08X does not match the recompiled entry %08X, wrong executable", xbe_name.c_str(), img.entry,
             dispatch::EntryPoint());
        return false;
    }
    LOGI("boot: title %08X, entry %08X, %zu sections", img.title_id, img.entry, img.sections.size());
    for (auto& l : img.libraries) LOGI("boot: library %-8s %u.%u.%u", l.name.c_str(), l.major, l.minor, l.build);

    mem::ReserveImage(img.base, img.image_size);
    std::memcpy(g_base + img.base, img.file.data(), std::min<size_t>(img.header_size, img.file.size()));
    // XBE header 0x120 points at the section headers, 56 bytes each, +0x18 is the reference count that marks one loaded
    uint32_t sec_hdr = img.U32(0x120);
    for (size_t i = 0; i < img.sections.size(); i++) {
        auto& s = img.sections[i];
        if (!(s.flags & xbe::kPreload)) continue;
        std::memcpy(g_base + s.va, img.file.data() + s.raw, std::min(s.rsize, s.vsize));
        W32(sec_hdr + uint32_t(i) * 56 + 0x18, 1);
    }
    // the engine holds frames to 1/60 s itself, the frame rate cap in the options menu replaces it
    if (HAS_Engine_FrameTimeMin && R32(SYM(Engine_FrameTimeMin)) == 0x3C888723) W32(SYM(Engine_FrameTimeMin), 0);

    InitKernelData();
    dispatch::Init();

    uint32_t missing = 0;
    for (uint32_t slot = img.kernel_thunk;; slot += 4) {
        uint32_t v = R32(slot);
        if (!v) break;
        uint32_t ord = v & 0x7FFFFFFF;  // high bit flags an import by ordinal
        uint32_t data = DataAddress(ord);
        W32(slot, data ? data : dispatch::KernelThunkAddress(ord));
        if (!data && std::string(ExportName(ord)) == "?") {
            LOGW("boot: kernel import %u has no implementation", ord);
            missing++;
        }
    }
    LOGI("boot: kernel imports resolved, %u missing", missing);

    MountDefaults(game_dir, data_dir);

    // a quick reboot into this title left its launch data page behind
    g_launch_file = data_dir + "/launch.bin";
    std::ifstream lf(g_launch_file, std::ios::binary);
    if (lf) {
        uint32_t page = mem::AllocContiguous(0x1000, 0, 0xFFFFFFFF, 0x1000);
        lf.read(static_cast<char*>(HostPtr(page)), 0x1000);
        lf.close();
        std::remove(g_launch_file.c_str());
        W32(g_launch_data_page_va, page);
        LOGI("boot: launch data from the previous run, type %u", R32(page));
        LoadPersistedDisplay();
    }
    std::remove(PersistFile().c_str());

    uint32_t tls = 0;
    if (img.tls_addr) tls = ((img.tls.data_end - img.tls.data_start + img.tls.zero_fill + 15) & ~15u) + 4;
    ThreadRef t = CreateThread(std::max<uint32_t>(img.stack_commit, 0x10000), tls, 0, img.entry, 0, false);
    StartThread(t);
    return true;
}

const char* ImageExe(std::string xbe_path) {
    for (char& c : xbe_path) c = char(std::tolower(uint8_t(c)));
    static const std::pair<const char*, const char*> kImages[] = {{"default.xbe", "SCDA"}, {"sc4_adv.xbe", "SCDA_Versus"}};
    for (auto& [xbe, exe] : kImages) {
        size_t n = std::strlen(xbe);
        if (xbe_path.size() >= n && xbe_path.compare(xbe_path.size() - n, n, xbe) == 0 &&
            (xbe_path.size() == n || xbe_path[xbe_path.size() - n - 1] == '\\' || xbe_path[xbe_path.size() - n - 1] == ';'))
            return exe;
    }
    return nullptr;
}

void Relaunch(const char* exe) {
    logx::Flush();
#ifdef _WIN32
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring path = self;
    path = path.substr(0, path.find_last_of(L'\\') + 1) + std::wstring(exe, exe + std::strlen(exe)) + L".exe";
    // the command line keeps its arguments (--instance) behind the new executable
    std::wstring line = GetCommandLineW();
    size_t args = line[0] == L'"' ? line.find(L'"', 1) + 1 : line.find(L' ');
    line = L"\"" + path + L"\"" + (args == std::wstring::npos ? L"" : line.substr(args));
    if (line.find(L"--relaunched") == std::wstring::npos) line += L" --relaunched";  // no boot splash again
    // a co-op host's tick rate and input delay floor, for the next session init (game/coop_sync.cpp)
    if (size_t at = line.find(L" --coop-host="); at != std::wstring::npos) line.erase(at, line.find(L' ', at + 1) - at);
    if (uint32_t rate, floor; xnet::CoopHostValues(rate, floor))
        line += L" --coop-host=" + std::to_wstring(rate) + L"," + std::to_wstring(floor);
    // this process lives on through the handover below: its ports must be free for the new one (co-op binds them again)
    xnet::CloseSockets();
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(path.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        // this window stays up until the new instance shows its first frames (HandoverEvent), so no gap shows
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, HandoverEvent(pi.dwProcessId).c_str());
        AllowSetForegroundWindow(pi.dwProcessId);
        g_relaunching = true;
        ResumeThread(pi.hThread);
        HANDLE wait[] = {ready, pi.hProcess};
        if (ready) WaitForMultipleObjects(2, wait, FALSE, 15000);
        if (ready) ::CloseHandle(ready);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    } else {
        LOGE("relaunch: cannot start %s.exe (%lu)", exe, GetLastError());
    }
    std::_Exit(0);
#else
    char self[4096] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    std::string path = std::string(self, n > 0 ? size_t(n) : 0);
    path = path.substr(0, path.find_last_of('/') + 1) + exe;
    // the arguments (--instance) carry over, as on Windows
    std::vector<std::string> args{path};
    {
        std::ifstream f("/proc/self/cmdline", std::ios::binary);
        std::string all((std::istreambuf_iterator<char>(f)), {});
        size_t at = all.find('\0');  // past argv[0]
        while (at != std::string::npos && at + 1 < all.size()) {
            size_t end = all.find('\0', at + 1);
            std::string a = all.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
            if (a.rfind("--coop-host=", 0) != 0) args.push_back(a);
            at = end;
        }
    }
    bool relaunched = false;
    for (const std::string& a : args) relaunched |= a == "--relaunched";
    if (!relaunched) args.push_back("--relaunched");  // no boot splash again
    if (uint32_t rate, floor; xnet::CoopHostValues(rate, floor))
        args.push_back("--coop-host=" + std::to_string(rate) + "," + std::to_string(floor));
    xnet::CloseSockets();
    // this window stays up until the new instance shows its first frames: it writes a byte to the pipe whose end it
    // finds in SCDA_HANDOVER_FD (app/handover)
    int ready[2] = {-1, -1};
    bool piped = pipe(ready) == 0;
    if (piped) fcntl(ready[0], F_SETFD, FD_CLOEXEC);
    std::vector<std::string> env_strings;
    for (char** e = environ; *e; e++)
        if (std::string_view(*e).rfind("SCDA_HANDOVER_FD=", 0) != 0) env_strings.push_back(*e);
    if (piped) env_strings.push_back("SCDA_HANDOVER_FD=" + std::to_string(ready[1]));
    std::vector<char*> argv, envp;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    for (std::string& e : env_strings) envp.push_back(e.data());
    envp.push_back(nullptr);
    pid_t child = 0;
    if (posix_spawn(&child, path.c_str(), nullptr, nullptr, argv.data(), envp.data()) == 0) {
        g_relaunching = true;
        if (piped) {
            close(ready[1]);
            auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (std::chrono::steady_clock::now() < until) {
                pollfd p{ready[0], POLLIN, 0};
                if (poll(&p, 1, 100) != 0) break;  // a byte, or the child gone (end of file)
                int status = 0;
                if (waitpid(child, &status, WNOHANG) == child) break;
            }
        }
    } else {
        LOGE("relaunch: cannot start %s", path.c_str());
    }
    logx::Flush();
    std::_Exit(0);
#endif
}

void Shutdown(int code) {
    LOGR("Exit (%d)", code);
    logx::Flush();
    std::_Exit(code);
}

}  // namespace kernel
