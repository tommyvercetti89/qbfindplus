#include "core.h"

#include <windows.h>
#include <shlobj.h>
#include <dbghelp.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <mutex>

namespace qb {

DirPool& DirPool::global() {
    static DirPool instance;
    return instance;
}

uint32_t DirPool::getOrAdd(std::wstring_view dir) {
    if (dir.empty()) return 0;
    std::lock_guard<std::mutex> lk(mu_);
    std::wstring s(dir);
    auto it = map_.find(s);
    if (it != map_.end()) return it->second;
    uint32_t id = static_cast<uint32_t>(dirs_.size());
    dirs_.push_back({s, fold(s)});
    map_.emplace(std::move(s), id);
    return id;
}

std::wstring_view DirPool::get(uint32_t id) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (id < dirs_.size()) return dirs_[id].path;
    return {};
}

std::wstring_view DirPool::getLower(uint32_t id) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (id < dirs_.size()) return dirs_[id].lower;
    return {};
}

void DirPool::finishScan() {
    std::lock_guard<std::mutex> lk(mu_);
    map_.clear();
    dirs_.shrink_to_fit();
}

void DirPool::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    dirs_.clear();
    dirs_.shrink_to_fit();
    map_.clear();
}

size_t DirPool::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return dirs_.size();
}

std::vector<DirPool::Item> DirPool::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return dirs_;
}

std::wstring Entry::lowerName() const {
    return fold(name);
}

std::wstring Entry::lowerExt() const {
    size_t p = name.rfind(L'.');
    if (p == std::wstring::npos) return {};
    return fold(std::wstring_view(name).substr(p));
}

std::wstring_view Entry::dir() const {
    return DirPool::global().get(dirId);
}

std::wstring_view Entry::lowerDir() const {
    return DirPool::global().getLower(dirId);
}

std::wstring Entry::path() const {
    std::wstring_view d = dir();
    if (d.empty()) return name;
    if (d.back() == L'\\' || d.back() == L'/') return std::wstring(d) + name;
    return std::wstring(d) + L'\\' + name;
}

std::wstring Entry::lower() const {
    std::wstring_view d = lowerDir();
    std::wstring ln = lowerName();
    if (d.empty()) return ln;
    if (d.back() == L'\\' || d.back() == L'/') return std::wstring(d) + ln;
    return std::wstring(d) + L'\\' + ln;
}

Entry makeEntry(const std::wstring& fullPath, int64_t size, int64_t mtime, bool isDir, int priority) {
    Entry e;
    size_t p = fullPath.rfind(L'\\');
    std::wstring_view d, n;
    if (p == std::wstring::npos) {
        n = fullPath;
    } else {
        d = std::wstring_view(fullPath).substr(0, p);
        n = std::wstring_view(fullPath).substr(p + 1);
    }
    e.dirId = DirPool::global().getOrAdd(d);
    e.name = std::wstring(n);
    e.size = size;
    e.mtime = mtime;
    e.isDir = isDir;
    e.priority = priority;
    return e;
}

Entry makeEntry(uint32_t dirId, const wchar_t* fileName, int64_t size, int64_t mtime, bool isDir, int priority) {
    Entry e;
    e.dirId = dirId;
    e.name = fileName ? fileName : L"";
    e.size = size;
    e.mtime = mtime;
    e.isDir = isDir;
    e.priority = priority;
    return e;
}

wchar_t foldChar(wchar_t c) {
    if (c < 128) return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
    switch (c) {
        case 0x130: case 0x131: return L'i';
        case 0xC7: case 0xE7: return L'c';
        case 0x11E: case 0x11F: return L'g';
        case 0xD6: case 0xF6: return L'o';
        case 0x15E: case 0x15F: return L's';
        case 0xDC: case 0xFC: return L'u';
        case 0xC2: case 0xE2: return L'a';  // Â, â
        case 0xCE: case 0xEE: return L'i';  // Î, î
        case 0xDB: case 0xFB: return L'u';  // Û, û
    }
    if (c >= 0xD800 && c <= 0xDFFF) return c;  // surrogate halves untouched
    return static_cast<wchar_t>(reinterpret_cast<uintptr_t>(CharLowerW(reinterpret_cast<LPWSTR>(static_cast<uintptr_t>(c)))));
}

std::wstring fold(std::wstring_view s) {
    std::wstring out(s.size(), L'\0');
    for (size_t i = 0; i < s.size(); ++i) out[i] = foldChar(s[i]);
    return out;
}

bool wildMatch(std::wstring_view p, std::wstring_view t) {
    size_t pi = 0, ti = 0, star = std::wstring_view::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == L'?' || p[pi] == t[ti])) {
            ++pi; ++ti;
        } else if (pi < p.size() && p[pi] == L'*') {
            star = pi++;
            mark = ti;
        } else if (star != std::wstring_view::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == L'*') ++pi;
    return pi == p.size();
}

std::wstring trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) ++b;
    while (e > b && iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::vector<std::wstring> split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        out.push_back(s.substr(start, p == std::wstring::npos ? p : p - start));
        if (p == std::wstring::npos) break;
        start = p + 1;
    }
    return out;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring formatInt(int64_t v, wchar_t sep) {
    uint64_t uv = (v < 0) ? 0ULL - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
    std::wstring digits = std::to_wstring(uv);
    std::wstring out;
    out.reserve(digits.size() + digits.size() / 3 + 2);
    int c = 0;
    for (size_t i = digits.size(); i-- > 0;) {
        out.insert(out.begin(), digits[i]);
        if (++c % 3 == 0 && i > 0) out.insert(out.begin(), sep);
    }
    return v < 0 ? L"-" + out : out;
}

std::wstring formatSize(int64_t b) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = (double)b;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    wchar_t buf[64];
    if (u == 0) swprintf(buf, 64, L"%lld B", (long long)b);
    else swprintf(buf, 64, L"%.1f %ls", v, units[u]);
    return buf;
}

std::wstring formatTime(int64_t t) {
    if (t <= 0) return L"";
    time_t tt = (time_t)t;
    std::tm tmv{};
    if (localtime_s(&tmv, &tt) != 0) return L"";
    wchar_t buf[32];
    swprintf(buf, 32, L"%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    return buf;
}

std::wstring getEnv(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring v(n, L'\0');
    DWORD got = GetEnvironmentVariableW(name, v.data(), n);
    v.resize(got);
    return v;
}

static std::wstring getRegistryUserFolder(const wchar_t* valueName) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD bufSize = sizeof(buf);
    DWORD type = 0;
    std::wstring out;
    if (RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &bufSize) == ERROR_SUCCESS) {
        wchar_t expanded[MAX_PATH * 2] = {};
        if (ExpandEnvironmentStringsW(buf, expanded, MAX_PATH * 2) > 0) {
            out = expanded;
        } else {
            out = buf;
        }
    }
    RegCloseKey(hKey);
    return out;
}

static std::wstring queryKnownFolder(const KNOWNFOLDERID& id, const wchar_t* regFallback1 = nullptr, const wchar_t* regFallback2 = nullptr) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    PWSTR out = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &out)) && out) {
        r = out;
        CoTaskMemFree(out);
    }
    if (r.empty() && regFallback1) {
        r = getRegistryUserFolder(regFallback1);
    }
    if (r.empty() && regFallback2) {
        r = getRegistryUserFolder(regFallback2);
    }
    while (r.size() > 3 && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    return r;
}

const ScopePaths& getScopePaths() {
    static ScopePaths paths = [] {
        ScopePaths sp;
        sp.desktopOriginal = queryKnownFolder(FOLDERID_Desktop, L"Desktop");
        sp.desktop = fold(sp.desktopOriginal);

        sp.downloadsOriginal = queryKnownFolder(FOLDERID_Downloads, L"{374DE290-123F-4565-9164-39C4925E467B}", L"{7D83EE9B-2244-4E70-B1F5-5393042AF1E4}");
        if (sp.downloadsOriginal.empty()) sp.downloadsOriginal = getRegistryUserFolder(L"{374DE290-123F-4565-9164-39C4925E467B}");
        sp.downloads = fold(sp.downloadsOriginal);

        sp.documentsOriginal = queryKnownFolder(FOLDERID_Documents, L"Personal", L"{F42EE2D3-909F-4907-8871-4C22FC0BF756}");
        sp.documents = fold(sp.documentsOriginal);

        sp.picturesOriginal = queryKnownFolder(FOLDERID_Pictures, L"My Pictures");
        sp.pictures = fold(sp.picturesOriginal);

        sp.musicOriginal = queryKnownFolder(FOLDERID_Music, L"My Music");
        sp.music = fold(sp.musicOriginal);

        sp.videosOriginal = queryKnownFolder(FOLDERID_Videos, L"My Video");
        sp.videos = fold(sp.videosOriginal);

        sp.userProfileOriginal = getEnv(L"USERPROFILE");
        sp.userProfile = fold(sp.userProfileOriginal);

        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(mask & (1u << i))) continue;
            std::wstring rootOrig = {static_cast<wchar_t>(L'A' + i), L':', L'\\'};
            sp.drivesOriginal.push_back(rootOrig);
            sp.drives.push_back(fold(rootOrig));
        }
        return sp;
    }();
    return paths;
}

std::wstring appDataDir() {
    std::wstring base = getEnv(L"APPDATA");
    if (base.empty()) return {};
    std::wstring dir = base + L"\\QBFind";
    CreateDirectoryW(dir.c_str(), nullptr);
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return {};
    return dir;
}

void logf(const char* fmt, ...) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    std::wstring dir = appDataDir();
    if (dir.empty()) return;
    std::wstring path = dir + L"\\qbfind.log";
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) && fad.nFileSizeLow > 1024 * 1024) {
        MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || !f) return;
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &now);
    std::fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d ", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}

static std::wstring settingsPath() {
    std::wstring d = appDataDir();
    return d.empty() ? std::wstring() : d + L"\\settings.txt";
}

Settings loadSettings() {
    Settings s;
    std::wstring path = settingsPath();
    if (path.empty()) return s;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return s;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && data.size() < (1u << 20)) data.append(buf, n);
    std::fclose(f);
    if (data.size() >= 3 && (uint8_t)data[0] == 0xEF && (uint8_t)data[1] == 0xBB && (uint8_t)data[2] == 0xBF) data.erase(0, 3);
    for (const std::wstring& line : split(fromUtf8(data), L'\n')) {
        std::wstring l = trim(line);
        size_t eq = l.find(L'=');
        if (l.empty() || eq == std::wstring::npos) continue;
        std::wstring key = trim(l.substr(0, eq)), val = trim(l.substr(eq + 1));
        if (key == L"lang") {
            s.lang = val;
        } else if (key == L"dark") {
            s.darkMode = (val == L"true" || val == L"1");
        } else if (key == L"toolbar") {
            s.showToolbar = (val == L"true" || val == L"1");
        } else if (key == L"tabs") {
            s.showTabs = (val == L"true" || val == L"1");
        } else if (key == L"breadcrumb") {
            s.showBreadcrumb = (val == L"true" || val == L"1");
        } else if (key == L"scoperight") {
            s.scopeOnRight = (val == L"true" || val == L"1");
        } else if (key == L"exclude") {
            for (const std::wstring& part : split(val, L';')) {
                std::wstring p = fold(trim(part));
                if (!p.empty()) s.excludes.push_back(p);
            }
        }
    }
    return s;
}

bool saveSettings(const Settings& s) {
    std::wstring path = settingsPath();
    if (path.empty()) return false;
    std::wstring ex;
    for (size_t i = 0; i < s.excludes.size(); ++i) ex += (i ? L";" : L"") + s.excludes[i];
    std::wstring content = L"lang=" + s.lang +
                           L"\ndark=" + (s.darkMode ? L"true" : L"false") +
                           L"\ntoolbar=" + (s.showToolbar ? L"true" : L"false") +
                           L"\ntabs=" + (s.showTabs ? L"true" : L"false") +
                           L"\nbreadcrumb=" + (s.showBreadcrumb ? L"true" : L"false") +
                           L"\nscoperight=" + (s.scopeOnRight ? L"true" : L"false") +
                           L"\nexclude=" + ex + L"\n";
    std::string data = toUtf8(content);
    std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// ------------------------------------------------------------- History / Saved
static std::vector<std::wstring> readLinesFile(const std::wstring& filename) {
    std::vector<std::wstring> out;
    std::wstring dir = appDataDir();
    if (dir.empty()) return out;
    std::wstring path = dir + L"\\" + filename;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return out;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && data.size() < (512u * 1024)) data.append(buf, n);
    std::fclose(f);
    if (data.size() >= 3 && (uint8_t)data[0] == 0xEF && (uint8_t)data[1] == 0xBB && (uint8_t)data[2] == 0xBF) data.erase(0, 3);
    for (const auto& line : split(fromUtf8(data), L'\n')) {
        std::wstring t = trim(line);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

static void writeLinesFile(const std::wstring& filename, const std::vector<std::wstring>& lines) {
    std::wstring dir = appDataDir();
    if (dir.empty()) return;
    std::wstring path = dir + L"\\" + filename;
    std::wstring content;
    for (const auto& l : lines) content += l + L"\n";
    std::string data = toUtf8(content);
    std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return;
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::vector<std::wstring> loadHistory() {
    return readLinesFile(L"history.txt");
}

void addHistory(const std::wstring& q) {
    std::wstring t = trim(q);
    if (t.size() < 2) return;
    auto history = loadHistory();
    // remove duplicate if exists
    history.erase(std::remove_if(history.begin(), history.end(), [&](const std::wstring& item) {
        return _wcsicmp(item.c_str(), t.c_str()) == 0;
    }), history.end());
    history.insert(history.begin(), t);
    if (history.size() > 20) history.resize(20);
    writeLinesFile(L"history.txt", history);
}

void clearHistory() {
    writeLinesFile(L"history.txt", {});
}

std::vector<std::wstring> loadSavedSearches() {
    return readLinesFile(L"saved.txt");
}

void toggleSavedSearch(const std::wstring& q) {
    std::wstring t = trim(q);
    if (t.empty()) return;
    auto saved = loadSavedSearches();
    auto it = std::find_if(saved.begin(), saved.end(), [&](const std::wstring& item) {
        return _wcsicmp(item.c_str(), t.c_str()) == 0;
    });
    if (it != saved.end()) {
        saved.erase(it);
    } else {
        saved.insert(saved.begin(), t);
    }
    writeLinesFile(L"saved.txt", saved);
}

// ------------------------------------------------------------- Crash Reporter
typedef BOOL(WINAPI* MiniDumpWriteDumpFn)(
    HANDLE hProcess, DWORD ProcessId, HANDLE hFile,
    MINIDUMP_TYPE DumpType,
    PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
    PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
    PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

static LONG WINAPI CustomUnhandledExceptionFilter(EXCEPTION_POINTERS* ep) {
    logf("FATAL: Unhandled exception code 0x%08lx at address %p",
         ep ? ep->ExceptionRecord->ExceptionCode : 0,
         ep ? ep->ExceptionRecord->ExceptionAddress : nullptr);

    HMODULE hDbg = LoadLibraryW(L"dbghelp.dll");
    if (hDbg) {
        auto pWrite = reinterpret_cast<MiniDumpWriteDumpFn>(reinterpret_cast<void*>(GetProcAddress(hDbg, "MiniDumpWriteDump")));
        if (pWrite) {
            std::wstring dir = appDataDir();
            std::wstring dmpPath = dir.empty() ? L"qbfind_crash.dmp" : (dir + L"\\qbfind_crash.dmp");
            HANDLE hFile = CreateFileW(dmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei;
                mei.ThreadId = GetCurrentThreadId();
                mei.ExceptionPointers = ep;
                mei.ClientPointers = FALSE;
                pWrite(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &mei, nullptr, nullptr);
                CloseHandle(hFile);
                logf("Minidump written to %ls", dmpPath.c_str());
            }
        }
        FreeLibrary(hDbg);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void initCrashReporter() {
    SetUnhandledExceptionFilter(CustomUnhandledExceptionFilter);
}

}  // namespace qb
