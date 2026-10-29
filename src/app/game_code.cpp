// The recompiled game code is built on the player's PC, so a release carries none of it: xbe_recomp translates each
// xbe in assets to C++, clang compiles that and lld links it into <exe>.dll beside the executables, all from the
// compiler folder. The first start does this behind a progress window. Finished objects are kept, so a closed window
// or a crash loses only the files in progress and the next start carries on.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <string_view>
#include <thread>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/disc.h"

namespace dispatch {
bool Load(const std::filesystem::path& dll);  // cpu/dispatch.cpp
}

namespace app {

namespace {

namespace fs = std::filesystem;

// the executables and the xbe recompiled into each
struct Image {
    const wchar_t* exe;
    const char* xbe;
    const char* config;
};
constexpr Image kImages[] = {
    {L"SCDA", "default.xbe", "scda_v2.toml"},
    {L"SCDA_Versus", "sc4_adv.xbe", "sc4_adv.toml"},
};

// as CMakePresets.json and CMakeLists.txt compile it for build.bat
constexpr wchar_t kClangFlags[] =
    L"-target x86_64-pc-windows-msvc -fms-compatibility-version=19.40 -std=c++20 -O2 -march=x86-64-v2 "
    L"-fno-strict-aliasing -DNDEBUG -DX86_GAME_CODE -nostdinc -nostdinc++ -w";
constexpr wchar_t kLinkFlags[] = L"/dll /noentry /nodefaultlib /machine:x64 /opt:noicf";
constexpr wchar_t kTitle[] = L"Tom Clancy's Splinter Cell: Double Agent - XBE Recomp";
constexpr uint64_t kJobMemory = 512ull << 20;  // a compiler's peak on the biggest file is about half of this
constexpr uint64_t kDiskNeeded = 1ull << 30;    // both images' sources, objects and DLLs, with room to spare

// the F10 panel's colours
constexpr COLORREF kBackground = RGB(10, 11, 10);
constexpr COLORREF kText = RGB(227, 232, 219);
constexpr COLORREF kDim = RGB(145, 153, 138);
constexpr COLORREF kBarEmpty = RGB(28, 33, 26);
constexpr COLORREF kBarFull = RGB(145, 158, 130);

enum class Stage { Translating, Compiling, Linking, Ready };

// written by the build thread, read by the window
struct Progress {
    std::atomic<Stage> stage{Stage::Translating};
    std::atomic<uint64_t> bytes_done{0};
    std::atomic<uint64_t> bytes_total{0};
    std::atomic<uint32_t> files_done{0};
    std::atomic<uint32_t> files_total{0};
    std::atomic<uint32_t> first_wave{1};  // files compiled before the time left is worth showing
    std::atomic<int64_t> compile_start{0};  // steady clock ms
    std::atomic<bool> cancel{false};
};

Progress g_progress;
std::wstring g_failure;  // the step that failed, written by the build thread before it ends
HWND g_window = nullptr;
bool g_start = false;  // the player picked Start once the game was ready
bool g_delete_compiler = false;  // ticked on the ready page
uint64_t g_compiler_bytes = 0;
constexpr UINT kBuildDone = WM_APP + 1;

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

fs::path CompilerDir() { return ExeDir() / "compiler"; }
fs::path WorkDir(const Image& image) { return CompilerDir() / "work" / image.exe; }
fs::path CodeDll(const Image& image) { return ExeDir() / (std::wstring(image.exe) + L".dll"); }
fs::path StampFile(const Image& image) { return WorkDir(image) / "built.txt"; }
std::wstring Quote(const fs::path& path) { return L"\"" + path.wstring() + L"\""; }
fs::path CompileLog() { return ExeDir() / "logs" / "compile.log"; }

// keeps the failed step for the message the player sees; false, to return
bool RecordFailure(const std::wstring& what) {
    g_failure = what;
    LOGE("Game code: %ls", what.c_str());
    return false;
}

// an image to build, with what its finished DLL is stamped with
struct Target {
    const Image* image = nullptr;
    std::vector<uint8_t> xbe;
    std::string stamp;
};

// a DLL serves the executable it was linked against and the xbe it was translated from
bool StampFor(const Image& image, const std::vector<uint8_t>& xbe, std::string& stamp) {
    WIN32_FILE_ATTRIBUTE_DATA exe{};
    fs::path path = ExeDir() / (std::wstring(image.exe) + L".exe");
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &exe)) return false;
    uint64_t size = (uint64_t(exe.nFileSizeHigh) << 32) | exe.nFileSizeLow;
    uint64_t time = (uint64_t(exe.ftLastWriteTime.dwHighDateTime) << 32) | exe.ftLastWriteTime.dwLowDateTime;
    stamp = std::to_string(size) + " " + std::to_string(time) + " " + Sha1(xbe);
    return true;
}

// build.bat's DLL has no stamp: it is current when newer than the executable it was linked against
bool Current(const Target& target) {
    std::error_code ec;
    fs::file_time_type built = fs::last_write_time(CodeDll(*target.image), ec);
    if (ec) return false;
    std::ifstream f(StampFile(*target.image));
    if (!f) return built > fs::last_write_time(ExeDir() / (std::wstring(target.image->exe) + L".exe"), ec);
    std::string line;
    return std::getline(f, line) && line == target.stamp;
}

// an image's xbe from the game in assets: the extracted file, else the mounted disc image
bool ReadXbe(const GameFiles& game, const char* name, std::vector<uint8_t>& out) {
    if (std::string_view(name) == IMAGE_XBE) {
        out = game.xbe;
        return true;
    }
    std::ifstream f(game.dir / name, std::ios::binary);
    if (f) {
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    }
    const kernel::Disc* disc = kernel::MountedDisc();
    return disc && disc->Load(name, out);
}

std::vector<Target> StaleTargets(const GameFiles& game) {
    std::vector<Target> stale;
    for (const Image& image : kImages) {
        Target target{&image};
        if (!ReadXbe(game, image.xbe, target.xbe)) {
            LOGW("game code: no %s in the game files, %ls is not built", image.xbe, image.exe);
            continue;
        }
        if (!StampFor(image, target.xbe, target.stamp) || Current(target)) continue;
        stale.push_back(std::move(target));
    }
    return stale;
}

// a source to compile and the object it makes
struct Job {
    fs::path source;
    fs::path object;
    uint64_t bytes = 0;
};

uint32_t PartitionCount(const Image& image) {
    try {
        toml::table t = toml::parse_file((CompilerDir() / "config" / image.config).string());
        return uint32_t(t["xbe"]["partitions"].value_or(int64_t(0)));
    } catch (const toml::parse_error&) {
        return 0;
    }
}

// the newest of what every object of an image is built from besides its own source
fs::file_time_type SharedInputsTime(const Image& image) {
    std::error_code ec;
    fs::file_time_type newest = fs::last_write_time(WorkDir(image) / "x86_recomp_shared.h", ec);
    newest = std::max(newest, fs::last_write_time(CompilerDir() / "clang.exe", ec));
    for (auto& e : fs::recursive_directory_iterator(CompilerDir() / "include", ec))
        newest = std::max(newest, e.last_write_time(ec));
    return newest;
}

// the image's sources whose object is missing or older than what it is built from
void AddJobs(const Image& image, std::vector<Job>& jobs) {
    fs::path work = WorkDir(image);
    std::vector<std::string> names{"x86_func_mapping"};
    for (uint32_t i = 0; i < PartitionCount(image); i++) names.push_back("x86_recomp." + std::to_string(i));
    fs::file_time_type shared = SharedInputsTime(image);
    for (const std::string& name : names) {
        Job job{work / (name + ".cpp"), work / (name + ".obj")};
        std::error_code ec;
        job.bytes = fs::file_size(job.source, ec);
        fs::file_time_type built = fs::last_write_time(job.object, ec);
        if (!ec && built > shared && built > fs::last_write_time(job.source, ec)) continue;
        jobs.push_back(std::move(job));
    }
}

// compilers that run at once: one a logical processor, as many as the free memory holds
size_t Slots() {
    MEMORYSTATUSEX mem{sizeof(mem)};
    GlobalMemoryStatusEx(&mem);
    size_t by_memory = size_t(mem.ullAvailPhys / kJobMemory);
    size_t by_cpu = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return std::clamp<size_t>(std::min(by_memory, by_cpu), 1, MAXIMUM_WAIT_OBJECTS);
}

// starts and waits on the build's processes; closing the job, or this process ending, ends them all
class Processes {
public:
    Processes() {
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
        DWORD attributes = FILE_ATTRIBUTE_NORMAL;
        log_ = CreateFileW(CompileLog().c_str(), FILE_APPEND_DATA, share, &inherit, CREATE_ALWAYS, attributes, nullptr);
    }
    ~Processes() {
        CloseHandle(job_);
        if (log_ != INVALID_HANDLE_VALUE) CloseHandle(log_);
    }

    // null when it did not start; its output goes to logs\compile.log
    HANDLE Start(std::wstring command, const fs::path& dir) {
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = log_;
        si.hStdError = log_;
        PROCESS_INFORMATION pi{};
        DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS;
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, flags, nullptr, dir.c_str(), &si, &pi)) {
            LOGE("game code: could not start %ls (%lu)", command.c_str(), GetLastError());
            return nullptr;
        }
        AssignProcessToJobObject(job_, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }

    // closes the process; 0 when it exited cleanly
    static DWORD ExitCode(HANDLE process) {
        DWORD code = 1;
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        return code;
    }
    static bool Succeeded(HANDLE process) { return ExitCode(process) == 0; }

    // false when cancelled, which ends every process of the build
    bool WaitAll(std::vector<HANDLE>& processes) {
        while (WaitForMultipleObjects(DWORD(processes.size()), processes.data(), TRUE, 250) == WAIT_TIMEOUT)
            if (g_progress.cancel) return Cancel();
        return true;
    }

    bool Cancel() {
        TerminateJobObject(job_, 1);
        return false;
    }

private:
    HANDLE job_ = nullptr;
    HANDLE log_ = INVALID_HANDLE_VALUE;
};

// xbe_recomp writes each image's C++ into its work folder, the images side by side
bool Translate(std::vector<Target>& targets, Processes& run) {
    std::vector<HANDLE> processes;
    for (Target& target : targets) {
        fs::path work = WorkDir(*target.image);
        fs::path xbe = work / target.image->xbe;
        std::ofstream(xbe, std::ios::binary).write(reinterpret_cast<const char*>(target.xbe.data()), target.xbe.size());
        fs::path config = CompilerDir() / "config" / target.image->config;
        std::wstring command = Quote(CompilerDir() / "xbe_recomp.exe") + L" " + Quote(config) + L" " + Quote(xbe);
        HANDLE process = run.Start(command + L" " + Quote(work), work);
        if (!process) return RecordFailure(L"xbe_recomp.exe could not be started.");
        processes.push_back(process);
    }
    if (!run.WaitAll(processes)) return false;
    bool ok = true;
    constexpr DWORD kDllNotFound = 0xC0000135;  // STATUS_DLL_NOT_FOUND, the process never started
    const wchar_t* missing_runtime = L"xbe_recomp.exe needs msvcp140.dll and vcruntime140.dll beside it in the compiler folder. Extract the whole release again, then start the game.";
    for (size_t i = 0; i < processes.size(); i++) {
        DWORD code = Processes::ExitCode(processes[i]);
        std::string xbe = targets[i].image->xbe;
        if (code == kDllNotFound) ok = RecordFailure(missing_runtime);
        else if (code) ok = RecordFailure(L"Translating " + std::wstring(xbe.begin(), xbe.end()) + L" failed.");
    }
    return ok;
}

std::wstring CompileCommand(const Job& job) {
    fs::path include = CompilerDir() / "include";
    std::wstring command = Quote(CompilerDir() / "clang.exe") + L" " + kClangFlags;
    command += L" -isystem " + Quote(include / "freestanding") + L" -I" + Quote(include) + L" -c ";
    return command + job.source.filename().wstring() + L" -o " + job.object.filename().wstring() + L".tmp";
}

// an object is written beside its final name and moved there once whole
bool FinishJob(const Job& job, HANDLE process) {
    std::wstring name = job.source.parent_path().filename().wstring() + L"\\" + job.source.filename().wstring();
    if (!Processes::Succeeded(process)) return RecordFailure(L"Compiling " + name + L" failed.");
    fs::path temp = job.object;
    temp += ".tmp";
    std::error_code ec;
    fs::rename(temp, job.object, ec);
    if (ec) return RecordFailure(L"Saving the compiled " + name + L" failed.");
    g_progress.bytes_done += job.bytes;
    g_progress.files_done++;
    return true;
}

// the biggest files first, as many at once as Slots allows
bool Compile(std::vector<Job>& jobs, Processes& run) {
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.bytes > b.bytes; });
    size_t slots = Slots();
    LOGR("Game code: compiling %zu files, %zu at a time", jobs.size(), slots);
    for (const Job& job : jobs) g_progress.bytes_total += job.bytes;
    g_progress.files_total = uint32_t(jobs.size());
    g_progress.first_wave = uint32_t(std::min(slots, jobs.size()));
    g_progress.compile_start = NowMs();
    g_progress.stage = Stage::Compiling;

    std::vector<HANDLE> running;
    std::vector<const Job*> running_jobs;
    size_t next = 0;
    while (next < jobs.size() || !running.empty()) {
        for (; running.size() < slots && next < jobs.size(); next++) {
            HANDLE process = run.Start(CompileCommand(jobs[next]), jobs[next].source.parent_path());
            if (!process) {
                RecordFailure(L"clang.exe could not be started.");
                return run.Cancel();
            }
            running.push_back(process);
            running_jobs.push_back(&jobs[next]);
        }
        DWORD wait = WaitForMultipleObjects(DWORD(running.size()), running.data(), FALSE, 250);
        if (g_progress.cancel) return run.Cancel();
        if (wait == WAIT_TIMEOUT) continue;
        size_t done = wait - WAIT_OBJECT_0;
        if (done >= running.size()) {
            RecordFailure(L"Waiting on the compilers failed.");
            return run.Cancel();
        }
        if (!FinishJob(*running_jobs[done], running[done])) return run.Cancel();
        running.erase(running.begin() + done);
        running_jobs.erase(running_jobs.begin() + done);
    }
    return true;
}

// lld links the objects against the executable's exports into the DLL, then the stamp says what it was built from
bool Link(const Target& target, Processes& run) {
    const Image& image = *target.image;
    fs::path work = WorkDir(image);
    std::wstring command = Quote(CompilerDir() / "lld-link.exe") + L" " + kLinkFlags + L" /def:exports.def";
    command += L" /implib:code.lib /out:" + Quote(CodeDll(image));
    command += L" " + Quote(CompilerDir() / (std::wstring(image.exe) + L".lib")) + L" x86_func_mapping.obj";
    for (uint32_t i = 0; i < PartitionCount(image); i++) command += L" x86_recomp." + std::to_wstring(i) + L".obj";
    std::wstring dll = CodeDll(image).filename().wstring();
    std::vector<HANDLE> process{run.Start(command, work)};
    if (!process[0]) return RecordFailure(L"lld-link.exe could not be started.");
    if (!run.WaitAll(process)) return false;
    if (!Processes::Succeeded(process[0])) {
        std::wstring hint = L" If the game is already running, close it, then start it again.";
        return RecordFailure(L"Linking " + dll + L" failed." + hint);
    }
    std::ofstream(StampFile(image)) << target.stamp << "\n";
    return true;
}

bool Build(std::vector<Target>& targets) {
    Processes run;
    if (!Translate(targets, run)) return false;
    std::vector<Job> jobs;
    for (const Target& target : targets) AddJobs(*target.image, jobs);
    if (!Compile(jobs, run)) return false;
    g_progress.stage = Stage::Linking;
    for (const Target& target : targets)
        if (!Link(target, run)) return false;
    return true;
}

// what stops a build before it starts, for the player to fix; empty when nothing does
std::wstring Blocker(const std::vector<Target>& targets) {
    for (const wchar_t* tool : {L"clang.exe", L"lld-link.exe", L"xbe_recomp.exe"}) {
        std::error_code ec;
        if (!fs::exists(CompilerDir() / tool, ec))
            return L"The compiler folder is missing files. Extract the whole release again, then start the game.";
    }
    for (const Target& target : targets) {
        std::error_code ec;
        fs::create_directories(WorkDir(*target.image), ec);
        if (ec)
            return L"The game's folder cannot be written to. Move it somewhere you can write to, such as Documents.";
    }
    ULARGE_INTEGER free{};
    if (GetDiskFreeSpaceExW(CompilerDir().c_str(), &free, nullptr, nullptr) && free.QuadPart < kDiskNeeded)
        return L"Preparing the game needs 1 GB of free space on the drive it is on.";
    return {};
}

std::wstring StageText() {
    switch (g_progress.stage.load()) {
        case Stage::Translating:
            return L"Translating the game's code";
        case Stage::Compiling:
            return L"Compiling the game's code, file " +
                   std::to_wstring(std::min(g_progress.files_done + 1, g_progress.files_total.load())) + L" of " +
                   std::to_wstring(g_progress.files_total);
        case Stage::Linking:
            return L"Linking the game's code";
        default:
            return L"Start the game now?";
    }
}

// from the rate the files compiled so far went at, once every compiler has finished one
std::wstring TimeLeftText() {
    if (g_progress.stage != Stage::Compiling) return {};
    uint64_t done = g_progress.bytes_done;
    if (g_progress.files_done < g_progress.first_wave || !done) return L"Working out the time left";
    double elapsed = double(NowMs() - g_progress.compile_start);
    double left = elapsed * double(g_progress.bytes_total - done) / double(done);
    int minutes = int(left / 60000.0 + 0.5);
    if (minutes < 1) return L"Less than a minute left";
    return L"About " + std::to_wstring(minutes) + (minutes == 1 ? L" minute left" : L" minutes left");
}

float Fraction() {
    switch (g_progress.stage.load()) {
        case Stage::Translating:
            return 0.0f;
        case Stage::Compiling:
            return g_progress.bytes_total ? float(g_progress.bytes_done) / float(g_progress.bytes_total) : 0.0f;
        default:
            return 1.0f;
    }
}

void DrawLine(HDC dc, RECT row, const std::wstring& text, COLORREF color, UINT align) {
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), int(text.size()), &row, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

void FillBox(HDC dc, const RECT& box, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &box, brush);
    DeleteObject(brush);
}

// top to bottom: a heading, the stage, the bar, then the percentage and time left, or the buttons once ready
struct Layout {
    RECT heading;
    RECT stage;
    RECT bar;
    RECT bottom;
    RECT start;
    RECT exit;
    RECT delete_toggle;
};

Layout Arrange(const RECT& client, int unit) {
    Layout l;
    l.heading = {client.left + unit, client.top + unit, client.right - unit, client.top + unit * 3};
    l.stage = l.heading;
    OffsetRect(&l.stage, 0, unit * 2);
    l.bar = {l.stage.left, l.stage.bottom + unit / 2, l.stage.right, l.stage.bottom + unit / 2 + unit};
    l.bottom = {l.bar.left, l.bar.bottom + unit / 2, l.bar.right, l.bar.bottom + unit / 2 + unit * 2};
    l.exit = {l.bottom.right - unit * 8, l.bottom.top, l.bottom.right, l.bottom.bottom};
    l.start = {l.exit.left - unit * 9, l.bottom.top, l.exit.left - unit, l.bottom.bottom};
    l.delete_toggle = {l.bottom.left, l.bottom.top, l.start.left - unit, l.bottom.bottom};
    return l;
}

// how lit each ready page control is, 0 to 1, fading with the cursor over it
struct Glow {
    float start = 0;
    float exit = 0;
    float toggle = 0;
};
Glow g_glow;
bool g_pad = false;  // a controller is connected, so the labels name its buttons
constexpr float kFadeMs = 120.0f;

COLORREF Mix(COLORREF from, COLORREF to, float t) {
    auto channel = [t](int a, int b) { return BYTE(a + (b - a) * t + 0.5f); };
    return RGB(channel(GetRValue(from), GetRValue(to)), channel(GetGValue(from), GetGValue(to)),
               channel(GetBValue(from), GetBValue(to)));
}

// the controller button that presses a control, shown only with a controller connected
std::wstring Label(const std::wstring& text, const wchar_t* button) {
    return g_pad ? text + L" (" + button + L")" : text;
}

void PaintButton(HDC dc, const RECT& box, const std::wstring& text, float glow) {
    FillBox(dc, box, Mix(kBarEmpty, kBarFull, glow));
    DrawLine(dc, box, text, Mix(kText, kBackground, glow), DT_CENTER);
}

void PaintChoices(HDC dc, const Layout& l, int unit) {
    PaintButton(dc, l.start, Label(L"Start", L"A"), g_glow.start);
    PaintButton(dc, l.exit, Label(L"Exit", L"B"), g_glow.exit);
    int middle = (l.delete_toggle.top + l.delete_toggle.bottom) / 2;
    RECT box{l.delete_toggle.left, middle - unit / 2, l.delete_toggle.left + unit, middle + unit / 2};
    FillBox(dc, box, Mix(kBarEmpty, kBarFull, g_glow.toggle));
    InflateRect(&box, -unit / 4, -unit / 4);
    if (g_delete_compiler) FillBox(dc, box, Mix(kBarFull, kBackground, g_glow.toggle));
    RECT label = l.delete_toggle;
    label.left += unit * 3 / 2;
    std::wstring megabytes = std::to_wstring(g_compiler_bytes >> 20);
    DrawLine(dc, label, Label(L"Delete the compiler files, " + megabytes + L" MB", L"X"), kText, DT_LEFT);
}

void Paint(HDC dc, const RECT& client, int unit) {
    Layout l = Arrange(client, unit);
    bool ready = g_progress.stage == Stage::Ready;
    FillBox(dc, client, kBackground);
    SetBkMode(dc, TRANSPARENT);
    const wchar_t* heading = ready ? L"The game is ready." : L"Preparing the game for this PC. This only happens once.";
    DrawLine(dc, l.heading, heading, kText, DT_LEFT);
    DrawLine(dc, l.stage, StageText(), kDim, DT_LEFT);
    FillBox(dc, l.bar, kBarEmpty);
    RECT filled = l.bar;
    filled.right = l.bar.left + LONG(float(l.bar.right - l.bar.left) * Fraction());
    FillBox(dc, filled, kBarFull);
    if (ready) {
        PaintChoices(dc, l, unit);
        return;
    }
    DrawLine(dc, l.bottom, std::to_wstring(int(Fraction() * 100.0f)) + L"%", kDim, DT_LEFT);
    DrawLine(dc, l.bottom, TimeLeftText(), kDim, DT_RIGHT);
}

int Unit(HWND hwnd) { return MulDiv(10, GetDpiForWindow(hwnd), 96); }

// painted off screen, then copied, so the bar does not flicker
void PaintWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
    HGDIOBJ old_font = SelectObject(memory, reinterpret_cast<HFONT>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)));
    Paint(memory, client, Unit(hwnd));
    BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old_font);
    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(hwnd, &ps);
}

// what the compiler folder holds, shown beside the delete box
uint64_t CompilerBytes() {
    uint64_t bytes = 0;
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(CompilerDir(), ec))
        if (e.is_regular_file(ec)) bytes += e.file_size(ec);
    return bytes;
}

// the game runs from the DLLs alone; a later release brings the compiler again
void DeleteCompiler() {
    std::error_code ec;
    fs::remove_all(CompilerDir(), ec);
    if (ec) LOGE("Game code: deleting the compiler folder failed: %s", ec.message().c_str());
    else LOGR("Game code: compiler folder deleted");
}

void CloseWindowWith(HWND hwnd, bool start) {
    g_start = start;
    if (g_progress.stage == Stage::Ready && g_delete_compiler) DeleteCompiler();
    DeleteObject(reinterpret_cast<HFONT>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)));
    DestroyWindow(hwnd);
}

// once ready: Start or Enter starts the game, Exit, Escape or the close button leaves, Space ticks the delete box
void OnClick(HWND hwnd, LPARAM lp) {
    if (g_progress.stage != Stage::Ready) return;
    RECT client;
    GetClientRect(hwnd, &client);
    Layout l = Arrange(client, Unit(hwnd));
    POINT at{short(LOWORD(lp)), short(HIWORD(lp))};
    if (PtInRect(&l.start, at)) CloseWindowWith(hwnd, true);
    else if (PtInRect(&l.exit, at)) CloseWindowWith(hwnd, false);
    else if (PtInRect(&l.delete_toggle, at)) g_delete_compiler = !g_delete_compiler;
}

float Approach(float glow, bool lit, float step) {
    return lit ? std::min(glow + step, 1.0f) : std::max(glow - step, 0.0f);
}

// read with each repaint: the cursor, so leaving the window or another window over it unlights too, and controllers
void UpdateReadyPage(HWND hwnd) {
    static int64_t last = NowMs();
    int64_t now = NowMs();
    float step = float(now - last) / kFadeMs;
    last = now;
    g_pad = false;
    for (int slot = 0; slot < 4 && !g_pad; slot++) g_pad = input::PadConnected(slot);
    POINT at;
    GetCursorPos(&at);
    bool over = WindowFromPoint(at) == hwnd;
    ScreenToClient(hwnd, &at);
    RECT client;
    GetClientRect(hwnd, &client);
    Layout l = Arrange(client, Unit(hwnd));
    g_glow.start = Approach(g_glow.start, over && PtInRect(&l.start, at), step);
    g_glow.exit = Approach(g_glow.exit, over && PtInRect(&l.exit, at), step);
    g_glow.toggle = Approach(g_glow.toggle, over && PtInRect(&l.delete_toggle, at), step);
}

// a controller's A starts, B leaves and X ticks the delete box, once ready; on the press, not the hold
void PollController(HWND hwnd) {
    static uint16_t held = 0;
    uint16_t now = input::HeldOnAnyController();
    uint16_t pressed = now & ~held;
    held = now;
    if (g_progress.stage != Stage::Ready) return;
    if (pressed & XINPUT_GAMEPAD_X) g_delete_compiler = !g_delete_compiler;
    if (pressed & XINPUT_GAMEPAD_A) CloseWindowWith(hwnd, true);
    else if (pressed & XINPUT_GAMEPAD_B) CloseWindowWith(hwnd, false);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    bool ready = g_progress.stage == Stage::Ready;
    switch (msg) {
        case WM_PAINT:
            PaintWindow(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (ready) UpdateReadyPage(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            PollController(hwnd);
            return 0;
        case WM_LBUTTONUP:
            OnClick(hwnd, lp);
            return 0;
        case WM_KEYDOWN:
            if (ready && wp == VK_SPACE) g_delete_compiler = !g_delete_compiler;
            if (ready && (wp == VK_RETURN || wp == VK_ESCAPE)) CloseWindowWith(hwnd, wp == VK_RETURN);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_CLOSE: {
            if (ready) {
                CloseWindowWith(hwnd, false);
                return 0;
            }
            const wchar_t* question = L"Stop preparing the game? What is done so far is kept, and the next start carries on.";
            if (MessageBoxW(hwnd, question, kTitle, MB_YESNO | MB_ICONQUESTION) == IDYES) g_progress.cancel = true;
            return 0;
        }
        case kBuildDone:
            if (!wp) {
                CloseWindowWith(hwnd, false);  // failed or stopped; ready stays up for the player's pick
                return 0;
            }
            g_compiler_bytes = CompilerBytes();
            FlashWindow(hwnd, TRUE);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// centred on the primary monitor, sized for its DPI, in the message font
HWND OpenWindow() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = L"SCDAGameCode";
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, MulDiv(520, dpi, 96), MulDiv(130, dpi, 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, kTitle, style, x, y, w, h, nullptr, nullptr, inst, nullptr);
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(CreateFontIndirectW(&metrics.lfMessageFont)));
    SetTimer(hwnd, 1, 16, nullptr);  // the controller and cursor are read with each repaint
    ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

// the last lines of logs\compile.log, where a compiler's error is
std::wstring CompileLogTail() {
    constexpr size_t kTailBytes = 1500;
    std::ifstream f(CompileLog(), std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamoff size = f.tellg();
    std::streamoff start = size > std::streamoff(kTailBytes) ? size - std::streamoff(kTailBytes) : 0;
    std::string tail(size_t(size - start), '\0');
    f.seekg(start);
    f.read(tail.data(), std::streamsize(tail.size()));
    if (start) tail.erase(0, tail.find('\n') + 1);  // from a whole line
    int length = MultiByteToWideChar(CP_UTF8, 0, tail.data(), int(tail.size()), nullptr, 0);
    std::wstring text(size_t(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, tail.data(), int(tail.size()), text.data(), length);
    return text;
}

void ShowFailure() {
    std::wstring text = L"Preparing the game failed. " + g_failure;
    if (std::wstring tail = CompileLogTail(); !tail.empty()) text += L"\n\nThe last lines of the log:\n\n" + tail;
    text += L"\n\nThe full details are in logs\\compile.log.";
    MessageBoxW(nullptr, text.c_str(), kTitle, MB_ICONERROR);
}

// the build on its own thread, the window on this one; false when it failed, the player stopped it, or chose Exit
bool BuildWithWindow(std::vector<Target>& targets) {
    if (std::wstring blocker = Blocker(targets); !blocker.empty()) {
        MessageBoxW(nullptr, blocker.c_str(), kTitle, MB_ICONERROR);
        return false;
    }
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);  // no sleep while it works
    input::StartControllers();
    g_window = OpenWindow();
    bool ok = false;
    std::thread worker([&] {
        ok = Build(targets);
        if (ok) g_progress.stage = Stage::Ready;
        PostMessageW(g_window, kBuildDone, ok, 0);
    });
    MSG msg;
    while (IsWindow(g_window) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    worker.join();
    SetThreadExecutionState(ES_CONTINUOUS);
    if (!ok && !g_progress.cancel) ShowFailure();
    return ok && g_start;
}

}  // namespace

bool PrepareGameCode(const GameFiles& game) {
    // one build at a time: a second copy of the game started meanwhile waits for it, then finds the DLL current
    HANDLE lock = CreateMutexW(nullptr, FALSE, L"SCDA-XBERecomp-GameCode");
    WaitForSingleObject(lock, INFINITE);
    std::vector<Target> stale = StaleTargets(game);
    for (const Target& target : stale) LOGR("Game code: building %ls.dll", target.image->exe);
    bool built = stale.empty() || BuildWithWindow(stale);
    ReleaseMutex(lock);
    CloseHandle(lock);
    if (!built) return false;
    for (const Image& image : kImages)
        if (std::string_view(image.xbe) == IMAGE_XBE && dispatch::Load(CodeDll(image))) return true;
    MessageBoxW(nullptr, L"The game's code could not be loaded, see the log in the logs folder.", kTitle, MB_ICONERROR);
    return false;
}

}  // namespace app
