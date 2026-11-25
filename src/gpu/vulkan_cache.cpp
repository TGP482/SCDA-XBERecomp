#include "gpu/vulkan_cache.h"

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"

namespace gpu::vkcache {

namespace {

namespace fs = std::filesystem;

// shaders_vk.bin: FileHeader, then a Record and its SPIR-V per shader, appended by every process sharing the folder
constexpr uint32_t kShaderMagic = 0x56534353;  // 'SCSV'
constexpr uint32_t kShaderVersion = 1;         // bump when the HLSL adaptation or glslang options change
constexpr uint64_t kMaxShaderFile = 256ull << 20;
constexpr uint64_t kMaxCacheFile = 1ull << 30;
constexpr int64_t kSaveIdleMs = 10000;
constexpr uint32_t kKeysMagic = 0x4B564353;  // 'SCVK'
constexpr uint32_t kKeysVersion = 1;
constexpr uint64_t kMaxKeysFile = 64ull << 20;

struct FileHeader {
    uint32_t magic, version;
};
struct Record {
    uint64_t key, check;
    uint32_t size, sum;  // bytes; sum: low half of Hash over them
};
struct KeyRecord {
    uint64_t hash;
    uint32_t size;
    uint32_t unused;
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
    int file = -1;
};

struct Keys {
    std::mutex lock;
    std::unordered_set<uint64_t> known;
    std::vector<std::vector<uint8_t>> loaded;
    int file = -1;
};

struct Cache {
    VkDevice dev = VK_NULL_HANDLE;
    VkPipelineCache cache = VK_NULL_HANDLE;
    fs::path file;
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
};

Shaders& S() {
    static Shaders* s = new Shaders;
    return *s;
}
Keys& K() {
    static Keys* k = new Keys;
    return *k;
}
Cache& C() {
    static Cache* c = new Cache;
    return *c;
}
Pool& P() {
    static Pool* p = new Pool;
    return *p;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool ReadAll(int fd, std::vector<uint8_t>& out, uint64_t max) {
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0 || uint64_t(size) > max) return false;
    out.resize(size_t(size));
    size_t done = 0;
    while (done < out.size()) {
        ssize_t n = pread(fd, out.data() + done, out.size() - done, off_t(done));
        if (n <= 0) return false;
        done += size_t(n);
    }
    return true;
}

// one write at the end of the file, whole (O_APPEND), whatever another process appends meanwhile
void Append(int fd, const void* p, size_t n) { [[maybe_unused]] ssize_t w = write(fd, p, n); }

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
        if (r.size > s.data.size() - at || r.size % 4 || uint32_t(Hash(s.data.data() + at, r.size)) != r.sum) break;
        Shaders::Entry e{r.check, at, r.size};
        if (over) s.index[r.key] = e;
        else s.index.emplace(r.key, e);
        good = at + r.size;
    }
    return good;
}

void OpenShaders(const fs::path& path) {
    Shaders& s = S();
    s.file = open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (s.file < 0) {
        LOGW("shader cache: %s cannot be opened", path.c_str());
        return;
    }
    size_t good = 0;
    if (ReadAll(s.file, s.data, kMaxShaderFile)) good = Index(s, 0, true);
    if (good < s.data.size() || !good) {
        if (!s.data.empty()) LOGI("shader cache: %zu of %zu bytes kept", good, s.data.size());
        [[maybe_unused]] int r = ftruncate(s.file, off_t(good));
        if (!good) {
            s.index.clear();
            s.data.clear();
            FileHeader fh{kShaderMagic, kShaderVersion};
            Append(s.file, &fh, sizeof(fh));
        }
    }
    LOGI("shader cache: %zu SPIR-V shaders, %.1f MB", s.index.size(), good / 1048576.0);
}

void OpenBase(const fs::path& path) {
    Shaders& s = S();
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    std::vector<uint8_t> data;
    bool ok = ReadAll(fd, data, kMaxShaderFile);
    close(fd);
    if (!ok) return;
    size_t from = s.data.size(), before = s.index.size();
    s.data.insert(s.data.end(), data.begin(), data.end());
    Index(s, from, false);
    LOGI("shader cache: %zu more from %s", s.index.size() - before, path.filename().c_str());
}

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
        if (k.known.insert(r.hash).second) k.loaded.emplace_back(data.begin() + long(at), data.begin() + long(at + r.size));
        good = at + r.size;
    }
    return good;
}

void OpenKeys(const fs::path& path) {
    Keys& k = K();
    k.file = open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (k.file < 0) {
        LOGW("pipeline keys: %s cannot be opened", path.c_str());
        return;
    }
    std::vector<uint8_t> data;
    size_t good = 0;
    if (ReadAll(k.file, data, kMaxKeysFile)) good = IndexKeys(k, data);
    if (!good) {
        k.known.clear();
        k.loaded.clear();
        [[maybe_unused]] int r = ftruncate(k.file, 0);
        FileHeader fh{kKeysMagic, kKeysVersion};
        Append(k.file, &fh, sizeof(fh));
    } else if (good < data.size()) {
        [[maybe_unused]] int r = ftruncate(k.file, off_t(good));
    }
    LOGI("pipeline keys: %zu from %s", k.loaded.size(), path.filename().c_str());
}

void OpenCache(VkDevice dev, const fs::path& path) {
    Cache& c = C();
    c.dev = dev;
    c.file = path;
    std::vector<uint8_t> blob;
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (!ReadAll(fd, blob, kMaxCacheFile)) blob.clear();
        close(fd);
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.empty() ? nullptr : blob.data();
    // the driver checks the header (vendor, device, its cache UUID) and starts empty on another one
    if (vkCreatePipelineCache(dev, &ci, nullptr, &c.cache) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        if (vkCreatePipelineCache(dev, &ci, nullptr, &c.cache) != VK_SUCCESS) c.cache = VK_NULL_HANDLE;
    }
    LOGI("pipeline cache: %.1f MB from %s", blob.size() / 1048576.0, path.filename().c_str());
}

void Run() {
    pthread_setname_np(pthread_self(), "shader build");
    Pool& p = P();
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(p.lock);
            p.wake.wait(lock, [&] { return !p.jobs.empty(); });
            job = std::move(p.jobs.front());
            p.jobs.pop_front();
        }
        job();
    }
}

}  // namespace

void Open(VkDevice dev, uint64_t root) {
    char exe[4096] = {};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    std::string stem = fs::path(std::string(exe, n > 0 ? size_t(n) : 0)).stem().string();
    auto dir = app::ExeDir() / "shadercache";
    std::error_code ec;
    fs::create_directories(dir, ec);
    OpenShaders(dir / "shaders_vk.bin");
    OpenBase(dir / "base_shaders_vk.bin");
    char tag[17];
    std::snprintf(tag, sizeof(tag), "%08x", unsigned(root));
    // per executable and pipeline layout: offline and versus write theirs whole
    OpenCache(dev, dir / (stem + "_pipelines_vk_" + tag + ".bin"));
    OpenKeys(dir / (stem + "_pipeline_keys_vk.bin"));
}

std::vector<uint32_t> FindShader(uint64_t key, uint64_t check) {
    Shaders& s = S();
    std::lock_guard lock(s.lock);
    auto it = s.index.find(key);
    if (it == s.index.end() || it->second.check != check) return {};
    std::vector<uint32_t> out(it->second.size / 4);
    std::memcpy(out.data(), s.data.data() + it->second.offset, it->second.size);
    return out;
}

void AddShader(uint64_t key, uint64_t check, const std::vector<uint32_t>& spirv) {
    Shaders& s = S();
    if (s.file < 0 || spirv.empty()) return;
    const auto* bytes = reinterpret_cast<const uint8_t*>(spirv.data());
    uint32_t size = uint32_t(spirv.size() * 4);
    Record r{key, check, size, uint32_t(Hash(bytes, size))};
    std::vector<uint8_t> out(sizeof(r) + size);
    std::memcpy(out.data(), &r, sizeof(r));
    std::memcpy(out.data() + sizeof(r), bytes, size);
    std::lock_guard lock(s.lock);
    Append(s.file, out.data(), out.size());
}

VkPipelineCache Pipelines() { return C().cache; }

void PipelineMade() {
    C().unsaved++;
    C().last_store = NowMs();
}

void SaveWhenIdle() {
    Cache& c = C();
    if (!c.unsaved || NowMs() - c.last_store < kSaveIdleMs || c.saving.exchange(true)) return;
    Post([] {
        Save();
        C().saving = false;
    });
}

void Save() {
    Cache& c = C();
    std::lock_guard lock(c.save_lock);
    uint32_t n = c.unsaved.exchange(0);
    if (!c.cache || !n) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(c.dev, c.cache, &size, nullptr) != VK_SUCCESS || !size || size > kMaxCacheFile) return;
    std::vector<uint8_t> data(size);
    if (vkGetPipelineCacheData(c.dev, c.cache, &size, data.data()) != VK_SUCCESS) return;
    data.resize(size);
    fs::path tmp = c.file;
    tmp += "." + std::to_string(getpid()) + ".tmp";
    bool ok = false;
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
        ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
        ok = std::fclose(f) == 0 && ok;
    }
    std::error_code ec;
    if (ok) fs::rename(tmp, c.file, ec);
    if (!ok || ec) {
        fs::remove(tmp, ec);
        c.unsaved += n;
        c.last_store = NowMs();
        return;
    }
    LOGI("pipeline cache: %u new, %.1f MB saved", n, data.size() / 1048576.0);
}

std::vector<std::vector<uint8_t>> TakePipelineKeys() {
    Keys& k = K();
    std::lock_guard lock(k.lock);
    return std::move(k.loaded);
}

void AddPipelineKey(const void* key, size_t size) {
    Keys& k = K();
    if (k.file < 0) return;
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
        if (ahead) p.jobs.push_front(std::move(job));
        else p.jobs.push_back(std::move(job));
        if (!p.started) {
            p.started = true;
            unsigned cores = std::thread::hardware_concurrency();
            int n = std::clamp(int(cores) - 3, 1, 4);
            for (int i = 0; i < n; i++) std::thread(Run).detach();
            LOGI("shader build threads: %d", n);
        }
    }
    p.wake.notify_one();
}

}  // namespace gpu::vkcache

namespace gpu {
void SavePipelineCache() { vkcache::Save(); }
}  // namespace gpu
