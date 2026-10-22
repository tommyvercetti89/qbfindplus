#include "core.h"

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <mutex>

namespace qb {

std::wstring_view Entry::lowerExt() const {
    std::wstring_view n = lowerName();
    size_t p = n.rfind(L'.');
    if (p == std::wstring_view::npos) return {};
    return n.substr(p);
}

Entry makeEntry(std::wstring path, int64_t size, int64_t mtime, bool isDir, int priority) {
    Entry e;
    e.lower = fold(path);
    size_t p = path.rfind(L'\\');
    e.nameOff = (p == std::wstring::npos) ? 0 : static_cast<uint32_t>(p + 1);
    e.path = std::move(path);
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
    std::wstring digits = std::to_wstring(v < 0 ? -v : v);
    std::wstring out;
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
    std::wstring content = L"lang=" + s.lang + L"\ndark=" + (s.darkMode ? L"true" : L"false") + L"\nexclude=" + ex + L"\n";
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
