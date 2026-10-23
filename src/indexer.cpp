#include "indexer.h"

#include <windows.h>
#include <shlobj.h>
#include <psapi.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unordered_map>
#include <unordered_set>

namespace qb {

namespace {

constexpr uint32_t kCacheVersion = 3;
constexpr char kMagic[4] = {'Q', 'B', 'F', '3'};
constexpr char kEnd[4] = {'E', 'N', 'D', '!'};
constexpr int64_t kCacheMaxAgeSec = 30LL * 24 * 3600;
constexpr size_t kMaxPathChars = 32000;
constexpr size_t kStreamBufferSize = 64 * 1024; // 64 KB streaming buffer

int64_t fileTimeToUnix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (u.QuadPart < 116444736000000000ULL) return 0;
    return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

std::wstring pathKey(std::wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    for (auto& c : p) {
        if (c == L'/') c = L'\\';
    }
    return fold(p);
}

bool sameOrUnder(const std::wstring& key, const std::wstring& root) {
    if (key.empty() || root.empty()) return false;
    if (key == root) return true;
    if (root.back() == L'\\') return key.compare(0, root.size(), root) == 0;
    return key.size() > root.size() && key[root.size()] == L'\\' && key.compare(0, root.size(), root) == 0;
}

bool underAny(const std::wstring& key, const std::vector<std::wstring>& roots) {
    for (auto& r : roots)
        if (sameOrUnder(key, r)) return true;
    return false;
}

bool isDirectory(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring joinPath(const std::wstring& dir, const wchar_t* name) {
    std::wstring r = dir;
    if (!r.empty() && r.back() != L'\\') r += L'\\';
    r += name;
    return r;
}

std::wstring winPath(const std::wstring& p) {
    if (p.size() < 240 || p.compare(0, 4, L"\\\\?\\") == 0) return p;
    if (p.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

std::wstring knownFolder(const KNOWNFOLDERID& id) {
    PWSTR out = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &out)) && out) r = out;
    CoTaskMemFree(out);
    return r;
}

std::vector<std::wstring> commonUserFolders() {
    std::vector<std::wstring> v;
    const KNOWNFOLDERID* ids[] = {&FOLDERID_Desktop, &FOLDERID_Downloads, &FOLDERID_Documents,
                                  &FOLDERID_Pictures, &FOLDERID_Music,     &FOLDERID_Videos,
                                  &FOLDERID_StartMenu, &FOLDERID_CommonStartMenu};
    for (auto* id : ids) {
        std::wstring p = knownFolder(*id);
        if (!p.empty()) v.push_back(p);
    }
    return v;
}

std::vector<std::wstring> uniqueExistingDirs(const std::vector<std::wstring>& in) {
    std::vector<std::wstring> out;
    std::unordered_set<std::wstring> seen;
    for (auto& p : in) {
        if (p.empty() || !isDirectory(p)) continue;
        if (seen.insert(pathKey(p)).second) out.push_back(p);
    }
    return out;
}

bool driveReady(const std::wstring& root) {
    ULARGE_INTEGER a;
    UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);
    BOOL ok = GetDiskFreeSpaceExW(root.c_str(), &a, nullptr, nullptr);
    SetErrorMode(oldMode);
    return ok != 0;
}

std::vector<std::wstring> logicalDriveRoots() {
    std::vector<std::wstring> roots;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        std::wstring root = {static_cast<wchar_t>(L'A' + i), L':', L'\\'};
        UINT t = GetDriveTypeW(root.c_str());
        if ((t == DRIVE_FIXED || t == DRIVE_REMOVABLE || t == DRIVE_REMOTE || t == DRIVE_RAMDISK) && driveReady(root))
            roots.push_back(root);
    }
    return roots;
}

// Streaming Binary Writer (64 KB chunking, UTF-8 paths)
class StreamWriter {
public:
    explicit StreamWriter(FILE* f) : f_(f) {
        buf_.resize(kStreamBufferSize);
    }

    ~StreamWriter() { flush(); }

    bool ok() const { return ok_; }

    void writeBytes(const void* src, size_t n) {
        if (!ok_) return;
        const char* p = static_cast<const char*>(src);
        while (n > 0) {
            size_t avail = kStreamBufferSize - pos_;
            size_t take = (n < avail) ? n : avail;
            std::memcpy(buf_.data() + pos_, p, take);
            pos_ += take;
            p += take;
            n -= take;
            if (pos_ == kStreamBufferSize) flush();
        }
    }

    template <class T>
    void writeVal(T val) {
        writeBytes(&val, sizeof(T));
    }

    void flush() {
        if (pos_ > 0 && ok_) {
            if (std::fwrite(buf_.data(), 1, pos_, f_) != pos_) {
                ok_ = false;
            }
            pos_ = 0;
        }
    }

private:
    FILE* f_;
    std::vector<char> buf_;
    size_t pos_ = 0;
    bool ok_ = true;
};

// Streaming Binary Reader (64 KB chunking, UTF-8 paths)
class StreamReader {
public:
    explicit StreamReader(FILE* f) : f_(f) {
        buf_.resize(kStreamBufferSize);
    }

    bool ok() const { return ok_; }

    bool readBytes(void* dst, size_t n) {
        if (!ok_) return false;
        char* p = static_cast<char*>(dst);
        while (n > 0) {
            if (pos_ >= available_) {
                available_ = std::fread(buf_.data(), 1, kStreamBufferSize, f_);
                pos_ = 0;
                if (available_ == 0) {
                    ok_ = false;
                    return false;
                }
            }
            size_t take = (n < (available_ - pos_)) ? n : (available_ - pos_);
            std::memcpy(p, buf_.data() + pos_, take);
            pos_ += take;
            p += take;
            n -= take;
        }
        return true;
    }

    template <class T>
    T readVal() {
        T val{};
        readBytes(&val, sizeof(T));
        return val;
    }

private:
    FILE* f_;
    std::vector<char> buf_;
    size_t pos_ = 0;
    size_t available_ = 0;
    bool ok_ = true;
};

}  // namespace

Indexer::Indexer() { initPriority(); }

Indexer::~Indexer() { stop(); }

void Indexer::setExcludes(std::vector<std::wstring> folded) {
    std::lock_guard<std::mutex> lk(ctlMu_);
    excludes_ = std::move(folded);
}

bool Indexer::excluded(const std::wstring& name) const {
    if (excludes_.empty()) return false;
    std::wstring f = fold(name);
    for (auto& pat : excludes_) {
        if (f == pat) return true;
        if (pat.find_first_of(L"*?") != std::wstring::npos && wildMatch(pat, f)) return true;
    }
    return false;
}

void Indexer::initPriority() {
    priorityRoots_.clear();
    userRoots_.clear();
    noisyRoots_.clear();
    for (auto& p : uniqueExistingDirs(commonUserFolders())) priorityRoots_.push_back(pathKey(p));
    std::wstring home = getEnv(L"USERPROFILE");
    if (!home.empty()) {
        userRoots_.push_back(pathKey(home));
        noisyRoots_.push_back(pathKey(home + L"\\AppData"));
    }
    for (const wchar_t* env : {L"SystemRoot", L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432", L"ProgramData", L"TEMP", L"TMP"}) {
        std::wstring v = getEnv(env);
        if (!v.empty()) noisyRoots_.push_back(pathKey(v));
    }
}

int Indexer::priorityOf(const std::wstring& key) const {
    if (key.empty()) return 3;
    if (underAny(key, priorityRoots_)) return 0;
    static const wchar_t* noisy[] = {L"\\windows\\", L"\\program files\\", L"\\program files (x86)\\", L"\\programdata\\",
                                     L"\\appdata\\", L"\\node_modules\\", L"\\.git\\", L"\\system volume information\\",
                                     L"\\$recycle.bin\\"};
    bool noisySeg = false;
    for (auto* s : noisy)
        if (key.find(s) != std::wstring::npos) { noisySeg = true; break; }
    if (noisySeg || underAny(key, noisyRoots_)) return 5;
    if (underAny(key, userRoots_)) return 1;
    return 3;
}

Entry Indexer::build(const std::wstring& path, int64_t size, int64_t mtime, bool isDir) const {
    Entry e = makeEntry(path, size, mtime, isDir, 3);
    e.priority = static_cast<uint8_t>(priorityOf(e.lower()));
    return e;
}

Entry Indexer::build(uint32_t dirId, const wchar_t* fileName, int64_t size, int64_t mtime, bool isDir, uint8_t dirPrio) const {
    Entry e = makeEntry(dirId, fileName, size, mtime, isDir, dirPrio);
    return e;
}

// ---------------------------------------------------------------- control

void Indexer::init() {
    {
        std::lock_guard<std::mutex> lk(ctlMu_);
        if (scanThread_.joinable()) return;
        if (loadCache()) {
            quickRoots_ = uniqueExistingDirs(commonUserFolders());
            startWatchers();
            if (onChanged) onChanged();
            return;
        }
    }
    rescan();
}

void Indexer::rescan() {
    stop();
    std::lock_guard<std::mutex> lk(ctlMu_);
    uint64_t id = ++scanId_;
    {
        std::unique_lock<std::shared_mutex> w(mu_);
        entries_.clear();
        entries_.shrink_to_fit();
    }
    count_ = 0;
    scanning_ = true;
    scanThread_ = std::thread([this, id] {
        try {
            scanMain(id);
        } catch (const std::exception& ex) {
            logf("scan failed: %s", ex.what());
        } catch (...) {
            logf("scan failed: unknown exception");
        }
        if (scanId_.load() == id) scanning_ = false;
        if (onChanged) onChanged();
    });
}

void Indexer::stop() {
    std::thread t;
    {
        std::lock_guard<std::mutex> lk(ctlMu_);
        ++scanId_;  // cancels scan workers
        t = std::move(scanThread_);
        stopWatchers();
    }
    qCv_.notify_all();
    if (t.joinable()) t.join();
    scanning_ = false;
}

// ---------------------------------------------------------------- scanning

void Indexer::pushJob(std::wstring dir) {
    {
        std::lock_guard<std::mutex> lk(qMu_);
        queue_.push_back(std::move(dir));
        ++pending_;
    }
    qCv_.notify_one();
}

void Indexer::flush(uint64_t id, std::vector<Entry>& batch) {
    if (batch.empty()) return;
    if (scanId_.load() == id) {
        {
            std::unique_lock<std::shared_mutex> w(mu_);
            entries_.insert(entries_.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
        }
        count_ += static_cast<int64_t>(batch.size());
        if (onChanged) onChanged();
    }
    batch.clear();
}

void Indexer::walkDir(uint64_t id, const std::wstring& dir, std::vector<Entry>& batch) {
    uint32_t dirId = DirPool::global().getOrAdd(dir);
    uint8_t dirPrio = static_cast<uint8_t>(priorityOf(std::wstring(DirPool::global().getLower(dirId))));
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(joinPath(winPath(dir), L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (scanId_.load() != id) break;
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        if (excluded(n)) continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        int64_t size = isDir ? 0 : ((static_cast<int64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow);
        batch.push_back(build(dirId, n, size, fileTimeToUnix(fd.ftLastWriteTime), isDir, dirPrio));
        if (batch.size() >= 1024) flush(id, batch);
        if (isDir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            std::wstring full = joinPath(dir, n);
            std::wstring key = pathKey(full);
            bool dup = false;
            for (auto& q : quickRoots_)
                if (pathKey(q) == key) { dup = true; break; }
            if (!dup) pushJob(std::move(full));
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void Indexer::worker(uint64_t id) {
    std::vector<Entry> batch;
    batch.reserve(1024);
    for (;;) {
        std::wstring dir;
        {
            std::unique_lock<std::mutex> lk(qMu_);
            if (queue_.empty() && pending_ > 0 && !batch.empty()) {
                lk.unlock();
                flush(id, batch);
                lk.lock();
            }
            qCv_.wait(lk, [&] { return !queue_.empty() || pending_ == 0 || scanId_.load() != id; });
            if (scanId_.load() != id || queue_.empty()) break;
            dir = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            if (scanId_.load() == id) walkDir(id, dir, batch);
        } catch (const std::exception& ex) {
            logf("walkDir failed: %s", ex.what());
        } catch (...) {
        }
        {
            std::lock_guard<std::mutex> lk(qMu_);
            if (--pending_ == 0) qCv_.notify_all();
        }
    }
    flush(id, batch);
}

void Indexer::scanMain(uint64_t id) {
    std::vector<std::wstring> roots = logicalDriveRoots();
    quickRoots_ = uniqueExistingDirs(commonUserFolders());
    {
        std::lock_guard<std::mutex> lk(qMu_);
        queue_.clear();
        pending_ = 0;
    }
    if (roots.empty()) return;

    for (auto& q : quickRoots_) pushJob(q);
    for (auto& r : roots) pushJob(r);

    unsigned n = std::thread::hardware_concurrency();
    n = std::min(std::max(n * 2, 2u), 16u);
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < n; ++i) pool.emplace_back([this, id] { worker(id); });
    for (auto& t : pool) t.join();

    if (scanId_.load() == id) {
        scanning_ = false;
        {
            std::unique_lock<std::shared_mutex> w(mu_);
            entries_.shrink_to_fit();
        }
        DirPool::global().finishScan();
        EmptyWorkingSet(GetCurrentProcess());
        saveCache();
        std::lock_guard<std::mutex> lk(ctlMu_);
        if (scanId_.load() == id) startWatchers();
        logf("scan %llu complete: %lld entries", (unsigned long long)id, (long long)count_.load());
    }
}

// ---------------------------------------------------------------- cache (streaming, compact UTF-8)

void Indexer::saveCache() {
    std::wstring dir = appDataDir();
    if (dir.empty()) return;
    std::wstring path = dir + L"\\index.cache", tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return;

    bool ok = true;
    {
        std::shared_lock<std::shared_mutex> lk(mu_);
        if (entries_.empty()) {
            std::fclose(f);
            DeleteFileW(tmp.c_str());
            return;
        }

        StreamWriter writer(f);
        writer.writeBytes(kMagic, 4);
        writer.writeVal<uint32_t>(kCacheVersion);
        writer.writeVal<int64_t>(static_cast<int64_t>(std::time(nullptr)));
        writer.writeVal<uint64_t>(entries_.size());

        for (const Entry& e : entries_) {
            std::string u8Path = toUtf8(e.path());
            uint32_t u8Len = static_cast<uint32_t>(u8Path.size());
            writer.writeVal<uint32_t>(u8Len);
            writer.writeBytes(u8Path.data(), u8Len);
            writer.writeVal<int64_t>(e.size);
            writer.writeVal<int64_t>(e.mtime);
            writer.writeVal<uint8_t>(e.isDir ? 1 : 0);
            if (!writer.ok()) {
                ok = false;
                break;
            }
        }
        writer.writeBytes(kEnd, 4);
        writer.flush();
        ok = ok && writer.ok();
    }

    ok = (std::fclose(f) == 0) && ok;
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
    }
}

bool Indexer::loadCache() {
    std::wstring dir = appDataDir();
    if (dir.empty()) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir + L"\\index.cache").c_str(), L"rb") != 0 || !f) return false;

    StreamReader reader(f);
    char magic[4];
    if (!reader.readBytes(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) {
        std::fclose(f);
        return false;
    }
    uint32_t ver = reader.readVal<uint32_t>();
    if (ver != kCacheVersion) {
        std::fclose(f);
        return false;
    }
    int64_t saved = reader.readVal<int64_t>();
    uint64_t count = reader.readVal<uint64_t>();
    if (!reader.ok() || std::time(nullptr) - saved > kCacheMaxAgeSec || count == 0 || count > 500000000ULL) {
        std::fclose(f);
        return false;
    }

    std::vector<Entry> entries;
    entries.reserve(static_cast<size_t>(count));
    std::string u8Buf;

    for (uint64_t i = 0; i < count && reader.ok(); ++i) {
        uint32_t len = reader.readVal<uint32_t>();
        if (!reader.ok() || len == 0 || len > kMaxPathChars * 4) {
            std::fclose(f);
            return false;
        }
        u8Buf.resize(len);
        if (!reader.readBytes(u8Buf.data(), len)) {
            std::fclose(f);
            return false;
        }
        std::wstring path = fromUtf8(u8Buf);
        int64_t size = reader.readVal<int64_t>();
        int64_t mtime = reader.readVal<int64_t>();
        uint8_t isDir = reader.readVal<uint8_t>();
        if (!reader.ok()) {
            std::fclose(f);
            return false;
        }
        entries.push_back(build(path, size, mtime, isDir != 0));
    }

    char endMarker[4];
    if (!reader.readBytes(endMarker, 4) || std::memcmp(endMarker, kEnd, 4) != 0) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);

    if (!excludes_.empty()) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& e) {
            if (excluded(e.name)) return true;
            for (const auto& seg : split(e.path(), L'\\')) {
                if (excluded(seg)) return true;
            }
            return false;
        }), entries.end());
    }
    {
        std::unique_lock<std::shared_mutex> w(mu_);
        entries_ = std::move(entries);
        entries_.shrink_to_fit();
        count_ = static_cast<int64_t>(entries_.size());
    }
    DirPool::global().finishScan();
    EmptyWorkingSet(GetCurrentProcess());
    logf("index cache loaded: %lld entries", (long long)count_.load());
    return true;
}

// ---------------------------------------------------------------- live watcher

void Indexer::startWatchers() {
    stopWatchers();
    uint64_t gen = ++watchGen_;
    std::lock_guard<std::mutex> lk(wMu_);
    for (auto& root : quickRoots_) {
        HANDLE h = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        watchHandles_.push_back(h);
        watchThreads_.emplace_back([this, gen, root, h] { watchLoop(gen, root, h); });
    }
    if (!watchHandles_.empty()) logf("live index watching %zu folders", watchHandles_.size());
}

void Indexer::stopWatchers() {
    ++watchGen_;
    std::vector<void*> handles;
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(wMu_);
        handles.swap(watchHandles_);
        threads.swap(watchThreads_);
    }
    for (void* h : handles) CancelIoEx(static_cast<HANDLE>(h), nullptr);
    for (auto& t : threads)
        if (t.joinable()) t.join();
    for (void* h : handles) CloseHandle(static_cast<HANDLE>(h));
}

void Indexer::watchLoop(uint64_t gen, std::wstring root, void* handle) {
    std::vector<DWORD> buffer(16 * 1024);
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                         FILE_NOTIFY_CHANGE_LAST_WRITE;
    while (watchGen_.load() == gen) {
        DWORD returned = 0;
        if (!ReadDirectoryChangesW(static_cast<HANDLE>(handle), buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(DWORD)),
                                   TRUE, filter, &returned, nullptr, nullptr))
            return;
        if (returned == 0 || watchGen_.load() != gen) continue;

        std::vector<std::wstring> removed;
        std::vector<Entry> added;
        const char* base = reinterpret_cast<const char*>(buffer.data());
        size_t off = 0;
        while (off + sizeof(FILE_NOTIFY_INFORMATION) <= returned) {
            auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + off);
            size_t nameBytes = fni->FileNameLength;
            if (offsetof(FILE_NOTIFY_INFORMATION, FileName) + nameBytes > returned - off) break;
            std::wstring rel(fni->FileName, nameBytes / sizeof(wchar_t));
            std::wstring full = joinPath(root, rel.c_str());
            bool skip = false;
            if (!excludes_.empty()) {
                for (auto& seg : split(rel, L'\\'))
                    if (excluded(seg)) { skip = true; break; }
            }
            if (!skip) {
                switch (fni->Action) {
                    case FILE_ACTION_ADDED:
                    case FILE_ACTION_MODIFIED:
                    case FILE_ACTION_RENAMED_NEW_NAME: {
                        WIN32_FILE_ATTRIBUTE_DATA fad;
                        if (GetFileAttributesExW(winPath(full).c_str(), GetFileExInfoStandard, &fad)) {
                            bool isDir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                            int64_t size = isDir ? 0 : ((static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow);
                            added.push_back(build(full, size, fileTimeToUnix(fad.ftLastWriteTime), isDir));
                        }
                        break;
                    }
                    case FILE_ACTION_REMOVED:
                    case FILE_ACTION_RENAMED_OLD_NAME:
                        removed.push_back(fold(full));
                        break;
                }
            }
            if (fni->NextEntryOffset == 0) break;
            off += fni->NextEntryOffset;
        }
        if (!removed.empty() || !added.empty()) applyChanges(gen, removed, added);

        for (int i = 0; i < 5 && watchGen_.load() == gen; ++i) Sleep(50);
    }
}

// In-place zero-reallocation updates:
// 1. Erase removed files and directory trees (matching prefix) in-place with std::remove_if
// 2. Replace modified items in-place
// 3. Append new items
void Indexer::applyChanges(uint64_t gen, std::vector<std::wstring>& removed, std::vector<Entry>& added) {
    {
        std::unique_lock<std::shared_mutex> w(mu_);
        if (watchGen_.load() != gen) return;

        std::unordered_set<std::wstring> removedSet(removed.begin(), removed.end());
        std::vector<std::wstring> prefixes;
        for (const auto& r : removed) {
            prefixes.push_back(r + L"\\");
        }

        // 1. In-place deletion of removed files and folders (including sub-items)
        if (!removedSet.empty()) {
            entries_.erase(
                std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) {
                    std::wstring eLower = e.lower();
                    if (removedSet.count(eLower)) return true;
                    for (const auto& p : prefixes) {
                        if (eLower.compare(0, p.size(), p) == 0) return true;
                    }
                    return false;
                }),
                entries_.end()
            );
        }

        // 2. Update existing entries in-place or add new ones
        if (!added.empty()) {
            std::unordered_map<std::wstring, size_t> addMap;
            for (size_t i = 0; i < added.size(); ++i) {
                addMap[added[i].lower()] = i;
            }

            std::vector<bool> consumed(added.size(), false);
            for (Entry& e : entries_) {
                auto it = addMap.find(e.lower());
                if (it != addMap.end()) {
                    e = std::move(added[it->second]);
                    consumed[it->second] = true;
                }
            }

            // Append genuinely new entries
            for (size_t i = 0; i < added.size(); ++i) {
                if (!consumed[i]) {
                    entries_.push_back(std::move(added[i]));
                }
            }
        }

        count_ = static_cast<int64_t>(entries_.size());
    }
    if (onChanged) onChanged();
}

}  // namespace qb
