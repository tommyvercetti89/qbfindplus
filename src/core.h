// QBFind C++ - Core types, utilities, settings, history and crash handling.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace qb {

// One indexed file-system entry.
struct Entry {
    std::wstring path;
    std::wstring lower;     // search-folded copy of path
    uint32_t nameOff = 0;   // offset where file/folder name starts
    int64_t size = 0;
    int64_t mtime = 0;      // unix seconds
    bool isDir = false;
    int priority = 3;

    std::wstring_view name() const { return std::wstring_view(path).substr(nameOff); }
    std::wstring_view lowerName() const { return std::wstring_view(lower).substr(nameOff); }
    std::wstring_view lowerExt() const;  // includes leading '.', e.g. ".pdf", empty if none
};

Entry makeEntry(std::wstring path, int64_t size, int64_t mtime, bool isDir, int priority = 3);

// Lower-cases and folds Turkish characters: ı İ ç ğ ö ş ü -> i i c g o s u.
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

// %APPDATA%\QBFind (created on demand).
std::wstring appDataDir();
std::wstring getEnv(const wchar_t* name);

// Thread-safe rotating logger.
void logf(const char* fmt, ...);

// Persistent settings.
struct Settings {
    std::wstring lang;                   // "en" / "tr" / empty = auto
    bool darkMode = false;
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
