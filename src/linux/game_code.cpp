// Linux: the recompiled game code is built on the player's PC, as app/game_code.cpp does it on Windows: xbe_recomp
// translates each xbe in assets to C++, the system's clang compiles it and links <exe>.so beside the executables.
// The first start does this behind a progress window; finished objects are kept, so a closed window or a crash
// loses only the files in progress and the next start carries on.
#include <SDL3/SDL.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/disc.h"
#include "linux/win32_compat.h"

extern char** environ;

namespace dispatch {
bool Load(const std::filesystem::path& dll);  // cpu/dispatch.cpp
}

namespace app {

namespace {

namespace fs = std::filesystem;

struct Image {
    const char* exe;
    const char* xbe;
    const char* config;
};
constexpr Image kImages[] = {
    {"SCDA", "default.xbe", "scda_v2.toml"},
    {"SCDA_Versus", "sc4_adv.xbe", "sc4_adv.toml"},
};

// as CMakeLists.txt compiles it
const std::vector<std::string> kClangFlags = {
    "-std=c++20", "-O2", "-march=x86-64-v2", "-fno-strict-aliasing", "-fno-stack-protector", "-fPIC", "-DNDEBUG",
    "-DX86_GAME_CODE", "-nostdinc", "-nostdinc++", "-w"};
constexpr const char* kTitle = "Tom Clancy's Splinter Cell: Double Agent - XBE Recomp";
constexpr uint64_t kJobMemory = 512ull << 20;
constexpr uint64_t kDiskNeeded = 1ull << 30;

enum class Stage { Translating, Compiling, Linking, Ready };

struct Progress {
    std::atomic<Stage> stage{Stage::Translating};
    std::atomic<uint64_t> bytes_done{0};
    std::atomic<uint64_t> bytes_total{0};
    std::atomic<uint32_t> files_done{0};
    std::atomic<uint32_t> files_total{0};
    std::atomic<uint32_t> first_wave{1};
    std::atomic<int64_t> compile_start{0};
    std::atomic<bool> cancel{false};
};

Progress g_progress;
std::string g_failure;
bool g_start = false;

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// the compiler's files beside the executables (read only in an AppImage); its work and the .so go where the game writes
fs::path CompilerDir() { return InstallDir() / "compiler"; }
fs::path WorkDir(const Image& image) { return ExeDir() / "compiler" / "work" / image.exe; }
fs::path BundledLibs() { return CompilerDir() / "lib"; }
fs::path CodeDll(const Image& image) { return ExeDir() / (std::string(image.exe) + ".so"); }
fs::path StampFile(const Image& image) { return WorkDir(image) / "built.txt"; }
fs::path CompileLog() { return ExeDir() / "logs" / "compile.log"; }

bool RecordFailure(const std::string& what) {
    g_failure = what;
    LOGE("Game code: %s", what.c_str());
    return false;
}

// a program on PATH, empty when there is none
std::string FindProgram(const char* name) {
    if (std::strchr(name, '/')) return access(name, X_OK) == 0 ? name : "";
    const char* path = std::getenv("PATH");
    std::string_view rest = path ? path : "/usr/bin:/bin";
    while (!rest.empty()) {
        size_t colon = rest.find(':');
        std::string dir(rest.substr(0, colon));
        rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
        std::string full = (dir.empty() ? "." : dir) + "/" + name;
        if (access(full.c_str(), X_OK) == 0) return full;
    }
    return {};
}

// SCDA_CXX, else the clang the AppImage carries (compiler/bin), else clang++ on PATH
std::string Compiler() {
    if (const char* c = std::getenv("SCDA_CXX"); c && *c) return FindProgram(c);
    if (std::string b = (CompilerDir() / "bin" / "clang").string(); access(b.c_str(), X_OK) == 0) return b;
    return FindProgram("clang++");
}

// the carried lld, else ld.lld on PATH; empty: clang++ links with the system's linker
std::string Linker() {
    if (std::string b = (CompilerDir() / "bin" / "ld.lld").string(); access(b.c_str(), X_OK) == 0) return b;
    return FindProgram("ld.lld");
}

struct Target {
    const Image* image = nullptr;
    std::vector<uint8_t> xbe;
    std::string stamp;
};

bool StampFor(const Image& image, const std::vector<uint8_t>& xbe, std::string& stamp) {
    std::error_code ec;
    fs::path path = InstallDir() / image.exe;
    uint64_t size = fs::file_size(path, ec);
    if (ec) return false;
    auto time = fs::last_write_time(path, ec).time_since_epoch().count();
    stamp = std::to_string(size) + " " + std::to_string(time) + " " + Sha1(xbe);
    return true;
}

// a build's .so has no stamp: it is current when newer than the executable
bool Current(const Target& target) {
    std::error_code ec;
    fs::file_time_type built = fs::last_write_time(CodeDll(*target.image), ec);
    if (ec) return false;
    std::ifstream f(StampFile(*target.image));
    if (!f) return built > fs::last_write_time(InstallDir() / target.image->exe, ec);
    std::string line;
    return std::getline(f, line) && line == target.stamp;
}

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
        std::error_code ec;
        if (!fs::exists(InstallDir() / image.exe, ec)) continue;  // only the executables that are here
        if (!ReadXbe(game, image.xbe, target.xbe)) {
            LOGW("game code: no %s in the game files, %s is not built", image.xbe, image.exe);
            continue;
        }
        if (!StampFor(image, target.xbe, target.stamp) || Current(target)) continue;
        stale.push_back(std::move(target));
    }
    return stale;
}

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

fs::file_time_type SharedInputsTime(const Image& image) {
    std::error_code ec;
    fs::file_time_type newest = fs::last_write_time(WorkDir(image) / "x86_recomp_shared.h", ec);
    for (auto& e : fs::recursive_directory_iterator(CompilerDir() / "include", ec))
        newest = std::max(newest, e.last_write_time(ec));
    return newest;
}

void AddJobs(const Image& image, std::vector<Job>& jobs) {
    fs::path work = WorkDir(image);
    std::vector<std::string> names{"x86_func_mapping"};
    for (uint32_t i = 0; i < PartitionCount(image); i++) names.push_back("x86_recomp." + std::to_string(i));
    fs::file_time_type shared = SharedInputsTime(image);
    for (const std::string& name : names) {
        Job job{work / (name + ".cpp"), work / (name + ".o")};
        std::error_code ec;
        job.bytes = fs::file_size(job.source, ec);
        fs::file_time_type built = fs::last_write_time(job.object, ec);
        if (!ec && built > shared && built > fs::last_write_time(job.source, ec)) continue;
        jobs.push_back(std::move(job));
    }
}

size_t Slots() {
    uint64_t avail = uint64_t(sysconf(_SC_AVPHYS_PAGES)) * uint64_t(sysconf(_SC_PAGESIZE));
    size_t by_memory = size_t(avail / kJobMemory);
    size_t by_cpu = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp<size_t>(std::min(by_memory, by_cpu), 1, 64);
}

// the build's processes, in a process group of their own so a cancel ends them all; output to logs/compile.log
class Processes {
public:
    Processes() {
        std::error_code ec;
        fs::create_directories(CompileLog().parent_path(), ec);
        log_ = open(CompileLog().c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        // the carried compiler finds its libraries beside it; nothing else the game starts sees them
        std::string libs = BundledLibs().string();
        bool carried = fs::is_directory(BundledLibs());
        for (char** e = environ; *e; e++) {
            std::string_view v = *e;
            if (carried && v.rfind("LD_LIBRARY_PATH=", 0) == 0) continue;
            env_strings_.emplace_back(v);
        }
        if (carried) {
            const char* old = std::getenv("LD_LIBRARY_PATH");
            env_strings_.push_back("LD_LIBRARY_PATH=" + libs + (old && *old ? ":" + std::string(old) : ""));
        }
        for (std::string& e : env_strings_) env_.push_back(e.data());
        env_.push_back(nullptr);
    }
    ~Processes() {
        if (log_ >= 0) close(log_);
    }

    // 0 when it did not start
    pid_t Start(const std::vector<std::string>& args, const fs::path& dir) {
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        if (log_ >= 0) {
            posix_spawn_file_actions_adddup2(&fa, log_, 1);
            posix_spawn_file_actions_adddup2(&fa, log_, 2);
        }
        posix_spawn_file_actions_addchdir_np(&fa, dir.c_str());
        posix_spawnattr_t at;
        posix_spawnattr_init(&at);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&at, 0);  // a group of its own: a cancel ends it and what it started
        std::vector<char*> argv;
        std::vector<std::string> copy = args;
        for (std::string& a : copy) argv.push_back(a.data());
        argv.push_back(nullptr);
        pid_t pid = 0;
        int r = posix_spawn(&pid, copy[0].c_str(), &fa, &at, argv.data(), env_.data());
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&at);
        if (r != 0) {
            LOGE("game code: could not start %s (%s)", copy[0].c_str(), std::strerror(r));
            return 0;
        }
        live_.push_back(pid);
        setpriority(PRIO_PROCESS, id_t(pid), 10);
        return pid;
    }

    // the exit status of one that ended, -1 while none has; pid set to it
    static int Reap(pid_t& pid, bool block) {
        int status = 0;
        pid_t done = waitpid(-1, &status, block ? 0 : WNOHANG);
        if (done <= 0) return -1;
        pid = done;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    }

    bool WaitAll(std::vector<pid_t> pids, std::vector<int>* codes = nullptr) {
        std::vector<int> out(pids.size(), 1);
        size_t left = pids.size();
        while (left) {
            if (g_progress.cancel) return Cancel();
            pid_t pid = 0;
            int code = Reap(pid, false);
            if (code < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            for (size_t i = 0; i < pids.size(); i++)
                if (pids[i] == pid) {
                    out[i] = code;
                    pids[i] = 0;
                    left--;
                }
        }
        if (codes) *codes = out;
        return true;
    }

    bool Cancel() {
        for (pid_t pid : live_) kill(-pid, SIGTERM);
        while (waitpid(-1, nullptr, 0) > 0) {
        }
        return false;
    }

private:
    int log_ = -1;
    std::vector<std::string> env_strings_;
    std::vector<char*> env_;
    std::vector<pid_t> live_;  // every process started; one already reaped is no group any more
};

bool Translate(std::vector<Target>& targets, Processes& run) {
    std::vector<pid_t> pids;
    for (Target& target : targets) {
        fs::path work = WorkDir(*target.image);
        fs::path xbe = work / target.image->xbe;
        std::ofstream(xbe, std::ios::binary).write(reinterpret_cast<const char*>(target.xbe.data()), std::streamsize(target.xbe.size()));
        fs::path config = CompilerDir() / "config" / target.image->config;
        pid_t pid = run.Start({(CompilerDir() / "xbe_recomp").string(), config.string(), xbe.string(), work.string()}, work);
        if (!pid) return RecordFailure("xbe_recomp could not be started.");
        pids.push_back(pid);
    }
    std::vector<int> codes;
    if (!run.WaitAll(pids, &codes)) return false;
    for (size_t i = 0; i < codes.size(); i++)
        if (codes[i]) return RecordFailure(std::string("Translating ") + targets[i].image->xbe + " failed.");
    return true;
}

std::vector<std::string> CompileCommand(const std::string& cxx, const Job& job) {
    fs::path include = CompilerDir() / "include";
    std::vector<std::string> c{cxx};
    c.insert(c.end(), kClangFlags.begin(), kClangFlags.end());
    c.insert(c.end(), {"-isystem", (include / "freestanding").string(), "-I" + include.string(), "-c",
                       job.source.filename().string(), "-o", job.object.filename().string() + ".tmp"});
    return c;
}

bool FinishJob(const Job& job, int code) {
    std::string name = job.source.parent_path().filename().string() + "/" + job.source.filename().string();
    if (code) return RecordFailure("Compiling " + name + " failed.");
    fs::path temp = job.object;
    temp += ".tmp";
    std::error_code ec;
    fs::rename(temp, job.object, ec);
    if (ec) return RecordFailure("Saving the compiled " + name + " failed.");
    g_progress.bytes_done += job.bytes;
    g_progress.files_done++;
    return true;
}

bool Compile(const std::string& cxx, std::vector<Job>& jobs, Processes& run) {
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.bytes > b.bytes; });
    size_t slots = Slots();
    LOGR("Game code: compiling %zu files, %zu at a time", jobs.size(), slots);
    for (const Job& job : jobs) g_progress.bytes_total += job.bytes;
    g_progress.files_total = uint32_t(jobs.size());
    g_progress.first_wave = uint32_t(std::min(slots, jobs.size()));
    g_progress.compile_start = NowMs();
    g_progress.stage = Stage::Compiling;
    std::vector<std::pair<pid_t, const Job*>> running;
    size_t next = 0;
    while (next < jobs.size() || !running.empty()) {
        for (; running.size() < slots && next < jobs.size(); next++) {
            pid_t pid = run.Start(CompileCommand(cxx, jobs[next]), jobs[next].source.parent_path());
            if (!pid) {
                RecordFailure("clang++ could not be started.");
                return run.Cancel();
            }
            running.push_back({pid, &jobs[next]});
        }
        if (g_progress.cancel) return run.Cancel();
        pid_t pid = 0;
        int code = Processes::Reap(pid, false);
        if (code < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        auto it = std::find_if(running.begin(), running.end(), [&](const auto& r) { return r.first == pid; });
        if (it == running.end()) continue;
        if (!FinishJob(*it->second, code)) return run.Cancel();
        running.erase(it);
    }
    return true;
}

bool Link(const std::string& cxx, const Target& target, Processes& run) {
    const Image& image = *target.image;
    fs::path work = WorkDir(image);
    fs::path out = CodeDll(image);
    fs::path temp = out;
    temp += ".tmp";
    // lld alone: no C runtime start files and no libraries, which need a system's development packages. What the
    // code calls in libc, libm and libstdc++ is bound when the executable (which has them) opens it
    std::string lld = Linker();
    std::vector<std::string> c;
    if (!lld.empty()) c = {lld, "-shared", "--icf=none", "-o", temp.string(), "x86_func_mapping.o"};
    else c = {cxx, "-shared", "-o", temp.string(), "x86_func_mapping.o"};
    for (uint32_t i = 0; i < PartitionCount(image); i++) c.push_back("x86_recomp." + std::to_string(i) + ".o");
    if (lld.empty()) c.push_back("-lm");
    pid_t pid = run.Start(c, work);
    if (!pid) return RecordFailure("clang++ could not be started to link.");
    std::vector<int> codes;
    if (!run.WaitAll({pid}, &codes)) return false;
    if (codes[0]) return RecordFailure("Linking " + out.filename().string() + " failed.");
    // replaced whole: a running copy keeps the file it mapped
    std::error_code ec;
    fs::rename(temp, out, ec);
    if (ec) return RecordFailure("Saving " + out.filename().string() + " failed.");
    std::ofstream(StampFile(image)) << target.stamp << "\n";
    return true;
}

bool Build(std::vector<Target>& targets) {
    std::string cxx = Compiler();
    Processes run;
    if (!Translate(targets, run)) return false;
    std::vector<Job> jobs;
    for (const Target& target : targets) AddJobs(*target.image, jobs);
    if (!Compile(cxx, jobs, run)) return false;
    g_progress.stage = Stage::Linking;
    for (const Target& target : targets)
        if (!Link(cxx, target, run)) return false;
    return true;
}

std::string Blocker(const std::vector<Target>& targets) {
    std::error_code ec;
    if (!fs::exists(CompilerDir() / "xbe_recomp", ec))
        return "The compiler folder is missing files. Extract the whole release again, then start the game.";
    if (Compiler().empty())
        return "Preparing the game needs clang (clang++ on PATH, or SCDA_CXX naming it). Install clang and lld with "
               "your package manager, then start the game again.";
    std::error_code wc;
    fs::create_directories(ExeDir() / "compiler" / "work", wc);
    for (const Target& target : targets) {
        fs::create_directories(WorkDir(*target.image), ec);
        if (ec) return "The game's folder cannot be written to. Move it somewhere you can write to, such as your home folder.";
    }
    struct statvfs st{};
    if (statvfs(ExeDir().c_str(), &st) == 0 && uint64_t(st.f_bavail) * st.f_frsize < kDiskNeeded)
        return "Preparing the game needs 1 GB of free space on the drive it is on.";
    return {};
}

std::string StageText() {
    switch (g_progress.stage.load()) {
        case Stage::Translating: return "Translating the game's code";
        case Stage::Compiling:
            return "Compiling the game's code, file " +
                   std::to_string(std::min(g_progress.files_done + 1, g_progress.files_total.load())) + " of " +
                   std::to_string(g_progress.files_total);
        case Stage::Linking: return "Linking the game's code";
        default: return "Start the game now?";
    }
}

std::string TimeLeftText() {
    if (g_progress.stage != Stage::Compiling) return {};
    uint64_t done = g_progress.bytes_done;
    if (g_progress.files_done < g_progress.first_wave || !done) return "Working out the time left";
    double elapsed = double(NowMs() - g_progress.compile_start);
    double left = elapsed * double(g_progress.bytes_total - done) / double(done);
    int minutes = int(left / 60000.0 + 0.5);
    if (minutes < 1) return "Less than a minute left";
    return "About " + std::to_string(minutes) + (minutes == 1 ? " minute left" : " minutes left");
}

float Fraction() {
    switch (g_progress.stage.load()) {
        case Stage::Translating: return 0.0f;
        case Stage::Compiling:
            return g_progress.bytes_total ? float(g_progress.bytes_done) / float(g_progress.bytes_total) : 0.0f;
        default: return 1.0f;
    }
}

// the F10 panel's colours
void Colour(SDL_Renderer* r, uint8_t red, uint8_t green, uint8_t blue) { SDL_SetRenderDrawColor(r, red, green, blue, 255); }

void Paint(SDL_Renderer* r, int w, int h, bool pad) {
    constexpr float kScale = 2.0f;  // SDL's 8x8 debug font, doubled
    bool ready = g_progress.stage == Stage::Ready;
    Colour(r, 10, 11, 10);
    SDL_RenderClear(r);
    SDL_SetRenderScale(r, kScale, kScale);
    float u = 10.0f / kScale;
    Colour(r, 227, 232, 219);
    SDL_RenderDebugText(r, u, u, ready ? "The game is ready." : "Preparing the game for this PC. This only happens once.");
    Colour(r, 145, 153, 138);
    SDL_RenderDebugText(r, u, u * 3, StageText().c_str());
    SDL_SetRenderScale(r, 1, 1);
    SDL_FRect bar{20, 80, float(w - 40), 18};
    Colour(r, 28, 33, 26);
    SDL_RenderFillRect(r, &bar);
    bar.w *= Fraction();
    Colour(r, 145, 158, 130);
    SDL_RenderFillRect(r, &bar);
    SDL_SetRenderScale(r, kScale, kScale);
    Colour(r, 145, 153, 138);
    float bottom = float(h) / kScale - u * 2;
    if (ready) {
        std::string choices = pad ? "Enter (A): Start      Escape (B): Exit" : "Enter: Start      Escape: Exit";
        Colour(r, 227, 232, 219);
        SDL_RenderDebugText(r, u, bottom, choices.c_str());
    } else {
        SDL_RenderDebugText(r, u, bottom, (std::to_string(int(Fraction() * 100.0f)) + "%   " + TimeLeftText()).c_str());
    }
    SDL_SetRenderScale(r, 1, 1);
    SDL_RenderPresent(r);
}

void ShowFailure() {
    std::string text = "Preparing the game failed. " + g_failure;
    std::ifstream f(CompileLog(), std::ios::binary | std::ios::ate);
    if (f) {
        constexpr std::streamoff kTail = 1500;
        std::streamoff size = f.tellg();
        std::streamoff start = size > kTail ? size - kTail : 0;
        std::string tail(size_t(size - start), '\0');
        f.seekg(start);
        f.read(tail.data(), std::streamsize(tail.size()));
        if (start) tail.erase(0, tail.find('\n') + 1);
        if (!tail.empty()) text += "\n\nThe last lines of the log:\n\n" + tail;
    }
    text += "\n\nThe full details are in logs/compile.log.";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kTitle, text.c_str(), nullptr);
}

bool BuildWithWindow(std::vector<Target>& targets) {
    if (std::string blocker = Blocker(targets); !blocker.empty()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kTitle, blocker.c_str(), nullptr);
        return false;
    }
    input::StartControllers();
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_CreateWindowAndRenderer(kTitle, 1040, 260, 0, &window, &renderer);
    std::atomic<bool> finished{false};
    bool ok = false;
    std::thread worker([&] {
        ok = Build(targets);
        if (ok) g_progress.stage = Stage::Ready;
        finished = true;
    });
    uint16_t held = 0;
    bool open = true;
    while (open) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            bool ready = g_progress.stage == Stage::Ready;
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                if (ready) {
                    open = false;
                } else {
                    const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes"},
                                                                {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No"}};
                    SDL_MessageBoxData data{SDL_MESSAGEBOX_INFORMATION, window, kTitle,
                                            "Stop preparing the game? What is done so far is kept, and the next start carries on.",
                                            2, buttons, nullptr};
                    int pressed = 0;
                    if (SDL_ShowMessageBox(&data, &pressed) && pressed) g_progress.cancel = true;
                }
            } else if (e.type == SDL_EVENT_KEY_DOWN && ready) {
                if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) g_start = true, open = false;
                else if (e.key.key == SDLK_ESCAPE) open = false;
            } else if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                input::OnPadEvent(e.type == SDL_EVENT_GAMEPAD_ADDED, e.gdevice.which);
            }
        }
        uint16_t now = input::HeldOnAnyController();
        uint16_t pressed = now & ~held;
        held = now;
        if (g_progress.stage == Stage::Ready) {
            if (pressed & XINPUT_GAMEPAD_A) g_start = true, open = false;
            else if (pressed & XINPUT_GAMEPAD_B) open = false;
        }
        if (finished && !ok) open = false;
        bool pad = false;
        for (int slot = 0; slot < 4 && !pad; slot++) pad = input::PadConnected(slot);
        int w = 0, h = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &w, &h);
        Paint(renderer, w, h, pad);
        SDL_Delay(16);
    }
    if (!finished) g_progress.cancel = true;
    worker.join();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    if (!ok && !g_progress.cancel) ShowFailure();
    return ok && g_start;
}

}  // namespace

bool PrepareGameCode(const GameFiles& game) {
    // one build at a time: a second copy of the game started meanwhile waits for it, then finds the .so current
    std::error_code ec;
    fs::create_directories(ExeDir() / "compiler", ec);
    int lock = open((ExeDir() / "compiler" / ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock >= 0) flock(lock, LOCK_EX);
    std::vector<Target> stale = StaleTargets(game);
    for (const Target& target : stale) LOGR("Game code: building %s.so", target.image->exe);
    bool built = stale.empty() || BuildWithWindow(stale);
    if (lock >= 0) close(lock);
    if (!built) return false;
    for (const Image& image : kImages)
        if (std::string_view(image.xbe) == IMAGE_XBE && dispatch::Load(CodeDll(image))) return true;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kTitle,
                             "The game's code could not be loaded, see the log in the logs folder.", nullptr);
    return false;
}

}  // namespace app
