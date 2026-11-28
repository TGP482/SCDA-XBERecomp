#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dcompiler.h>

#include "gpu/pipeline_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"

using Microsoft::WRL::ComPtr;

namespace gpu::cache {

namespace {

// shaders.bin: FileHeader, then a Record and its bytecode for each shader as it is compiled, by every process that
// shares the folder; a torn tail from a killed process is cut off at the next start
constexpr uint32_t kShaderMagic = 0x48534353;  // 'SCSH'
constexpr uint32_t kShaderVersion = 1;         // bump when the compile flags change
constexpr uint64_t kMaxShaderFile = 256ull << 20;
constexpr uint64_t kMaxLibraryFile = 1ull << 30;
constexpr int64_t kSaveIdleMs = 10000;
// <exe>_pipeline_keys.bin: FileHeader, then a KeyRecord and its bytes for each pipeline the first time it is drawn
constexpr uint32_t kKeysMagic = 0x4B504353;  // 'SCPK'
constexpr uint32_t kKeysVersion = 1;
constexpr uint64_t kMaxKeysFile = 64ull << 20;

struct FileHeader {
    uint32_t magic, version;
};
struct Record {
    uint64_t key, check;
    uint32_t size, sum;  // sum: low half of Hash over the bytecode
};

struct Shaders {
    std::mutex lock;
    std::vector<uint8_t> data;
    struct Entry {
        uint64_t check;
        size_t offset;
        uint32_t size;
    };
    std::unordered_map<uint64_t, Entry> index;
    HANDLE file = INVALID_HANDLE_VALUE;
};

struct KeyRecord {
    uint64_t hash;  // Hash over the key's bytes
    uint32_t size;
    uint32_t unused;
};

struct Keys {
    std::mutex lock;
    std::unordered_set<uint64_t> known;
    std::vector<std::vector<uint8_t>> loaded;
    HANDLE file = INVALID_HANDLE_VALUE;
};

struct Library {
    ComPtr<ID3D12PipelineLibrary> lib;
    std::vector<uint8_t> blob;  // the library reads from it for its whole life
    std::filesystem::path file;
    uint64_t root = 0;
    std::mutex load_lock;  // two threads loading the same pipeline at once is the one unsafe use
    std::mutex save_lock;
    std::atomic<uint32_t> unsaved{0};
    std::atomic<int64_t> last_store{0};
    std::atomic<bool> saving{false};
};

struct Pool {
    std::mutex lock;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    bool started = false;
    uint32_t running = 0;
    std::condition_variable idle;  // running fell to 0
};

// never freed: build threads may still run when the process exits
Shaders& S() {
    static Shaders* s = new Shaders;
    return *s;
}
Library& L() {
    static Library* l = new Library;
    return *l;
}
Pool& P() {
    static Pool* p = new Pool;
    return *p;
}
Keys& K() {
    static Keys* k = new Keys;
    return *k;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool ReadAll(HANDLE h, std::vector<uint8_t>& out) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || uint64_t(size.QuadPart) > kMaxLibraryFile) return false;
    out.resize(size_t(size.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        DWORD n = 0, want = DWORD(std::min<size_t>(out.size() - done, 1u << 30));
        if (!ReadFile(h, out.data() + done, want, &n, nullptr) || !n) return false;
        done += n;
    }
    return true;
}

void Truncate(HANDLE h, uint64_t size) {
    LARGE_INTEGER at;
    at.QuadPart = LONGLONG(size);
    SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
}

// one write at the end of the file, whole, whatever another process appends meanwhile
void Append(HANDLE h, const void* p, size_t n) {
    OVERLAPPED at{};
    at.Offset = at.OffsetHigh = 0xFFFFFFFF;
    DWORD done = 0;
    WriteFile(h, p, DWORD(n), &done, &at);
}

// the records of a file read into s.data at from; where the good ones end, 0 without a valid header. over: a record
// replaces one with its key already indexed
size_t Index(Shaders& s, size_t from, bool over) {
    if (s.data.size() - from < sizeof(FileHeader)) return 0;
    FileHeader fh;
    std::memcpy(&fh, s.data.data() + from, sizeof(fh));
    if (fh.magic != kShaderMagic || fh.version != kShaderVersion) return 0;
    size_t good = from + sizeof(fh);
    while (good + sizeof(Record) <= s.data.size()) {
        Record r;
        std::memcpy(&r, s.data.data() + good, sizeof(r));
        size_t at = good + sizeof(r);
        if (r.size > s.data.size() - at || uint32_t(Hash(s.data.data() + at, r.size)) != r.sum) break;
        Shaders::Entry e{r.check, at, r.size};
        if (over) s.index[r.key] = e;
        else s.index.emplace(r.key, e);
        good = at + r.size;
    }
    return good;
}

void OpenShaders(const std::filesystem::path& path) {
    Shaders& s = S();
    s.file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s.file == INVALID_HANDLE_VALUE) {
        LOGW("shader cache: %s cannot be opened (%lu)", path.string().c_str(), GetLastError());
        return;
    }
    size_t good = 0;
    if (ReadAll(s.file, s.data) && s.data.size() <= kMaxShaderFile) good = Index(s, 0, true);
    if (good < s.data.size() || !good) {
        if (!s.data.empty()) LOGI("shader cache: %zu of %zu bytes kept", good, s.data.size());
        Truncate(s.file, good);
        if (!good) {
            s.index.clear();
            s.data.clear();
            FileHeader fh{kShaderMagic, kShaderVersion};
            Append(s.file, &fh, sizeof(fh));
        }
    }
    LOGI("shader cache: %zu shaders, %.1f MB", s.index.size(), good / 1048576.0);
}

// the shaders a release ships, read only and after the player's own: a new player skips the compiler for them
void OpenBase(const std::filesystem::path& path) {
    Shaders& s = S();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<uint8_t> data;
    bool ok = ReadAll(h, data) && data.size() <= kMaxShaderFile;
    CloseHandle(h);
    if (!ok) return;
    size_t from = s.data.size(), before = s.index.size();
    s.data.insert(s.data.end(), data.begin(), data.end());
    Index(s, from, false);
    LOGI("shader cache: %zu more from %s", s.index.size() - before, path.filename().string().c_str());
}

// the records of data kept in k; where the good ones end, 0 without a valid header
size_t IndexKeys(Keys& k, const std::vector<uint8_t>& data) {
    if (data.size() < sizeof(FileHeader)) return 0;
    FileHeader fh;
    std::memcpy(&fh, data.data(), sizeof(fh));
    if (fh.magic != kKeysMagic || fh.version != kKeysVersion) return 0;
    size_t good = sizeof(fh);
    while (good + sizeof(KeyRecord) <= data.size()) {
        KeyRecord r;
        std::memcpy(&r, data.data() + good, sizeof(r));
        size_t at = good + sizeof(r);
        if (r.size > data.size() - at || Hash(data.data() + at, r.size) != r.hash) break;
        if (k.known.insert(r.hash).second) k.loaded.emplace_back(data.begin() + at, data.begin() + at + r.size);
        good = at + r.size;
    }
    return good;
}

void OpenKeys(const std::filesystem::path& path) {
    Keys& k = K();
    k.file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (k.file == INVALID_HANDLE_VALUE) {
        LOGW("pipeline keys: %s cannot be opened (%lu)", path.string().c_str(), GetLastError());
        return;
    }
    std::vector<uint8_t> data;
    size_t good = 0;
    if (ReadAll(k.file, data) && data.size() <= kMaxKeysFile) good = IndexKeys(k, data);
    if (!good) {
        k.known.clear();
        k.loaded.clear();
        Truncate(k.file, 0);
        FileHeader fh{kKeysMagic, kKeysVersion};
        Append(k.file, &fh, sizeof(fh));
    } else if (good < data.size()) {
        Truncate(k.file, good);
    }
    LOGI("pipeline keys: %zu from %s", k.loaded.size(), path.filename().string().c_str());
}

void OpenLibrary(ID3D12Device* dev, const std::filesystem::path& path) {
    Library& l = L();
    ComPtr<ID3D12Device1> dev1;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dev1)))) {
        LOGW("pipeline cache: no ID3D12Device1");
        return;
    }
    l.file = path;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        if (!ReadAll(h, l.blob)) l.blob.clear();
        CloseHandle(h);
    }
    HRESULT hr = E_FAIL;
    if (!l.blob.empty()) hr = dev1->CreatePipelineLibrary(l.blob.data(), l.blob.size(), IID_PPV_ARGS(&l.lib));
    if (FAILED(hr)) {
        // another driver or adapter
        if (!l.blob.empty()) LOGI("pipeline cache: %s discarded (%08lX)", path.filename().string().c_str(), (unsigned long)hr);
        l.blob.clear();
        l.blob.shrink_to_fit();
        hr = dev1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&l.lib));
    }
    if (FAILED(hr)) {
        l.lib.Reset();
        LOGW("pipeline cache: unavailable (%08lX)", (unsigned long)hr);
        return;
    }
    LOGI("pipeline cache: %.1f MB from %s", l.blob.size() / 1048576.0, path.filename().string().c_str());
}

std::wstring Name(uint64_t name) {
    wchar_t text[17];
    std::swprintf(text, 17, L"%016llx", (unsigned long long)Hash(&name, sizeof(name), L().root));
    return text;
}

void Run() {
    using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (auto set = reinterpret_cast<SetDescription>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))))
        set(GetCurrentThread(), L"shader build");
    Pool& p = P();
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(p.lock);
            p.wake.wait(lock, [&] { return !p.jobs.empty(); });
            job = std::move(p.jobs.front());
            p.jobs.pop_front();
            p.running++;
        }
        job();
        std::lock_guard lock(p.lock);
        if (--p.running == 0) p.idle.notify_all();
    }
}

}  // namespace

void OpenShaderFiles() {
    // a renderer swapped in during the run finds them open
    if (S().file != INVALID_HANDLE_VALUE) return;
    auto dir = app::ExeDir() / "shadercache";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    OpenShaders(dir / "shaders.bin");
    OpenBase(dir / "base_shaders.bin");
}

void Open(ID3D12Device* dev, uint64_t root) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    auto dir = app::ExeDir() / "shadercache";
    L().root = root;
    OpenShaderFiles();
    // per executable: offline and versus write theirs whole
    std::wstring stem = std::filesystem::path(exe).stem().wstring();
    OpenLibrary(dev, dir / (stem + L"_pipelines.bin"));
    if (K().file == INVALID_HANDLE_VALUE) OpenKeys(dir / (stem + L"_pipeline_keys.bin"));
}

ComPtr<ID3DBlob> FindShader(uint64_t key, uint64_t check) {
    Shaders& s = S();
    std::lock_guard lock(s.lock);
    auto it = s.index.find(key);
    if (it == s.index.end() || it->second.check != check) return nullptr;
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3DCreateBlob(it->second.size, &blob))) return nullptr;
    std::memcpy(blob->GetBufferPointer(), s.data.data() + it->second.offset, it->second.size);
    return blob;
}

void AddShader(uint64_t key, uint64_t check, ID3DBlob* code) {
    Shaders& s = S();
    if (s.file == INVALID_HANDLE_VALUE || !code) return;
    const auto* bytes = static_cast<const uint8_t*>(code->GetBufferPointer());
    Record r{key, check, uint32_t(code->GetBufferSize()), uint32_t(Hash(bytes, code->GetBufferSize()))};
    std::vector<uint8_t> out(sizeof(r) + r.size);
    std::memcpy(out.data(), &r, sizeof(r));
    std::memcpy(out.data() + sizeof(r), bytes, r.size);
    std::lock_guard lock(s.lock);
    Append(s.file, out.data(), out.size());
}

ComPtr<ID3D12PipelineState> LoadPipeline(uint64_t name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
    Library& l = L();
    if (!l.lib) return nullptr;
    ComPtr<ID3D12PipelineState> pso;
    std::lock_guard lock(l.load_lock);
    if (FAILED(l.lib->LoadGraphicsPipeline(Name(name).c_str(), &d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso;
}

void StorePipeline(uint64_t name, ID3D12PipelineState* pso) {
    Library& l = L();
    if (!l.lib || !pso || FAILED(l.lib->StorePipeline(Name(name).c_str(), pso))) return;
    l.unsaved++;
    l.last_store = NowMs();
}

void SaveWhenIdle() {
    Library& l = L();
    if (!l.unsaved || NowMs() - l.last_store < kSaveIdleMs || l.saving.exchange(true)) return;
    Post([] {
        SavePipelineCache();
        L().saving = false;
    });
}

std::vector<std::vector<uint8_t>> TakePipelineKeys() {
    Keys& k = K();
    std::lock_guard lock(k.lock);
    return std::move(k.loaded);
}

void AddPipelineKey(const void* key, size_t size) {
    Keys& k = K();
    if (k.file == INVALID_HANDLE_VALUE) return;
    KeyRecord r{Hash(key, size), uint32_t(size), 0};
    std::lock_guard lock(k.lock);
    if (!k.known.insert(r.hash).second) return;
    std::vector<uint8_t> out(sizeof(r) + size);
    std::memcpy(out.data(), &r, sizeof(r));
    std::memcpy(out.data() + sizeof(r), key, size);
    Append(k.file, out.data(), out.size());
}

void Post(std::function<void()> job, bool ahead) {
    Pool& p = P();
    {
        std::lock_guard lock(p.lock);
        if (ahead) {
            p.jobs.push_front(std::move(job));
        } else {
            p.jobs.push_back(std::move(job));
        }
        if (!p.started) {
            p.started = true;
            // the title's thread, the render thread and audio keep theirs
            unsigned cores = std::thread::hardware_concurrency();
            int n = std::clamp(int(cores) - 3, 1, 4);
            for (int i = 0; i < n; i++) std::thread(Run).detach();
            LOGI("shader build threads: %d", n);
        }
    }
    p.wake.notify_one();
}

void DropJobs() {
    Pool& p = P();
    std::unique_lock lock(p.lock);
    p.jobs.clear();
    p.idle.wait(lock, [&] { return p.running == 0; });
    // a dropped save would leave it set
    L().saving = false;
}

void CloseLibrary() {
    SavePipelineCache();
    Library& l = L();
    std::lock_guard lock(l.save_lock);
    l.lib.Reset();
    l.blob.clear();
    l.unsaved = 0;
}

}  // namespace gpu::cache

namespace gpu {

void SavePipelineCache() {
    cache::Library& l = cache::L();
    std::lock_guard lock(l.save_lock);
    uint32_t n = l.unsaved.exchange(0);
    if (!l.lib || !n) return;
    std::vector<uint8_t> data(l.lib->GetSerializedSize());
    if (data.empty() || data.size() > cache::kMaxLibraryFile || FAILED(l.lib->Serialize(data.data(), data.size()))) {
        LOG_FIRST(1, "pipeline cache: not saved, %zu bytes", data.size());
        return;
    }
    std::filesystem::path tmp = l.file;
    tmp += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    bool ok = false;
    if (FILE* f = _wfopen(tmp.c_str(), L"wb")) {
        ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
        ok = std::fclose(f) == 0 && ok;
    }
    // a process reading the file at its start keeps it: this goes again after the next new pipeline
    if (!ok || !MoveFileExW(tmp.c_str(), l.file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        l.unsaved += n;
        l.last_store = cache::NowMs();
        return;
    }
    LOGI("pipeline cache: %u new, %.1f MB saved", n, data.size() / 1048576.0);
}

}  // namespace gpu
