// QBFind C++ - query parser, matcher and ranking (no UI dependencies).
#pragma once

#include <atomic>
#include <ctime>
#include <regex>
#include <string>
#include <vector>

#include "core.h"

namespace qb {

struct Query {
    std::vector<std::wstring> tokens;
    std::vector<std::vector<std::wstring>> alternates;
    std::vector<std::wstring> names, paths, folders, exts, globs, fuzzy;
    std::vector<std::wregex> nameRegexes, pathRegexes;
    std::wstring phrase;
    int64_t sizeMin = -1, sizeMax = -1, dateMin = -1, dateMax = -1;

    bool empty() const {
        return tokens.empty() && alternates.empty() && names.empty() && paths.empty() && folders.empty() &&
               exts.empty() && globs.empty() && fuzzy.empty() && nameRegexes.empty() && pathRegexes.empty() &&
               sizeMin < 0 && sizeMax < 0 && dateMin < 0 && dateMax < 0;
    }
};

Query parseQuery(const std::wstring& text, std::time_t now = std::time(nullptr));
bool parseSize(const std::wstring& s, int64_t& out);
bool parseSizeRange(const std::wstring& s, int64_t& mn, int64_t& mx);
bool parseDateRange(const std::wstring& s, std::time_t now, int64_t& mn, int64_t& mx);

bool matches(const Entry& e, const Query& q);
int score(const Entry& e, const Query& q);

// Returns indexes into `entries` of the best `limit` matches, best first.
// Aborts and returns false if `cancel` becomes true.
bool searchEntries(const std::vector<Entry>& entries, const Query& q, size_t limit,
                   const std::atomic<bool>& cancel, std::vector<size_t>& out);

}  // namespace qb
