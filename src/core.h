// QBFind C++ - Core types, utilities, settings, history and crash handling.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace qb {

// Shared directory pool for deduplicating folder paths in RAM (80%+ memory savings).
class DirPool {
public:
    static DirPool& global();

    uint32_t getOrAdd(std::wstring_view dir);
    std::wstring_view get(uint32_t id) const;
    std::wstring_view getLower(uint32_t id) const;
    void finishScan();
    void clear();
    size_t size() const;

    struct Item {
        std::wstring path;
        std::wstring lower;
    };
    std::vector<Item> snapshot() const;

private:
    DirPool() = default;
    mutable std::mutex mu_;
    std::vector<Item> dirs_;
    std::unordered_map<std::wstring, uint32_t> map_;
};

// Ultra-compact indexed file-system entry (minimal memory footprint).
struct Entry {
    uint32_t dirId = 0;      // Directory index in DirPool
    std::wstring name;       // File/folder name
    int64_t size = 0;
    int64_t mtime = 0;       // Unix seconds
    bool isDir = false;
    uint8_t priority = 3;

    std::wstring lowerName() const;
    std::wstring lowerExt() const;       // includes leading '.', e.g. ".pdf", empty if none
    std::wstring path() const;           // Full absolute path reconstructed on demand
    std::wstring lower() const;          // Folded path reconstructed on demand
    std::wstring_view dir() const;       // Parent directory
    std::wstring_view lowerDir() const;  // Folded parent directory
};

Entry makeEntry(const std::wstring& fullPath, int64_t size, int64_t mtime, bool isDir, int priority = 3);
Entry makeEntry(uint32_t dirId, const wchar_t* fileName, int64_t size, int64_t mtime, bool isDir, int priority = 3);

// Lower-cases and folds Turkish characters: ı İ ç ğ ö ş ü â î û -> i i c g o s u a i u.
wchar_t foldChar(wchar_t c);
std::wstring fold(std::wstring_view s);

// '*' and '?' wildcard match (both patterns are assumed folded).
bool wildMatch(std::wstring_view pattern, std::wstring_view text);

std::wstring trim(const std::wstring& s);
std::vector<std::wstring> split(const std::wstring& s, wchar_t sep);

std::string toUtf8(const std::wstring& w);
std::wstring fromUtf8(const std::string& s);

std::wstring formatInt(int64_t v, wchar_t sep = L',');
std::wstring formatSize(int64_t bytes);
std::wstring formatTime(int64_t unixSeconds);

// Known user locations for fast scope filtering and smart relevance ranking.
struct ScopePaths {
    std::wstring desktop;
    std::wstring desktopOriginal;
    std::wstring downloads;
    std::wstring downloadsOriginal;
    std::wstring documents;
    std::wstring documentsOriginal;
    std::wstring pictures;
    std::wstring picturesOriginal;
    std::wstring music;
    std::wstring musicOriginal;
    std::wstring videos;
    std::wstring videosOriginal;
    std::wstring userProfile;
    std::wstring userProfileOriginal;
    std::vector<std::wstring> drives;
    std::vector<std::wstring> drivesOriginal;
};
const ScopePaths& getScopePaths();

// %APPDATA%\QBFind (created on demand).
std::wstring appDataDir();
std::wstring getEnv(const wchar_t* name);

// Thread-safe rotating logger.
void logf(const char* fmt, ...);

// Persistent settings.
struct Settings {
    std::wstring lang;                   // "en" / "tr" / empty = auto
    bool darkMode = false;               // Default false = light/white theme
    bool showToolbar = true;             // Optional toolbar (default: true)
    bool showTabs = false;               // Optional category tabs (default: false)
    bool showBreadcrumb = true;          // Optional breadcrumb path bar (default: true)
    bool scopeOnRight = false;           // Scope selector on the right (default: false)
    std::vector<std::wstring> excludes;  // folded names / globs
};
Settings loadSettings();
bool saveSettings(const Settings& s);

// Search history and saved searches management (%APPDATA%\QBFind\history.txt, saved.txt)
std::vector<std::wstring> loadHistory();
void addHistory(const std::wstring& query);
void clearHistory();

std::vector<std::wstring> loadSavedSearches();
void toggleSavedSearch(const std::wstring& query);

// Setup minidump crash reporter on unhandled SEH exception.
void initCrashReporter();

}  // namespace qb
