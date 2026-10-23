// QBFind C++ - query parser, matcher and ranking (no UI dependencies).
#pragma once

#include <atomic>
#include <ctime>
#include <regex>
#include <string>
#include <vector>

#include "core.h"

namespace qb {

enum ScopeType {
    SCOPE_ALL = 0,
    SCOPE_DESKTOP,
    SCOPE_DOWNLOADS,
    SCOPE_DOCUMENTS,
    SCOPE_USER_AREA,    // Desktop + Downloads + Documents
    SCOPE_PICTURES,
    SCOPE_MUSIC,
    SCOPE_VIDEOS,
    SCOPE_DRIVE_C,
    SCOPE_DRIVE_D,
    SCOPE_COUNT
};

enum FileCategory {
    CAT_ALL = 0,
    CAT_DOCS,
    CAT_IMAGES,
    CAT_AUDIO,
    CAT_VIDEO,
    CAT_ARCHIVES,
    CAT_APPS,
    CAT_FOLDERS,
    CAT_COUNT
};

struct Query {
    ScopeType scope = SCOPE_ALL;
    FileCategory category = CAT_ALL;
    std::wstring browseDir;
    bool browseDirectOnly = false;
    std::vector<std::wstring> tokens;
    std::vector<std::vector<std::wstring>> alternates;
    std::vector<std::wstring> names, paths, folders, exts, globs, fuzzy;
    std::vector<std::wregex> nameRegexes, pathRegexes;
    std::wstring phrase;
    int64_t sizeMin = -1, sizeMax = -1, dateMin = -1, dateMax = -1;

    bool emptyTokens() const {
        return tokens.empty() && alternates.empty() && names.empty() && paths.empty() &&
               folders.empty() && exts.empty() && globs.empty() && fuzzy.empty() &&
               nameRegexes.empty() && pathRegexes.empty() && phrase.empty() &&
               sizeMin < 0 && sizeMax < 0 && dateMin < 0 && dateMax < 0;
    }

    bool empty() const {
        return !browseDirectOnly && scope == SCOPE_ALL && category == CAT_ALL && emptyTokens();
    }
};

Query parseQuery(const std::wstring& text, std::time_t now = std::time(nullptr));
bool parseSize(const std::wstring& s, int64_t& out);
bool parseSizeRange(const std::wstring& s, int64_t& mn, int64_t& mx);
bool parseDateRange(const std::wstring& s, std::time_t now, int64_t& mn, int64_t& mx);

bool matchesScope(const Entry& e, ScopeType scope);
bool matchesCategory(const Entry& e, FileCategory cat);
bool matches(const Entry& e, const Query& q);
int score(const Entry& e, const Query& q);

// Returns indexes into `entries` of the best `limit` matches, best first.
// Aborts and returns false if `cancel` becomes true.
bool searchEntries(const std::vector<Entry>& entries, const Query& q, size_t limit,
                   const std::atomic<bool>& cancel, std::vector<size_t>& out);

}  // namespace qb
